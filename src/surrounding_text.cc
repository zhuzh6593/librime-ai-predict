// Copyright RIME Developers
// Distributed under the BSD License

#include "surrounding_text.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <chrono>
#include <cstdint>
#include <string>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace rime {
namespace predict {

namespace {

#if defined(__APPLE__)

// Cache the TCC answer: AXIsProcessTrusted() costs a round trip to tccd and we
// would otherwise pay it on every keystroke's prediction attempt. The user can
// grant permission at any time, so we re-check at most once every 10s.
bool g_permitted = false;
bool g_permitted_known = false;
int64_t g_last_check_ms = 0;

int64_t NowMs() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

CFTypeRef CopyAttribute(AXUIElementRef element, CFStringRef name) {
  CFTypeRef value = nullptr;
  if (!element || !name)
    return nullptr;
  if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess)
    return nullptr;
  return value;
}

// Convert a CFString to UTF-8. Callers pass an already-truncated substring; the
// final code-point trim happens once, after whitespace normalization.
string CfStringToUtf8(CFStringRef s) {
  if (!s)
    return {};
  CFIndex len = CFStringGetLength(s);
  if (len <= 0)
    return {};
  CFIndex bytes_needed =
      CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string buffer(static_cast<size_t>(bytes_needed), '\0');
  if (!CFStringGetCString(s, &buffer[0], bytes_needed, kCFStringEncodingUTF8))
    return {};
  buffer.erase(buffer.find('\0'));
  return buffer;
}

#endif  // __APPLE__

// Drop trailing ASCII letters/underscore/apostrophe: during a composition the
// text field's value may already contain the in-flight pinyin as marked text,
// and the model must receive the pinyin through the prompt channel instead.
void StripTrailingPrompt(const string& in, string* out) {
  size_t end = in.size();
  while (end > 0) {
    char c = in[end - 1];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' ||
        c == '_') {
      --end;
      continue;
    }
    break;
  }
  out->assign(in, 0, end);
}

// Trim whitespace/newlines at both edges and collapse inner newlines, so the
// context window reads as one line ("\n" in the middle disturbs the model).
void NormalizeWhitespace(const string& in, string* out) {
  size_t begin = 0;
  size_t end = in.size();
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (begin < end && is_space(in[begin]))
    ++begin;
  while (end > begin && is_space(in[end - 1]))
    --end;
  out->clear();
  out->reserve(end - begin);
  bool last_space = false;
  for (size_t i = begin; i < end; ++i) {
    char c = in[i];
    if (is_space(c)) {
      if (!last_space)
        out->push_back(' ');
      last_space = true;
    } else {
      out->push_back(c);
      last_space = false;
    }
  }
}

}  // namespace

const char* SurroundingTextPlatform() {
#if defined(__APPLE__)
  return "macos";
#else
  return "unsupported";
#endif
}

#if defined(__APPLE__)
namespace {

// Ask macOS to show the "…wants to control this computer" dialog. Once per
// process, and only when we are not trusted yet: the ad-hoc re-signature that
// every local rebuild needs drops the existing grant, and hunting for the
// Accessibility pane by hand is the worst part of the loop.
void PromptForAccessibilityOnce() {
  static bool prompted = false;
  if (prompted)
    return;
  prompted = true;
  const void* keys[] = {kAXTrustedCheckOptionPrompt};
  const void* values[] = {kCFBooleanTrue};
  CFDictionaryRef options =
      CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                         &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  if (!options)
    return;
  AXIsProcessTrustedWithOptions(options);
  CFRelease(options);
}

}  // namespace
#endif

bool SurroundingTextPermitted() {
#if defined(__APPLE__)
  int64_t now = NowMs();
  if (!g_permitted_known || now - g_last_check_ms > 10000) {
    g_permitted = AXIsProcessTrusted() ? true : false;
    if (!g_permitted) {
      PromptForAccessibilityOnce();
    }
    g_permitted_known = true;
    g_last_check_ms = now;
  }
  return g_permitted;
#else
  return false;
#endif
}

