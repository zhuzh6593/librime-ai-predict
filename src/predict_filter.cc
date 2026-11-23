// Copyright RIME Developers
// Distributed under the BSD License

#include "predict_filter.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <glog/logging.h>

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/translation.h>

#include "frontend_protocol.h"
#include "prediction_wire.h"

namespace rime {
namespace predict {

namespace {

constexpr const char* kAITextProperty = "ai_predict/text";
constexpr const char* kAICandidateType = "ai_predict";
constexpr const char* kAICommentMarker = "AI";

// Publish the index of the AI row so opt-in frontends accent-colour its
// comment (see frontend_protocol.h). Idempotent: skip the set_property when
// unchanged, to avoid a spurious notification on every Compose().
void PublishCommentHighlight(Engine* engine, int index) {
  if (!engine || !engine->context()) return;
  Context* ctx = engine->context();
  const std::string value = protocol::MakeCommentHighlightPayload(index);
  if (ctx->get_property(protocol::kCommentHighlight) != value) {
    ctx->set_property(protocol::kCommentHighlight, value);
  }
}

/// Wraps an upstream translation, prefetches up to `search_range` candidates,
/// then places the AI predictions (read from the engine's context property)
/// starting at their earned slot. Placement policy:
/// 1. Top hypothesis clears promote_score_ → slot #0; otherwise target_index_.
/// 2. Primary AI text already at that slot → do nothing (a relabel or
///    duplicate row would just be noise).
/// 3. Primary AI text matches a candidate further down → promote that one
///    (no dup row). Novel → insert a fresh candidate.
/// 4. Remaining hypotheses are appended right after the primary row, skipping
///    any that already exist in the upstream window (no duplicates).
///
/// AI-typed candidates emitted upstream by PredictTranslator are stripped up
/// front, so this filter is the single source of truth for the AI row.
class AIPredictFilteredTranslation : public Translation {
 public:
  AIPredictFilteredTranslation(an<Translation> upstream,
                               std::vector<Prediction> ai_rows,
                               double promote_score,
                               size_t target_index,
                               size_t search_range)
      : upstream_(std::move(upstream)),
        ai_rows_(std::move(ai_rows)),
        promote_score_(promote_score),
        target_index_(target_index),
        search_range_(search_range) {
    Build();
    set_exhausted(cursor_ >= reordered_.size() &&
                  (!upstream_ || upstream_->exhausted()));
  }

  // -1 means we did not insert an AI-tagged row this round (either the AI
  // text was empty, slot #1 already matched it so no relabelling happened,
  // or upstream produced nothing). PredictFilter::Apply forwards this to
  // PublishCommentHighlight, which encodes it as the wire payload.
  int ai_inserted_index() const { return ai_inserted_index_; }

  bool Next() override {
    if (exhausted()) return false;
    if (cursor_ < reordered_.size()) {
      ++cursor_;
    } else if (upstream_ && !upstream_->exhausted()) {
      upstream_->Next();
    }
    if (cursor_ >= reordered_.size() &&
        (!upstream_ || upstream_->exhausted())) {
      set_exhausted(true);
    }
    return !exhausted();
  }

  an<Candidate> Peek() override {
    if (exhausted()) return nullptr;
    if (cursor_ < reordered_.size()) {
      return reordered_[cursor_];
    }
    if (upstream_ && !upstream_->exhausted()) {
      return upstream_->Peek();
    }
    return nullptr;
  }

