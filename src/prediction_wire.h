// Wire format for publishing multiple AI predictions through a Context
// property (properties are plain strings).
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_PREDICTION_WIRE_H_
#define RIME_PREDICT_PREDICTION_WIRE_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "inference_backend.h"

namespace rime {
namespace predict {
namespace wire {

// Entries separated by \x1e, each entry "text\x1fscore". Model output is
// Hanzi/punctuation and never contains control bytes, so this stays cheap
// and unambiguous without pulling a JSON parser into the filter path.
constexpr char kEntrySep = '\x1e';
constexpr char kFieldSep = '\x1f';

inline std::string Encode(const std::vector<Prediction>& preds) {
  std::string out;
  char buf[32];
  for (size_t i = 0; i < preds.size(); ++i) {
    if (i)
      out.push_back(kEntrySep);
    out += preds[i].text;
    out.push_back(kFieldSep);
    std::snprintf(buf, sizeof(buf), "%.4f", preds[i].score);
    out += buf;
  }
  return out;
}

inline std::vector<Prediction> Decode(const std::string& blob) {
  std::vector<Prediction> out;
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t end = blob.find(kEntrySep, pos);
    if (end == std::string::npos)
      end = blob.size();
    std::string entry = blob.substr(pos, end - pos);
    pos = end + 1;
    size_t sep = entry.find(kFieldSep);
    if (sep == std::string::npos)
      continue;
    Prediction p;
    p.text = entry.substr(0, sep);
    p.score = static_cast<float>(std::atof(entry.c_str() + sep + 1));
    if (!p.text.empty())
      out.push_back(std::move(p));
  }
  return out;
}

}  // namespace wire
}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_PREDICTION_WIRE_H_
