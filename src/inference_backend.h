// Copyright RIME Developers
// Distributed under the BSD License
//
// Abstract inference backend for librime-ai-predict (CT2 now, llama.cpp later).

#ifndef RIME_PREDICT_INFERENCE_BACKEND_H_
#define RIME_PREDICT_INFERENCE_BACKEND_H_

#include <memory>
#include <string>
#include <vector>

namespace rime {
namespace predict {

struct InferenceBackendConfig {
  std::string model_path;
  std::string device = "cpu";
};

/// One decoded hypothesis. `score` is the mean log-prob per generated token
/// (closer to 0 = more confident), so hypotheses of different lengths stay
/// comparable; placement thresholds are calibrated against this scale.
struct Prediction {
  std::string text;
  float score = 0.0f;
};

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;

  virtual bool Initialize(const InferenceBackendConfig& config) = 0;
  virtual void Shutdown() = 0;
  virtual bool IsInitialized() const = 0;

  /// Run seq2seq inference, returning up to `num_hypotheses` distinct
  /// candidates sorted by descending score. An empty vector means failure
  /// (backend not ready, empty prompt, or no usable output).
  virtual std::vector<Prediction> Predict(const std::string& prompt,
                                          int max_tokens,
                                          int num_hypotheses) = 0;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_INFERENCE_BACKEND_H_