 private:
  void Build() {
    if (!upstream_) return;

    std::vector<an<Candidate>> buf;
    buf.reserve(search_range_);
    while (buf.size() < search_range_ && !upstream_->exhausted()) {
      if (auto c = upstream_->Peek()) {
        buf.push_back(c);
      }
      upstream_->Next();
    }

    // 引擎的 uniquifier 按「已弹出的候选列表」去重,而此刻引擎还没弹出任何候选、
    // 那批列表为空,所以它没法对这批预取的候选去重。这里按文本补一道去重,
    // 避免词库词被句模式/用户词库重复(如 haishi → 还是 + 还是)。
    for (size_t i = 0; i < buf.size(); ++i) {
      for (size_t j = i + 1; j < buf.size();) {
        if (buf[j]->text() == buf[i]->text()) {
          buf.erase(buf.begin() + j);
        } else {
          ++j;
        }
      }
    }

    if (ai_rows_.empty()) {
      // No AI suggestion in flight; pass everything through unchanged.
      reordered_ = std::move(buf);
      return;
    }

    // We are the single source of truth for the AI row, so drop any AI-typed
    // candidates the translator emitted before deciding our own placement.
    auto first_ai = std::stable_partition(
        buf.begin(), buf.end(), [](const an<Candidate>& c) {
          return !c || c->type() != kAICandidateType;
        });
    buf.erase(first_ai, buf.end());

    if (buf.empty()) {
      // No upstream candidate means no segment range to anchor an AI candidate.
      reordered_ = std::move(buf);
      return;
    }

    const string& primary_text = ai_rows_.front().text;
    // Score-based placement: a confident top hypothesis earns slot #0,
    // anything weaker falls back to the configured target slot.
    const bool promotes = ai_rows_.front().score >= promote_score_;
    size_t slot = promotes ? 0 : target_index_;

    auto text_in_buf = [&buf](const string& t) {
      return std::any_of(buf.begin(), buf.end(),
                         [&](const an<Candidate>& c) {
                           return c && c->text() == t;
                         });
    };

    // Rows to inject: primary first, then runner-up hypotheses that the
    // dictionary window does not already show (no duplicate rows).
    std::vector<Prediction> inject;
    inject.push_back(ai_rows_.front());
    for (size_t i = 1; i < ai_rows_.size(); ++i) {
      const string& t = ai_rows_[i].text;
      if (t == primary_text || text_in_buf(t)) {
        continue;
      }
      inject.push_back(ai_rows_[i]);
    }

    // Promote an existing match (wrapped so type/preedit/quality survive and
    // only the comment changes) rather than inserting a second copy.
    an<Candidate> primary_cand;
    bool primary_untouched = false;
    auto dup = std::find_if(buf.begin(), buf.end(),
                            [&primary_text](const an<Candidate>& c) {
                              return c && c->text() == primary_text;
                            });
    if (dup != buf.end() &&
        static_cast<size_t>(std::distance(buf.begin(), dup)) == slot) {
      // The dictionary already shows the primary text at the target slot;
      // leave that row alone and only append the runner-ups after it.
      primary_untouched = true;
      LOG(INFO) << "ai_predict_filter: primary '" << primary_text
                << "' already at slot #" << slot << "; appending "
                << (inject.size() - 1) << " runner-up(s)";
    } else if (dup != buf.end()) {
      auto matched = *dup;
      size_t orig_pos = std::distance(buf.begin(), dup);
      buf.erase(dup);
      primary_cand = New<ShadowCandidate>(matched, kAICandidateType,
                                          /*text=*/string(),
                                          /*comment=*/kAICommentMarker,
                                          /*inherit_comment=*/false);
      LOG(INFO) << "ai_predict_filter: promoted existing candidate '"
                << matched->text() << "' (was at slot #" << orig_pos
                << ", type=" << matched->type() << ") to slot #" << slot
                << " with AI marker";
    } else {
      // ShadowCandidate（shadow 到上游首候选）：librime 对 shadow 候选的
      // 确认/commit 走标准路径；SimpleCandidate 自定义类型选中时 commit 为空
      primary_cand = New<ShadowCandidate>(buf.front(), kAICandidateType,
                                          primary_text, kAICommentMarker,
                                          /*inherit_comment=*/false);
      LOG(INFO) << "ai_predict_filter: inserted '" << primary_text
                << "' at slot #" << slot << " (score="
                << ai_rows_.front().score << ", promote=" << promotes << ")";
    }

    // Splice: everything before slot, the AI rows (primary first, runner-ups
    // by descending score right after), then the rest of the dictionary.
    if (slot > buf.size()) {
      slot = buf.size();
    }
    auto push_secondary = [&](const Prediction& row) {
      reordered_.push_back(New<ShadowCandidate>(
          buf.front(), kAICandidateType, row.text, kAICommentMarker,
          /*inherit_comment=*/false));
    };
    reordered_.reserve(buf.size() + inject.size());
    for (size_t i = 0; i < slot; ++i) {
      reordered_.push_back(buf[i]);
    }
    if (primary_untouched) {
      // The dictionary already carries the primary text at `slot`; keep that
      // row in place and slot the runner-ups in directly after it.
      reordered_.push_back(buf[slot]);
      for (size_t i = 1; i < inject.size(); ++i) {
        push_secondary(inject[i]);
      }
      for (size_t i = slot + 1; i < buf.size(); ++i) {
        reordered_.push_back(buf[i]);
      }
    } else {
      reordered_.push_back(primary_cand);
      for (size_t i = 1; i < inject.size(); ++i) {
        push_secondary(inject[i]);
      }
      for (size_t i = slot; i < buf.size(); ++i) {
        reordered_.push_back(buf[i]);
      }
    }
    ai_inserted_index_ = static_cast<int>(slot);
  }

