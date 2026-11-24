// Copyright RIME Developers
// Distributed under the BSD License
//
// ai_sync_processor.cc

#include "ai_sync_processor.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace rime {
namespace predict {

std::atomic<bool> g_refresh_in_progress{false};

ProcessResult AISyncProcessor::ProcessKeyEvent(const KeyEvent& key) {
  // Spin until the worker's refresh critical section ends (if any). The cap
  // keeps a wedged worker from freezing the keyboard: after ~30ms we proceed
  // anyway -- the worker's own recheck then sees the changed composition and
  // aborts the refresh.
  int spins = 0;
  while (g_refresh_in_progress.load(std::memory_order_acquire)) {
    if (++spins > 300) {  // 300 x 100us = ~30ms
      break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return kNoop;
}

}  // namespace predict
}  // namespace rime
