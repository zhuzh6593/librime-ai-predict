# librime-ai-predict

**结合上文，为 Rime 的拼音输入补充本地 AI 候选。**

简体中文 · [English](README.en.md) · [下载安装包](https://github.com/openbankai/librime-ai-predict/releases/tag/v0.2.0-macos) · [Arch Linux / Omarchy](docs/arch-fcitx5.md)

<p>
  <img src="assets/readme/candidates.svg" width="100%" alt="候选示意：已输入「计算面积要用」后键入 gongshi，Rime 候选「公示」在第一位，AI 候选「公式」在第二位。">
</p>

以候选栏为例，已提交「计算面积要用」后输入 `gongshi`，模型可根据上文建议「公式」。新插入或前移的 AI 候选默认放在第 2 位。如果首选已经与模型建议一致，就保留原候选。

<details>
<summary>观看鼠须管输入演示（早期版本）</summary>

![鼠须管输入演示，候选栏随正在输入的拼音更新](docs/demo.gif)

</details>

## 快速开始

### macOS · 鼠须管

适用于 **macOS 13 及以上的 Apple Silicon Mac**。安装包已签名并公证，内置插件、`zh-base-ct2-int8` 简体模型、朙月拼音和雾凇拼音。[发布说明](https://github.com/openbankai/librime-ai-predict/releases/tag/v0.2.0-macos)

1. 下载并安装 [`Squirrel-1.1.4-ai-predict-arm64.pkg`](https://github.com/openbankai/librime-ai-predict/releases/download/v0.2.0-macos/Squirrel-1.1.4-ai-predict-arm64.pkg)。
2. 注销并重新登录 macOS，在输入法菜单中切换到「鼠须管」。按 `F4` 可切换方案。
3. 输入 `jintiantianqihenhao`，稍等片刻，查看候选栏。如果模型建议已是首选，候选栏会直接保留原来的那一项。

> 安装包会备份 `~/Library/Rime/`，并覆盖其中的 `default.custom.yaml`、`luna_pinyin.custom.yaml` 和 `rime_ice.custom.yaml`。用户词库和其他方案的自定义文件会保留。安装后可从 `~/Library/Rime.backup-<时间戳>/` 找回原配置。

Intel Mac 或需要自定义构建的用户可参照下文。

### Arch Linux / Omarchy · Fcitx5

仓库提供面向 `x86_64` 和 `aarch64` 的 PKGBUILD。构建产物包含带异步刷新补丁的 Fcitx5-Rime、合入插件的 librime、模型和「AI 拼音」方案。该包会替换 `fcitx5-rime`。构建、安装和启用步骤见 [Arch Linux / Omarchy 打包指南（中文）](docs/arch-fcitx5.md)。

## 工作原理

1. **收集上下文。** Translator 取近期已提交的中文与标点，结合当前正在输入的拼音。默认有中文上下文时至少需要 2 个拼音字母，冷启动时至少需要 12 个输入字符。
2. **后台推理。** CTranslate2 在本机 CPU 上运行 INT8 模型，输入停顿后异步推理。推理全部在本机完成，输入内容不会上传；安装包已内置模型，装好后可离线使用。
3. **更新候选。** Translator 默认只放行至少含两个汉字的预测；Filter 负责去重，并把候选放到目标位置。前端通过 `_refresh_ui` 接收异步刷新通知，鼠须管还可通过 `_comment_highlight` 高亮 AI 标记。[前端协议](https://github.com/rime/squirrel/issues/1124)

随附模型面向简体中文；繁体上下文下预测质量可能下降，自定义拼音方案时建议开启简体输出。插件的本机诊断日志可能包含拼音、已提交的上下文和预测文本，分享前请先脱敏。

## 从源码构建与配置

<details>
<summary>构建鼠须管和本插件</summary>

使用包含异步刷新协议的鼠须管源码，并准备 Xcode、CMake 和 Boost。[鼠须管 Makefile](https://github.com/rime/squirrel/blob/master/Makefile)

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

插件目录中的 `make deps` 编译 CTranslate2 静态库。模型脚本默认把 [INT8 模型（约 204 MiB）](https://github.com/openbankai/librime-ai-predict/releases/tag/model-v1) 下载到 `~/Library/Rime/predict_models/zh-base-ct2-int8/`。可用 `RIME_USER_DATA_DIR` 指定其他前端的数据目录，或用 `MODEL_URL` 指定镜像。

安装后注销并重新登录，再按下方步骤配置方案。

</details>

1. 在 Rime 的 `default.custom.yaml` 中，将 `'modules/@next': ai_predict` 合并到现有的 `patch`。
2. 在所用拼音方案中，把 `ai_predict_translator` 放到 `engine/translators` **第一项**，把 `ai_predict_filter` 加到 `engine/filters` 末尾。保留原有组件。
3. 将 `ai_predict/model_path` 设为模型目录，路径可相对于 Rime 用户数据目录，也可使用绝对路径。保存后在前端菜单中重新部署。

完整 YAML 示例、候选位置和触发阈值见 [方案配置示例](examples/schema.fragment.yaml)。将 `ai_predict/enabled` 设为 `false` 后重新部署，可关闭 AI 候选。

## 调试与评估

- **候选没有出现。** 检查模块是否加载、Translator 是否排在首位，以及模型目录中是否有 `model.bin` 和 `shared_vocabulary.json`。候选只有在继续键入后才出现时，检查前端是否支持异步刷新。自行编译鼠须管时需要包含 [rime/squirrel#1143](https://github.com/rime/squirrel/pull/1143)。
- **查看日志。** 插件可能与宿主共用日志，也可能写入名称含 `ai_predict` 的文件。优先查看前端设置的日志目录，鼠须管也可检查 `~/Library/Logs/Squirrel/` 和 `${TMPDIR}/rime.squirrel/`。[日志分析脚本](scripts/analyze_predictions.py)
- **评估模型。** `eval/testset.py` 从本机日志提取样本，`eval/bench.py` 使用 CTranslate2 和 Tokenizers 计算模型输出与推断提交文本的匹配率。样本来自日志中的上下文变化，测得的匹配率属于离线评估。[评估目录](eval/)

## 许可证

[BSD 3-Clause](LICENSE)