namespace {

// Privacy: password / OTP fields must never reach the model. macOS exposes
// secure NSTextFields with the AXSecureTextField subrole; WebKit/Chromium
// fields often lack it but mask the value as bullets or asterisks.
bool LooksLikeMaskedValue(CFStringRef s) {
  CFIndex len = CFStringGetLength(s);
  if (len == 0)
    return false;
  for (CFIndex i = 0; i < len; ++i) {
    UniChar c = CFStringGetCharacterAtIndex(s, i);
    if (c != 0x2022 && c != 0x00B7 && c != 0x25CF && c != '*')
      return false;
  }
  return true;
}

bool IsSecureTextField(AXUIElementRef element) {
  static const CFStringRef kSecureSubrole = CFSTR("AXSecureTextField");
  CFTypeRef subrole = CopyAttribute(element, kAXSubroleAttribute);
  if (subrole) {
    if (CFGetTypeID(subrole) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(subrole), kSecureSubrole,
                        kCFCompareCaseInsensitive) == 0) {
      CFRelease(subrole);
      return true;
    }
    CFRelease(subrole);
  }
  CFTypeRef role_desc = CopyAttribute(element, kAXRoleDescriptionAttribute);
  if (role_desc) {
    if (CFGetTypeID(role_desc) == CFStringGetTypeID()) {
      CFStringRef rd = static_cast<CFStringRef>(role_desc);
      if (CFStringFind(rd, CFSTR("password"), kCFCompareCaseInsensitive)
                  .location != kCFNotFound ||
          CFStringFind(rd, CFSTR("secure"), kCFCompareCaseInsensitive)
                  .location != kCFNotFound) {
        CFRelease(role_desc);
        return true;
      }
    }
    CFRelease(role_desc);
  }
  return false;
}

}  // namespace

// Blocking AX read. Runs on the refresh worker, never on a keystroke path.
static string ReadSurroundingTextBlocking(int max_chars, int timeout_ms) {
#if defined(__APPLE__)
  if (max_chars <= 0 || !SurroundingTextPermitted())
    return {};

  AXUIElementRef system_wide = AXUIElementCreateSystemWide();
  if (!system_wide)
    return {};
  CFTypeRef focused = nullptr;
  AXError err = AXUIElementCopyAttributeValue(
      system_wide, kAXFocusedUIElementAttribute, &focused);
  CFRelease(system_wide);
  if (err != kAXErrorSuccess || !focused)
    return {};

  AXUIElementRef element = static_cast<AXUIElementRef>(focused);
  // A hung target app must never stall the prediction worker.
  float timeout_s = static_cast<float>(timeout_ms > 0 ? timeout_ms : 200) / 1000.0f;
  AXUIElementSetMessagingTimeout(element, timeout_s);

  // Privacy gate: secure fields yield nothing to the model.
  if (IsSecureTextField(element)) {
    CFRelease(element);
    return {};
  }

  string raw;

  // Primary path: full value + caret offset, both available on native AppKit
  // text views (NSTextView, NSTextField, SwiftUI TextEditor).
  CFRange caret{-1, 0};
  CFTypeRef selected_range = CopyAttribute(element, kAXSelectedTextRangeAttribute);
  if (selected_range) {
    if (CFGetTypeID(selected_range) == AXValueGetTypeID()) {
      AXValueGetValue(static_cast<AXValueRef>(selected_range),
                      kAXValueTypeCFRange, &caret);
    }
    CFRelease(selected_range);
  }

  CFTypeRef value = CopyAttribute(element, kAXValueAttribute);
  if (value) {
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
      CFStringRef str = static_cast<CFStringRef>(value);
      if (LooksLikeMaskedValue(str)) {
        // The "value" is a mask (bullets/asterisks): a password field that
        // skipped the subrole check. Discard everything.
        CFRelease(value);
        CFRelease(element);
        return {};
      }
      CFIndex len = CFStringGetLength(str);
      CFIndex end = (caret.location >= 0 && caret.location <= len)
                        ? caret.location
                        : len;
      CFStringRef prefix = CFStringCreateWithSubstring(
          nullptr, str, CFRangeMake(0, end));
      if (prefix) {
        raw = CfStringToUtf8(prefix);
        CFRelease(prefix);
      }
    }
    CFRelease(value);
  }

  // Fallback path: WebKit/Chromium fields often expose no AXValue but do serve
  // AXStringForRange. Ask for the range before the caret.
  if (raw.empty() && caret.location >= 0) {
    CFRange before{caret.location > max_chars ? caret.location - max_chars : 0,
                   caret.location > max_chars ? max_chars : caret.location};
    if (before.length > 0) {
      AXValueRef range_value = AXValueCreate(kAXValueTypeCFRange, &before);
      if (range_value) {
        CFTypeRef piece = nullptr;
        if (AXUIElementCopyParameterizedAttributeValue(
                element, kAXStringForRangeParameterizedAttribute, range_value,
                &piece) == kAXErrorSuccess &&
            piece) {
          if (CFGetTypeID(piece) == CFStringGetTypeID()) {
            raw = CfStringToUtf8(static_cast<CFStringRef>(piece));
          }
          CFRelease(piece);
        }
        CFRelease(range_value);
      }
    }
  }

  CFRelease(element);

  if (raw.empty())
    return {};

  string stripped;
  StripTrailingPrompt(raw, &stripped);
  string normalized;
  NormalizeWhitespace(stripped, &normalized);

  // Keep only the last `max_chars` UTF-8 code points.
  size_t count = 0;
  size_t cut = normalized.size();
  for (size_t i = normalized.size(); i > 0;) {
    --i;
    if ((static_cast<unsigned char>(normalized[i]) & 0xC0) != 0x80) {
      if (++count > static_cast<size_t>(max_chars)) {
        cut = i + 1;
        break;
      }
    }
  }
  if (cut < normalized.size())
    normalized.erase(0, cut);
  return normalized;
