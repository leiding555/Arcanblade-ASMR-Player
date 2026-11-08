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
  <a href="https://github.com/leiding555/AsmrPlayer-Arcanblade/releases/tag/v0.1.0-beta.1"><img src="https://img.shields.io/badge/release-v0.1.0--beta.1-7C5CE7?style=flat-square" alt="Release v0.1.0-beta.1"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4?style=flat-square" alt="平台：Windows 10/11 x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-0969da?style=flat-square" alt="许可证：MIT"></a>
</p>

<p align="center">
  <a href="#主要功能">主要功能</a> ·
  <a href="#使用说明">使用说明</a> ·
  <a href="#翻译效果对比">翻译效果对比</a> ·
  <a href="#常见问题">常见问题</a> ·
  <a href="#界面展示">界面展示</a> ·
  <a href="#许可证与致谢">许可证与致谢</a>
</p>

<p align="center">
  <img src="assets/screenshots/mainscreen.png" width="960" alt="主界面">
</p>

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

引擎和模型不随播放器发行包附带，首次使用需联网下载。离线直译、日语识别均在本机运行；字幕翻译需自行配置 API 服务。

## 使用说明

### 一、作品导入与播放

- **导入作品**：点击 **导入作品 → 导入单个作品…** 选择作品目录。批量管理可选 **添加扫描根目录…**：软件会将该目录下的**每个直属子文件夹视为独立作品**，分别导入其中的音频与字幕；没有受支持音频的文件夹会跳过。
- **字幕自动绑定**：支持 SRT、VTT、LRC，在同一作品目录内按名称匹配音频，**字幕与音频可以放在不同子文件夹中**。需要调整时，点击播放栏的 **选择或管理字幕**。
- **批量生成字幕**：在作品音轨列表点击 **批量**，勾选当前目录的多首音轨，再通过 **添加 → 离线直译／日语识别** 批量加入任务。
- **进入播放详情**：双击音轨开始播放，点击底部播放栏的**封面图片**即可展开播放详情页。当前支持 MP3、WAV 音频。
- **点击字幕跳转**：播放详情页和侧边字幕中的文字均可点击，直接跳转到对应的音频位置。

### 二、AI 字幕

<details>
<summary><strong>1. 离线直译：音频直接生成中文字幕</strong></summary>

