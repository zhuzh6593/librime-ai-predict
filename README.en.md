# librime-ai-predict

**Context-aware AI candidates for Rime Pinyin, generated on your device.**

[简体中文](README.md) · English · [Download for macOS](https://github.com/openbankai/librime-ai-predict/releases/tag/v0.2.0-macos) · [Arch Linux / Omarchy](docs/arch-fcitx5.md)

<p>
  <img src="assets/readme/candidates.svg" width="100%" alt="Illustration: after committing 计算面积要用, the user types gongshi. Rime puts 公示 first and the AI suggestion 公式 second.">
</p>

After you commit “计算面积要用” (“to calculate the area, use”), typing `gongshi` can yield “公式” (“formula”) as an AI candidate. New or promoted suggestions appear in the second slot by default. If Rime already ranks the same text first, the original candidate stays in place.

<details>
<summary>Watch the Squirrel typing demo (early version)</summary>

![Squirrel typing demo showing the candidate menu updating as the user enters Pinyin](docs/demo.gif)

</details>

## Quick start

### macOS · Squirrel

The signed, notarized installer requires **macOS 13 or later on Apple silicon**. It bundles the plugin, the `zh-base-ct2-int8` Simplified Chinese model, and the Luna Pinyin and Rime Ice schemas. [Release notes](https://github.com/openbankai/librime-ai-predict/releases/tag/v0.2.0-macos)

1. Download and install [`Squirrel-1.1.4-ai-predict-arm64.pkg`](https://github.com/openbankai/librime-ai-predict/releases/download/v0.2.0-macos/Squirrel-1.1.4-ai-predict-arm64.pkg).
2. Log out and back into macOS, then select Squirrel (鼠须管) in the input menu. Press `F4` to switch schemas.
3. Type `jintiantianqihenhao`, pause briefly, and check the candidate menu. If the model agrees with the first candidate, the menu keeps that entry.

> The installer backs up `~/Library/Rime/` and replaces `default.custom.yaml`, `luna_pinyin.custom.yaml`, and `rime_ice.custom.yaml`. It preserves learned dictionaries and other schemas’ custom files. You can recover your previous settings from `~/Library/Rime.backup-<timestamp>/`.

For Intel Macs or custom builds, see the source instructions below.

### Arch Linux / Omarchy · Fcitx5

The repository provides a PKGBUILD for `x86_64` and `aarch64`. The resulting package includes Fcitx5-Rime with asynchronous refresh support, librime with the plugin built in, the model, and the AI Pinyin schema (AI 拼音). It replaces `fcitx5-rime`. See the [Arch Linux / Omarchy packaging guide (Chinese)](docs/arch-fcitx5.md) for build, installation, and setup steps.

## How it works

1. **Collect context.** The translator combines recently committed Chinese text and punctuation with the Pinyin you’re typing. By default, inference starts at 2 Pinyin letters when Chinese context is available and 12 input characters otherwise.
2. **Run inference in the background.** CTranslate2 runs the INT8 model on the local CPU after a brief pause in typing. All inference happens on your device and the plugin never uploads what you type; the installer bundles the model, so prediction works offline.
3. **Update the candidate menu.** By default the translator only surfaces predictions containing at least two Chinese characters; the filter deduplicates and repositions the suggestion. The frontend gets asynchronous updates through `_refresh_ui`. Squirrel can also highlight the AI marker through `_comment_highlight`. [Frontend protocol](https://github.com/rime/squirrel/issues/1124)

The bundled model targets Simplified Chinese; predictions may be less accurate with Traditional Chinese context. We recommend enabling Simplified Chinese output for your own Pinyin schema. Local diagnostic logs may include Pinyin, committed context, and predictions. Redact them before sharing.

## Build from source and configure

<details>
<summary>Build Squirrel with the plugin</summary>

Use Squirrel source that supports the asynchronous refresh protocol. You’ll need Xcode, CMake, and Boost. [Squirrel Makefile](https://github.com/rime/squirrel/blob/master/Makefile)

```bash
git clone --recursive https://github.com/rime/squirrel.git
cd squirrel

bash librime/install-plugins.sh \
  hchunhui/librime-lua \
  lotem/librime-octagram \
  rime/librime-predict \
  openbankai/librime-ai-predict

(cd librime/plugins/ai-predict && make deps)
export BOOST_ROOT="$(brew --prefix boost)"
make deps && make && sudo make install

(cd librime/plugins/ai-predict && bash scripts/download_model.sh)
```

Running `make deps` in the plugin directory builds the static CTranslate2 library. The download script installs the [INT8 model (about 204 MiB)](https://github.com/openbankai/librime-ai-predict/releases/tag/model-v1) in `~/Library/Rime/predict_models/zh-base-ct2-int8/` by default. Set `RIME_USER_DATA_DIR` for another frontend’s data directory or `MODEL_URL` to use a mirror.

After installing, log out and back in, then configure your schema below.

</details>

1. In Rime’s `default.custom.yaml`, merge `'modules/@next': ai_predict` into the existing `patch`.
2. In your Pinyin schema, add `ai_predict_translator` **first** in `engine/translators` and `ai_predict_filter` last in `engine/filters`. Keep your other components.
3. Set `ai_predict/model_path` to the model directory. Use an absolute path or one relative to Rime’s user data directory. Save your changes and redeploy from the frontend menu.

See the [schema example](examples/schema.fragment.yaml) for the full YAML, candidate position, and input thresholds. To disable AI candidates, set `ai_predict/enabled` to `false` and redeploy.

## Troubleshooting and evaluation

- **The candidate is missing.** Check that the module is loaded, the translator comes first, and the model directory contains `model.bin` and `shared_vocabulary.json`. If candidates update only after another keystroke, check the frontend’s asynchronous refresh support. Squirrel builds need [rime/squirrel#1143](https://github.com/rime/squirrel/pull/1143).
- **Find logs.** The plugin may share the host log or write to a file whose name contains `ai_predict`. Check the frontend’s log directory first. For Squirrel, also check `~/Library/Logs/Squirrel/` and `${TMPDIR}/rime.squirrel/`. [Log analysis script](scripts/analyze_predictions.py)
- **Evaluate the model.** `eval/testset.py` extracts examples from local logs. `eval/bench.py` uses CTranslate2 and Tokenizers to measure how often model output matches the inferred commits. The scripts infer commits from changes in logged context, so the result is an offline match rate. [Evaluation scripts](eval/)

## License

[BSD 3-Clause](LICENSE)
