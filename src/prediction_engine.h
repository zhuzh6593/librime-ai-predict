// Async prediction with debounce + cache; notifies Rime context on completion.
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_PREDICTION_ENGINE_H_
#define RIME_PREDICT_PREDICTION_ENGINE_H_

#include "context_builder.h"
#include "inference_backend.h"  // Prediction

#include <rime/common.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rime {

class Context;
class Engine;

namespace predict {

struct PredictionEngineOptions {
  int debounce_ms = 200;
  int max_tokens = 256;
  /// >1 enables beam hypotheses (backend widens the beam accordingly).
  int num_hypotheses = 1;
  /// When true, finished inferences call Context::RefreshNonConfirmedComposition
  /// so the AI row appears during the pause instead of never (a keypress
  /// changes the prompt, so the next Query always cache-MISSes -- cache-only
  /// mode effectively hides AI candidates). Guarded by refresh_idle_ms so the
  /// refresh only fires when the user has actually stopped typing.
  bool refresh_composition = true;
  /// Idle window after a finished inference: if any new request arrives
  /// within it (i.e. the user kept typing), the refresh is skipped and the
  /// newer inference handles the surface instead. This is what keeps the
  /// refresh from firing on every keystroke and racing the main thread.
  int refresh_idle_ms = 400;
};

class PredictionEngine {
 public:
  PredictionEngine(Engine* engine,
                   std::unique_ptr<InferenceBackend> backend,
                   const PredictionEngineOptions& opt);
  ~PredictionEngine();

  InferenceBackend* backend() { return backend_.get(); }

  /// Enqueue CT2 inference; returns immediately. On completion, notifies context.
  void Schedule(const PredictionContext& ctx);

  /// Last completed predictions keyed by `PredictionContext::cache_key`
  /// (which combines window_text and effective_prompt to avoid
  /// cross-context pollution). Sorted by descending score; empty vector
  /// inside the optional means "ran, produced nothing usable".
  std::optional<std::vector<Prediction>> GetCachedResult(
      const std::string& cache_key) const;

  void ClearCache();

 private:
  void WorkerLoop();

  Engine* engine_;
  std::unique_ptr<InferenceBackend> backend_;
  PredictionEngineOptions opt_;

  std::thread worker_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool shutdown_ = false;

  std::string pending_ct2_input_;
  std::string pending_cache_key_;
  // Pure pinyin (effective_prompt) of the in-flight request. Kept separately
  // from cache_key so that the post-inference "did the composition still
  // match?" check compares against ctx->input() (which is also pure pinyin),
  // not against the cache_key (which carries the window_text prefix).
  std::string pending_prompt_;

  mutable std::mutex result_mutex_;
  std::string last_cache_key_;
  std::optional<std::vector<Prediction>> last_result_;
  /// Key the composition refresh last fired for -- one refresh per result,
  /// even if GetCachedResult is queried again later.
  std::string last_refreshed_key_;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_PREDICTION_ENGINE_H_