  an<Translation> upstream_;
  std::vector<Prediction> ai_rows_;
  double promote_score_;
  size_t target_index_;
  size_t search_range_;

  std::vector<an<Candidate>> reordered_;
  size_t cursor_ = 0;
  int ai_inserted_index_ = -1;
};

}  // namespace

PredictFilter::PredictFilter(const Ticket& ticket)
    : Filter(ticket), TagMatching(ticket) {
  if (!engine_ || !engine_->schema() || !engine_->schema()->config()) {
    return;
  }
  // 在 Compose 完成（菜单已填充）后发刷新信号：宿主此刻采样快照才完整。
  // filter 与 engine 同生命周期重建，连接随 context 销毁自动失效
  engine_->context()->update_notifier().connect([this](Context* ctx) {
    if (enabled_ && !ctx->get_property(kAITextProperty).empty())
      ctx->set_property("ai_predict/refresh", "1");
  });
  Config* cfg = engine_->schema()->config();
  int n = 0;
  if (cfg->GetInt("ai_predict/target_index", &n) && n >= 0) {
    target_index_ = static_cast<size_t>(n);
  }
  if (cfg->GetInt("ai_predict/search_range", &n) && n > 0) {
    search_range_ = static_cast<size_t>(n);
  }
  double ps = 0.0;
  if (cfg->GetDouble("ai_predict/promote_score", &ps)) {
    promote_score_ = ps;
  } else {
    // The deployer serializes negative floats as quoted strings
    // ("promote_score: \"-0.45\""), which GetDouble refuses; coerce then.
    std::string ps_str;
    if (cfg->GetString("ai_predict/promote_score", &ps_str)) {
      promote_score_ = std::atof(ps_str.c_str());
    }
  }
  bool enabled = true;
  if (cfg->GetBool("ai_predict/enabled", &enabled)) {
    enabled_ = enabled;
  }
  LOG(INFO) << "ai_predict_filter: ctor target_index=" << target_index_
            << " promote_score=" << promote_score_
            << " search_range=" << search_range_ << " enabled=" << enabled_;
}

an<Translation> PredictFilter::Apply(an<Translation> translation,
                                     CandidateList* /*candidates*/) {
  if (!engine_ || !engine_->context() || !translation) {
    return translation;
  }
  std::vector<Prediction> ai_rows =
      wire::Decode(engine_->context()->get_property(kAITextProperty));
  if (ai_rows.empty()) {
    // No AI in flight this Compose; clear any stale highlight from the
    // previous frame so the frontend doesn't keep colouring a row that no
    // longer carries AI semantics.
    PublishCommentHighlight(engine_, -1);
    return translation;
  }
  auto wrapped = New<AIPredictFilteredTranslation>(
      std::move(translation), std::move(ai_rows), promote_score_,
      target_index_, search_range_);
  // Publish the index where the AI row landed (or -1 if none was inserted,
  // e.g. dedup against slot #1). Synchronous on Compose() so the frontend
  // has the index before it observes the new menu.
  PublishCommentHighlight(engine_, wrapped->ai_inserted_index());
  return wrapped;
}

}  // namespace predict
}  // namespace rime
