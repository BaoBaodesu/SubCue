# SubCue 架构说明

本文说明源码分层、目录约定与测试目标划分。构建与运行步骤见根目录 `README.md`。

## 分层

```
src/
├── app/          应用层：进程入口、控制器、QML 视图与原生主题
├── core/         领域层：不依赖具体窗口系统的编辑器能力
└── platform/     平台层：仅 Windows 的系统能力实现
```

依赖方向固定为 `app → core → platform`。`core` 通过接口（`IAsrService`、`IAiProvider`、`IHttpClient`、`PreviewRenderer`、`ICredentialStore` 等）声明能力，具体实现按平台注入，因此测试可在不启动真实网络或音频设备的前提下替换实现。

## 应用层 `src/app`

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 进程入口；`loadFromModule("SubCue", "Main")` 加载 QML 模块 |
| `app_controller.*` | 编辑器唯一控制面，向 QML 暴露媒体、字幕、播放与打轴任务 |
| `application_context.*` | 组装 `core` 各子系统，供控制器与测试共用的依赖容器 |
| `native_theme.*` | Win32 原生标题栏/深色模式适配 |
| `ui_theme.h` | 与 `src/app/qml/Theme.qml` 保持同步的 C++ 侧颜色常量 |
| `timeline_scene_item.*` | 时间轴 Scene Graph 绘制项 |
| `video_preview_item.*` | 视频预览绘制项 |
| `qml/` | QML 源文件与 `icons/` 资源 |

QML 位于 `src/app/qml/`，与 `qt_add_qml_module` 的 `SubCue` 模块同源。QML 文件之间的相对引用（如 `icons/play.svg`）依赖同目录解析，移动该目录时必须整体移动。

## 领域层 `src/core`

| 子目录 | 职责 |
| --- | --- |
| `common/` | `MediaTime`、错误类型、日志、Provider 公共类型 |
| `media/` | FFmpeg 解封装、解码、重采样、硬件加速、缩略图 |
| `playback/` | 音频时钟、音频输出、帧索引、逐帧步进、Seek 调度 |
| `waveform/` | 波形金字塔生成与降采样 |
| `cache/` | 缓存键、LRU、预览帧缓存与完整内容哈希分析缓存 |
| `inference/` | GPU 推理互斥、Worker 生命周期、取消与崩溃隔离 |
| `timeline/` | 时间轴视口、吸附引擎、编辑命令场景 |
| `subtitle/` | 时间码、TXT/SRT/ASS 解析与写出、字幕模型与撤销栈 |
| `alignment/` | 文本与音频词级时间戳对齐、强制对齐、模糊匹配 |
| `asr/` | ASR Provider 工厂、DashScope、本地 Python 推理与分块 |
| `ai/` | OpenAI-compatible Provider 与复核工厂 |
| `project/` | 工程文件序列化 |
| `roughcut/` | 自动粗剪的采样区间模型与 Premiere xmeml 导出 |
| `settings/` | 设置持久化与凭据存储接口 |

## 平台层 `src/platform/windows`

`dpapi_credential_store.*`（凭据加密）、`wasapi_audio_device.*`（音频设备）、`d3d11_preview_adapter.*`（D3D11 预览）。

## 测试目标

`tests/cpp/` 每个文件对应一个 CTest 目标，命名与职责一致，不再按开发阶段编号。

| CTest 目标 | 源文件 | 覆盖范围 |
| --- | --- | --- |
| `SubCueMediaTimeTests` | `test_media_time.cpp` | 时间基与毫秒换算 |
| `SubCueSubtitleProjectTests` | `test_subtitle_project.cpp` | 时间码、TXT/SRT/ASS、字幕模型、撤销栈、工程与设置持久化、凭据存储 |
| `SubCueMediaTests` | `test_media.cpp` | 探针、解码、重采样、有界队列、FFmpeg 错误与损坏样本 |
| `SubCuePlaybackTests` | `test_playback.cpp` | 音频时钟、输出代次、帧索引、逐帧步进、Seek、硬件加速回退 |
| `SubCueCacheWaveformTests` | `test_cache_waveform.cpp` | 缓存键、完整内容分析缓存、LRU 淘汰、波形金字塔、缩略图与预览缓存 |
| `SubCueTimelineTests` | `test_timeline.cpp` | 视口换算与缩放、吸附、拖动/修剪/切分/合并与 Undo、场景布局与命中测试 |
| `SubCueRoughCutXmlTests` | `test_roughcut_xml.cpp` | 非破坏性源区间、帧量化、声道轨和完整 WAV 引用 |
| `SubCueAlignmentTests` | `test_alignment.cpp` | 对齐 golden、模糊比、归一化、强制对齐、转写排序与置信度阈值 |
| `SubCueAsrTests` | `test_asr.cpp` | 分块计划、DashScope 解析、本地模型校验、HTTP 客户端 |
| `SubCueAiReviewTests` | `test_ai_review.cpp` | 复核窗口、载荷脱敏、低置信度跳过、模型发现与错误处理 |
| `SubCueAppControllerTests` | `test_app_controller.cpp` | 媒体加载、工程导入、字幕应用、设置往返、导出、预览后端、WASAPI 探测 |
| `SubCueEditorIntegrationTests` | `test_editor_integration.cpp` | 真实 QML 加载与快捷键矩阵、打轴流水线、A/V 同步与播放稳定性 |
| `SubCueSourceHygieneTests` | `test_source_hygiene.cpp` | 源码树不残留 Python/Nuitka 运行路径与外部 CLI 调用 |
| `SubCuePackagingTests` | `test_packaging.cpp` | 便携包布局、许可声明、PE 导入洁净度、隔离 PATH 冒烟 |
| `SubCueStartupTest` | `SubCue --smoke-test` | 应用离屏启动 |

测试夹具：`tests/media/` 提供小型 CFR/VFR/音频/损坏样本，`tests/golden/` 提供对齐与字幕格式基线。需要媒体目录或 golden 目录的目标由 `tests/CMakeLists.txt` 通过 `SUBCUE_TEST_MEDIA_DIR` / `SUBCUE_TEST_GOLDEN_DIR` 注入路径。

## 第三方

本地识别通过独立 Python 推理进程加载 PyTorch。FFmpeg 通过 `vcpkg_installed/x64-windows` 以动态库形式链接，不依赖外部 `ffmpeg.exe`。