#else
  (void)max_chars;
  (void)timeout_ms;
  return {};
#endif
}

#if defined(__APPLE__)
namespace {

// How often the worker is allowed to touch AX while the field is changing.
constexpr int64_t kRefreshIntervalMs = 250;
// A snapshot older than this is dropped: after a pause we would rather fall
// back to commit history than predict from text the user has since replaced.
constexpr int64_t kMaxSnapshotAgeMs = 1500;

struct SnapshotState {
  std::mutex m;
  std::condition_variable cv;
  bool request = false;
  bool stop = false;
  bool worker_started = false;
  string text;
  int64_t text_ms = 0;
  int max_chars = 0;
  int timeout_ms = 200;
};

SnapshotState& Snapshot() {
  static SnapshotState s;
  return s;
}

void RefreshWorker() {
  SnapshotState& s = Snapshot();
  for (;;) {
    int max_chars = 0;
    int timeout_ms = 200;
    {
      std::unique_lock<std::mutex> lk(s.m);
      s.cv.wait(lk, [&s] { return s.stop || s.request; });
      if (s.stop)
        return;
      s.request = false;
      max_chars = s.max_chars;
      timeout_ms = s.timeout_ms;
    }
    string text = ReadSurroundingTextBlocking(max_chars, timeout_ms);
    {
      std::lock_guard<std::mutex> lk(s.m);
      s.text = text;
      s.text_ms = NowMs();
    }
  }
}

}  // namespace

string ReadSurroundingText(int max_chars, int timeout_ms) {
  // AX is cross-process IPC: a hung or very busy target app can stall it for
  // far longer than a keystroke may take. Reading it inline used to make every
  // key press wait on the frontmost app, so the actual read happens on a single
  // background worker and callers only ever copy the last snapshot.
  if (max_chars <= 0)
    return {};

  SnapshotState& s = Snapshot();
  std::lock_guard<std::mutex> lk(s.m);
  if (!s.worker_started) {
    s.worker_started = true;
    std::thread(RefreshWorker).detach();
  }
  s.max_chars = max_chars;
  s.timeout_ms = timeout_ms > 0 ? timeout_ms : 200;

  int64_t now = NowMs();
  if (now - s.text_ms > kRefreshIntervalMs && !s.request) {
    s.request = true;
    s.cv.notify_one();
  }

  if (now - s.text_ms > kMaxSnapshotAgeMs)
    return {};
  return s.text;
}

#else

string ReadSurroundingText(int max_chars, int timeout_ms) {
  (void)max_chars;
  (void)timeout_ms;
  return {};
}

#endif

}  // namespace predict
}  // namespace rime
