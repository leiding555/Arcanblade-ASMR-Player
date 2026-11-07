<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="resources/branding/arcanblade-mark-256.png">
    <img src="resources/branding/arcanblade-mark-black-256.png" width="160" height="160" alt="Arcanblade 图标">
  </picture>
</p>

<h1 align="center">Arcanblade (Windows)</h1>

<p align="center">
  <strong>管理音声作品与字幕，AI识别音频并生成中文字幕。</strong>
</p>

<p align="center">
  面向 Windows 的 ASMR 音声管理与播放器，支持字幕自动匹配与同步显示。<br>
  离线生成中文字幕，或先识别日文，再结合<strong>作品级上下文</strong>润色与翻译。
</p>

<p align="center">
  <a href="https://github.com/leiding555/Arcanblade-ASMR-Player/releases/tag/v0.1.0-beta.1"><img src="https://img.shields.io/badge/release-v0.1.0--beta.1-7C5CE7?style=flat-square" alt="Release v0.1.0-beta.1"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4?style=flat-square" alt="平台：Windows 10/11 x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-0969da?style=flat-square" alt="许可证：MIT"></a>
</p>

<img src="assets/screenshots/mainscreen.png" width="960" alt="主界面">

## 主要功能

- **音声作品管理**：导入本地作品，按作品与目录浏览、播放音轨，自动保存播放进度。
- **同步字幕**：支持 SRT、VTT、LRC，自动匹配本地字幕，可在侧栏或桌面浮窗同步显示。
- **离线直译**：日语音频直接生成中文字幕，组件安装后完全离线运行。
- **作品级上下文润色与翻译**：先在本机识别日文字幕，再由大语言模型参考作品资料或同作品识别结果，润色校正日文并翻译为中文。

两种 AI 字幕方式，可按需求选择：

| 方式 | 流程 | 侧重点 |
| --- | --- | --- |
| 离线直译 | 日语音频 → 中文字幕 | 方便、完全离线 |
| 识别后翻译 | 日语音频 → 日文字幕 → 作品级上下文润色与翻译 → 中文字幕 | 作品级上下文、译文质量 |

首次使用需联网下载引擎和模型，组件不随播放器发行包附带。API 翻译需自行配置服务与 API Key；作品级上下文润色目前仅支持 DeepSeek，需对本软件生成、已入库的日文字幕启用“使用作品上下文”，普通模式仅翻译。API 翻译会向所配置的服务发送字幕及启用的上下文。

## 界面展示

**作品库与同步字幕**

<img src="assets/screenshots/Side%20subtitles.png" width="960" alt="作品库、音轨列表与侧栏同步字幕">

**播放详情**

<img src="assets/screenshots/Playback%20Details%20Page.png" width="960" alt="封面与同步字幕组成的播放详情页面">

## 使用说明

1. **导入作品**：点击 **导入作品 → 导入单个作品…**，选择作品目录；管理多个作品可使用 **添加扫描根目录…**。
2. **开始播放**：选择作品，双击音轨，或点击 **播放此目录**。当前支持 MP3、WAV 音频。
3. **查看或生成字幕**：本地字幕会按匹配规则自动关联，可通过底栏 **选择或管理字幕** 调整。没有字幕时，右键音轨选择 **离线直译…**，或先 **日语识别…**，再到 **AI 字幕** 页面翻译。

## 许可证与致谢

本项目原创代码及原创图标采用 [MIT License](LICENSE)。第三方引擎、模型和运行时依赖适用各自的许可证，来源与许可说明见 [NOTICE](NOTICE)。

感谢 Qt、TransWithAI、Purfview、SYSTRAN 及相关模型作者。
