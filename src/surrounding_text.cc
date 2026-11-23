// Copyright RIME Developers
// Distributed under the BSD License

#include "surrounding_text.h"

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

bool SurroundingTextPermitted() {
#if defined(__APPLE__)
  int64_t now = NowMs();
  if (!g_permitted_known || now - g_last_check_ms > 10000) {
    g_permitted = AXIsProcessTrusted() ? true : false;
    g_permitted_known = true;
    g_last_check_ms = now;
  }
  return g_permitted;
#else
  return false;
#endif
}

string ReadSurroundingText(int max_chars, int timeout_ms) {
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

}  // namespace predict
}  // namespace rime