1. 打开 **AI 字幕**，将 **生成方式** 设为 **离线直译**，进入 **组件与设置 → 安装组件**，选择下载安装位置，点击 **下载并准备**。如果已有完整组件，可通过 **选择已有组件…** 指定包含 `infer.exe` 的已解压目录。
2. **使用 GPU 加速需下载匹配的组件版本**：按显卡与驱动选择 NVIDIA CUDA 或对应 AMD 架构版本，可使用软件的自动推荐。随后将 **运行方式** 设为 **GPU（低显存）** 或 **GPU（高显存）**；没有合适 GPU 时可选择 **CPU**。显卡、显存与版本要求见 [上游硬件与版本说明](https://github.com/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice/blob/v1.10/README.md)。
3. 返回任务页，通过 **添加音频…** 或作品库的批量操作加入音频，点击 **开始生成**。支持批量任务，组件准备完成后无需联网。

<p align="center">
  <img src="assets/screenshots/modelDownload.png" width="800" alt="离线直译组件安装、硬件版本选择与运行方式设置">
</p>

</details>

<details>
<summary><strong>2. 日语识别：先生成日文字幕</strong></summary>

1. 将 **生成方式** 切换为 **日语识别**，进入 **组件与设置 → 安装组件**，设置安装位置后点击 **下载并准备**，软件会准备识别引擎和模型。已有完整组件时，可通过 **选择已有组件…** 指定包含 `faster-whisper-xxl.exe` 的目录。
2. 根据硬件选择 **GPU（低显存）／GPU（高显存）／CPU**。此流程的 GPU 加速使用 NVIDIA CUDA，组件说明见 [Faster-Whisper-XXL](https://github.com/Purfview/whisper-standalone-win)，硬件与运行环境可参考 [Faster Whisper 配置说明](https://github.com/SYSTRAN/faster-whisper#requirements)。
3. 添加音频并点击 **开始生成**，支持批量任务。成功生成的日文字幕会**自动加入字幕翻译列表**；点击 **前往字幕翻译** 即可继续配置并启动翻译。

<p align="center">
  <img src="assets/screenshots/modelDownload2.png" width="800" alt="日语识别引擎与模型的安装和运行方式设置">
</p>

</details>

<details>
<summary><strong>3. 字幕翻译：DeepSeek 润色与翻译</strong></summary>

1. 在 [DeepSeek 开放平台](https://platform.deepseek.com/api_keys) 创建 API Key，接口说明见 [DeepSeek API 文档](https://api-docs.deepseek.com/zh-cn/)。
2. 切换到 **字幕翻译 → 翻译设置**，服务选择 **DeepSeek**，选择模型，填写 API Key 并点击 **保存 Key**，可通过 **测试连接** 检查配置。
3. 返回任务页，使用日语识别自动加入的字幕，或点击 **添加字幕…** 导入字幕；可按需勾选下列选项，再点击 **开始翻译**，支持批量任务。

<p align="center">
  <img src="assets/screenshots/deepseek.png" width="800" alt="DeepSeek 服务、模型与 API Key 配置">
</p>

- **启用思考模式**：让模型先进行推理再输出，处理时间与用量可能增加。
- **使用作品上下文**：参考作品目录内已扫描的文本、Markdown、PDF 等资料构建作品上下文；没有可用正文时，参考同作品的日语识别结果，先校正日文，再翻译为中文。**目前仅支持 DeepSeek，以及本软件生成并已入库、原音轨仍在库中的日语识别字幕**；仅手动添加外部字幕无法使用此模式。该选项对整批任务生效，会增加 token 用量与费用。
- **自动绑定与使用**：作品库中的音轨翻译完成后，中文字幕会自动关联到原音轨；默认自动模式下使用最新生成的字幕。若曾手动指定默认字幕或选择不使用字幕，可在字幕管理中点击 **恢复默认**，恢复自动选择。

API 翻译会将字幕及启用的作品上下文发送给所选服务，请自行确认需要发送的内容。

</details>

## 翻译效果对比

以下为同一音频约 **11:49–12:26** 的实际输出。右侧使用日语识别后由 **DeepSeek Flash** 翻译，开启了**作品上下文与思考模式**。保留输出原文，仅合并展示相邻字幕；两种方式的断句与时间轴略有不同，单个片段仅供参考。

| 时间段（约） | 离线直译 | 日语识别＋字幕翻译（上下文＋思考） |
| --- | --- | --- |
| 11:49–12:00 | 对 对不起 我的头发很痒吧<br>下次给你掏耳朵的时候我会好好地睡一觉的 | 啊，对不起。是我的头发弄痒您了吧。下次掏耳朵的时候我会把头发扎起来的。 |
| 12:00–12:15 | 没错 头发变得挺长了对吧<br>因为前辈你说柚希的头发很漂亮嘛 | 是啊。长了不少吧。因为学长说过柚木的头发很漂亮嘛。 |
| 12:16–12:26 | 从中学开始就没有剪过<br>那之后这头发一直都是我最自豪的 | 从初中起就没剪过哦。从那以后，这头发一直是我最大的骄傲。 |

## 常见问题

**模型安装很慢、卡住或失败怎么办？**

软件从 GitHub 下载引擎组件，日语识别的 large-v2 模型从 Hugging Face 下载。网络不通或不稳定时，通常需要开启代理（梯子）后重试，需要 **开启全局代理/Tun/虚拟网卡模式**。如果仍无法下载，可以从上游手动下载，完整解压后在软件中设置组件路径：

1. **离线直译**：[ChickenRice v1.10 下载页](https://github.com/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice/releases/tag/v1.10)。选择适合显卡的 **translate 翻译版**，下载同一版本的全部分卷，合并为 ZIP 后完整解压，保留原有模型及依赖目录。在软件中选择包含 `infer.exe` 的目录。
2. **日语识别**：[Faster-Whisper-XXL 下载页](https://github.com/Purfview/whisper-standalone-win/releases/tag/Faster-Whisper-XXL)。下载并完整解压 **`Faster-Whisper-XXL_r245.4_windows.7z`**，另外下载 [large-v2 模型（与软件匹配的版本）](https://huggingface.co/Systran/faster-whisper-large-v2/tree/f0fe81560cb8b68660e564f55dd99207059c092e) 的四个完整文件，放入组件目录下的 **`_models/faster-whisper-large-v2/`**（`_models` 带下划线）：

```text
组件目录/
├─ faster-whisper-xxl.exe
└─ _models/
   └─ faster-whisper-large-v2/
      ├─ config.json
      ├─ model.bin
      ├─ tokenizer.json
      └─ vocabulary.txt
```

完成后，进入对应生成方式的 **组件与设置 → 选择已有组件…**，选择包含对应 EXE 的组件目录。日语识别可点击 **重新检查**，确认引擎和模型均已就绪后再开始任务。

## 界面展示

**作品库与同步字幕**

<p align="center">
  <img src="assets/screenshots/Side%20subtitles.png" width="960" alt="作品库、音轨列表与侧栏同步字幕">
</p>

**播放详情**

<p align="center">
  <img src="assets/screenshots/Playback%20Details%20Page.png" width="960" alt="封面与同步字幕组成的播放详情页面">
</p>

## 许可证与致谢

本项目原创代码及原创图标采用 [MIT License](LICENSE)。第三方引擎、模型和运行时依赖适用各自的许可证，来源与许可说明见 [NOTICE](NOTICE)。

感谢 Qt、TransWithAI、Purfview、SYSTRAN 及相关模型作者。
