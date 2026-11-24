// Copyright RIME Developers
// Distributed under the BSD License
//
// ai_sync_processor.h
//
// Main-thread gate for the AI worker's composition refresh.
//
// librime is not thread-safe: the AI worker thread pops + recomposes the
// composition to surface a finished prediction (refresh_composition), and
// that MUST NOT overlap with main-thread key processing. A key that arrives
// mid-refresh used to hit a Context mid-mutation and get swallowed (the
// "25+ chars of context => keys disappear" report).
//
// This processor sits FIRST in the engine's processor chain. Every key event
// passes through it before speller/segmentor run; while the worker is inside
// its refresh critical section the processor spins (capped) so the key is
// processed strictly after the refresh completes. The key is delayed by the
// refresh duration (~a few ms of Compose work), never dropped.
//
// Residual window: between the worker passing its final pending check and
// setting the flag there is a microseconds-wide gap a key could slip through;
// the worker's own composition-changed recheck afterwards turns any such
// overlap into a skipped refresh instead of a corruption.

#ifndef RIME_AI_PREDICT_AI_SYNC_PROCESSOR_H_
#define RIME_AI_PREDICT_AI_SYNC_PROCESSOR_H_

#include <rime/processor.h>

#include <atomic>

namespace rime {
namespace predict {

// Set while the AI worker thread is mutating Context (composition refresh).
// Defined in prediction_engine.cc.
extern std::atomic<bool> g_refresh_in_progress;

class AISyncProcessor : public Processor {
 public:
  explicit AISyncProcessor(const Ticket& ticket) : Processor(ticket) {}

  ProcessResult ProcessKeyEvent(const KeyEvent& key) override;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_AI_PREDICT_AI_SYNC_PROCESSOR_H_
