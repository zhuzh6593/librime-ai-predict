// Read the focused text field's text preceding the caret via macOS
// Accessibility (AX) API.
//
// Why: Rime's built-in context is commit_history(), which only knows text the
// user typed *through this IME*. Pasted text, text typed with another IME, and
// text the caret moved into are all invisible. The surrounding-text reader
// closes that gap so the CT2 model sees the real preceding sentence.
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_SURROUNDING_TEXT_H_
#define RIME_PREDICT_SURROUNDING_TEXT_H_

#include <rime/common.h>

namespace rime {
namespace predict {

/// Text immediately before the caret in the focused text field, at most
/// `max_chars` UTF-8 characters, with trailing ASCII letters (an in-flight
/// pinyin preedit) and edge whitespace stripped.
///
/// Returns an empty string when the reader is unavailable: no Accessibility
/// permission, the focused app exposes no AX value (common in some
/// Electron/WebKit fields), or the AX call exceeds `timeout_ms`.
///
/// `composition_hint`: the IME's currently displayed preedit (marked) text.
/// macOS inline preedit lives INSIDE the field's AXValue, so without this the
/// unconfirmed composition ("今tian") pollutes every snapshot and the model's
/// context/cache key churns on every keystroke. When the snapshot's tail
/// matches the hint, that tail is removed first.
///
/// Reads only the text before the caret; never writes anything to disk.
string ReadSurroundingText(int max_chars, int timeout_ms,
                           const string& composition_hint = {});

/// True when this process may use the AX API at all. Cached after the first
/// call because AXIsProcessTrusted() hits TCC every time.
bool SurroundingTextPermitted();

/// Platform string for logs: "macos" or "unsupported".
const char* SurroundingTextPlatform();

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_SURROUNDING_TEXT_H_
