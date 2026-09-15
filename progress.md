# SubCue 自动音频粗剪开发进展

更新时间：2026-09-14

本文用于在新对话中恢复上下文。当前工作区包含用户原有的未提交修改和本轮新增内容；后续操作不要重置、覆盖或清理整个工作区，应只修改与粗剪任务直接相关的文件。

## 总体目标

在现有 SubCue 中增加独立的“自动粗剪工作区”，处理完整旁白 WAV 和可选 TXT/DOCX 文案，通过 ASR、文案匹配、重录检测和保守决策生成非破坏性时间线，并导出 Premiere 可读取的 FCP7/xmeml XML。

计划共十个阶段。Phase 1 已由用户在 Premiere Pro 2026 中人工验收通过。Phase 2～8 的核心模块已经实现。Phase 9 已接入可疑区间提取、Fun-ASR/Whisper 串行调度、冲突规则和工程持久化。Phase 10 已完成正式工作区路由、分析版本保留和专用推理运行库打包；真实长音频与完整模型场景仍需验收。

## 已完成事项

### Phase 1：Premiere XML 验收工具

- 新增独立命令行工具 `SubCueRoughCutXml`，输入完整 WAV 和片段 JSON，输出 FCP7/xmeml XML。
- XML 中所有片段引用同一个完整 WAV，包含粗剪轨和禁用的完整参考轨。
- 保留源媒体时长、声道和采样信息，只在导出边界转换到帧时基。
- 已生成 A/B1/B2/C/D1/D2 测试素材、清单和验收说明。
- 用户已在 Premiere Pro 2026 验证拖边恢复 B1/D1、Match Frame、Slip Edit 和参考轨禁用，并明确确认 Phase 1 验收通过。
- 相关文件：
  - `src/core/roughcut/rough_cut_types.h`
  - `src/core/roughcut/xmeml_exporter.h/.cpp`
  - `src/tools/roughcut_xml_main.cpp`
  - `tests/cpp/test_roughcut_xml.cpp`
  - `docs/roughcut-phase1-premiere-check.md`

### Phase 2：专用推理环境与 Qwen 接入

- 保留系统 Python，不修改用户现有 Python 设置。
- 建立项目专用 `.venv-inference`，使用 Python 3.11.16、`torch==2.6.0+cu124` 和 `qwen-asr==0.0.6`。
- 在 RTX 3060 Ti 8GB 上验证 CUDA 可用，并完成真实 CUDA Tensor 运算。
- 使用 `models/fun-asr-nano-2512/example/zh.mp3` 完成真实 Qwen3-ASR 推理，识别结果为“開放時間：早上九點至下午五點。”。
- Qwen Forced Aligner 已返回真实字词时间，范围约 0.64～5.12 秒。
- 无对齐结果时改为保存真实分块覆盖范围并标记 `timing: chunk`，不再返回原先错误的 10ms 时长。
- 唯一推理进程 `SubCueInference.exe` 已通过 CMake 构建，内嵌 Python 3.11；主程序不加载 Python/Torch。
- Worker 使用带任务 ID 的逐行 JSON 标准输入输出协议；支持进度、取消、错误、结果文件传递。
- `InferenceManager` 对 GPU 任务使用统一互斥门禁，现有 Whisper 字幕入口也经过同一门禁。
- Worker 崩溃和显存不足会转换为错误，主程序保持存活。
- 分析缓存使用完整媒体 SHA256，以及模型、语言、参数和算法版本生成键。
- Fun-ASR 的 `model.pt.incomplete` 会被明确拒绝，未启用隐式 VAD 模型下载。
- CMake 输出位置：`out/build/phase2-nmake/inference/SubCueInference.exe`。
- 相关文件：
  - `src/inference/main.cpp`
  - `src/core/inference/inference_manager.h/.cpp`
  - `src/core/cache/analysis_cache.h/.cpp`
  - `src/core/asr/local_python_asr_service.cpp`
  - `src/core/asr/whisper_cpp_service.cpp`
  - `tools/asr_python_worker.py`
  - `tools/setup-inference-env.ps1`
  - `tools/requirements-inference.txt`
  - `docs/roughcut-phase2-inference.md`

### Phase 3：文案解析与顺序匹配

- TXT 使用 UTF-8 读取并验证编码，保留行号和段落信息。
- DOCX 由 Worker 使用 Python 标准库 `zipfile` 和 XML 解析，不引入 Word/ZIP SDK。
- 解析正文、显式换行和表格文本；不把批注或删除修订作为正文。
- 新增基于现有 `Normalizer` 和 `FuzzRatio` 的顺序模糊匹配。
- 支持 MATCH、MODIFIED、SKIPPED、RETAKE、ADDED。
- 文案差异只产生匹配状态，不直接触发 CUT。
- 相关文件：
  - `src/core/roughcut/script_document.h/.cpp`
  - `src/core/roughcut/script_matcher.h/.cpp`
  - `tests/cpp/test_roughcut_script.cpp`

### Phase 4：对齐校验与静音证据

- 将字词毫秒时间转换为原 WAV 采样率下的 `qint64` 采样坐标。
- 校验时间顺序、有效范围和源音频边界。
- 对重叠分块产生的同词重复进行合并；无法消解的异词重叠进入 REVIEW。
- `timing: chunk` 等粗定位结果标为不可信，不能据此自动裁切。
- 新增能量阈值静音检测，只输出静音证据，不把它解释为可靠语音分类。
- 相关文件：
  - `src/core/roughcut/alignment_evidence.h/.cpp`
  - `tests/cpp/test_roughcut_alignment.cpp`

### Phase 5：Retake Detector

- 按文案关联和文本相似度建立重录候选组。
- 默认只搜索邻近 90 秒，可通过参数调整。
- 保存每个 Take 的评分及“句子完整/疑似中断”等理由。
- 不使用“最后一次一定正确”的规则；质量接近或缺少完整版本时整组保持 REVIEW。
- 已测试最后一次更差、候选质量接近和超出 90 秒窗口的场景。
- 相关文件：
  - `src/core/roughcut/retake_detector.h/.cpp`
  - `tests/cpp/test_retake_detector.cpp`

### Phase 6：Decision Engine

- 每个片段分别保存 `autoDecision` 和可空的 `userDecision`。
- `effectiveDecision()` 优先使用人工决定；清空人工决定后恢复自动决定。
- 自动 CUT 同时要求：存在已选出的可信完整替代版本、当前 Take 有独立失败证据、对齐边界可信。
- 没有明确替代版本、只有文本重复、边界不可信或候选存在冲突时保持 REVIEW。
- 每个决定保存规则评分、reason 和 evidence。
- 相关文件：
  - `src/core/roughcut/decision_engine.h/.cpp`
  - `tests/cpp/test_roughcut_decision.cpp`

### Phase 7：TimelineEngine 与 GapOptimizer

- 按录音原始顺序编排 KEEP/REVIEW，CUT 只从时间线排除。
- 使用原 WAV 采样索引，不生成正式合成 WAV。
- 默认 PreRoll 80ms、PostRoll 120ms，并限制在源范围和相邻语音边界内，避免跨入已删除语音或重复播放。
- 将原始间距限制在任务书范围：
  - 同一句内部：100～180ms
  - 逗号：180～280ms
  - 普通句子：300～450ms
  - 段落：450～700ms
  - 明显换话题：600～900ms
- 相关文件：
  - `src/core/roughcut/timeline_engine.h/.cpp`
  - `tests/cpp/test_roughcut_timeline.cpp`

### Phase 8：独立界面开发入口（控制器已接入）

- 新增 `src/app/qml/RoughCutWorkspace.qml`。
- 当前包含素材/文案区、源波形/播放器区、虚拟化结果列表、ALL/KEEP/REVIEW/CUT 筛选和底部粗剪时间线区域。
- `Main.qml` 的正式字幕布局未修改。
- `src/app/main.cpp` 支持通过开发参数单独加载粗剪界面：

```powershell
out\build\phase2-nmake\SubCue.exe --roughcut-workspace
```

- 已使用 `--roughcut-workspace --smoke-test` 验证 QML 能创建并正常退出。
- 新增 `RoughCutController` 和 `RoughCutResultModel`，后台串接媒体探测、ASR、文案解析、顺序匹配、Retake、Decision 和 Timeline。
- 结果列表已接入真实模型，支持 KEEP/REVIEW/CUT 人工决定、恢复自动决定、撤销和重做。
- 源波形复用 `WaveformGenerator`、`WaveformPyramid` 和 `TimelineSceneItem` 在后台生成并通过 Scene Graph 显示。
- 片段试听复用 `PlaybackEngine` 和音频设备，按源采样边界自动停止，播放头使用播放引擎音频时钟。
- 底部粗剪时间线复用 `TimelineSceneItem`，按实际 KEEP/REVIEW 片段更新。
- XML 导出已直接连接 Phase 1 验收通过的 `XmemlExporter`。
- 尚未取得逐片段 Forced Alignment 时，控制器把 ASR 时间视为不可信，自动决定保持 REVIEW，防止粗定位触发误删。
- 新增 `SubCueRoughCutResultModelTests`；粗剪开发入口 `--roughcut-workspace --smoke-test` 再次通过。
- 粗剪工程通过 Qt SQL/QSQLITE 保存，包含分析版本、完整媒体 SHA-256、原始采样坐标、自动决定、人工覆盖、理由和证据。
- 打开工程时重新计算完整媒体哈希；内容变化时暂停旧切点并要求重新分析。
- 重新分析只迁移规范化文本一致且源区间重叠至少 80% 的人工决定，无法可靠映射的决定不套用并提示复核。
- 已实现 KEEP/REVIEW 时间线的连续源片段调度，片段间按 TimelineEngine 计算的间距等待。
- 结果列表已显示可展开依据所需的证据摘要，并提供工程打开/保存入口。

### Phase 9：辅助识别基础

- 新增可疑区间规划器，仅选择 REVIEW 片段，并按 padding 与相邻间距合并重叠范围。
- 新增主识别、Fun-ASR、Whisper 分路结果结构和保守冲突收敛：任一路失败、空结果或文本冲突均保持 REVIEW，不能升级为 CUT。
- 删除 Fun-ASR Worker 中 `vad_model="fsmn-vad"`，避免隐式下载 VAD 权重。
- 新增 `SubCueRoughCutAuxiliaryTests`。
- `RoughCutController::startAuxiliaryRecognition()` 已把 REVIEW 区间提取为临时 FLAC，依次调用 Fun-ASR 与现有 whisper.cpp 服务；两者共享 `InferenceManager` GPU 门禁。
- Whisper 只有在本地模型文件存在时运行，不触发下载；Fun-ASR 权重不完整时明确记为失败。
- Qwen 主结果、Fun-ASR、Whisper 文本及失败/冲突标志写入工程数据库；辅助失败或冲突继续 REVIEW。

### Phase 10：工程存储与打包基础

- CMake 声明 Qt SQL，粗剪工程使用 Qt 自带 QSQLITE，不引入第二套 SQLite。
- Windows 打包脚本显式收集 QSQLITE 驱动，并把 Qt6Sql 与 qsqlite 加入打包验收。
- 新增 `WorkspaceRouter` 和单个 `AppRouter.qml` Loader，正式启动在 `SubtitleWorkspace.qml` 与 `RoughCutWorkspace.qml` 间切换。
- 两个业务控制器常驻 C++ 层，切换后恢复各自状态和播放位置；任一控制器 busy 时禁止切换。
- 粗剪工程 schema 升级为 v2，按 `analysisVersion` 追加保存分析片段，旧分析及无法迁移的人工决定继续保留。
- Windows 包把 `SubCueInference.exe`、Python 3.11、PyTorch/Qwen/Fun-ASR 依赖限制在 `inference/`；模型目录不复制，主 `SubCue.exe` 的 PE 导入仍不含 Python。
- 最终包约 5.07 GB；已从仓库外工作目录运行打包 Worker，Qwen 检查返回 `cuda: true`。

## 关键决策

- 保留项目现有 C++20、工具链、构建预设、编辑器设置和用户未提交修改。
- 复用 FFmpeg、PlaybackEngine、AudioClock、WaveformPyramid、`IAsrService`、Normalizer 和 FuzzRatio。
- 粗剪内部统一使用原 WAV 采样率下的 `qint64` 采样索引；字幕工程原有时间类型保持不变。
- 模型继续引用 `D:\DSH\SubCue\models`，不复制权重、不自动下载模型。
- 主程序不直接加载 Python/Torch；Python 运行库只能位于专用推理运行目录。
- GPU 同时只运行一个任务，字幕 ASR 和粗剪 ASR 共用同一门禁。
- ASR 必须识别实际录音，不把原稿作为强制转录文本。
- REVIEW 默认保留；CUT 只表示不放入粗剪时间线，源 WAV 始终可恢复。
- 文案偏离、ASR 低置信度、单纯重复或录制较晚不能单独触发 CUT。
- 对齐失败或只有分块粗定位时必须 REVIEW，不能直接裁切。
- XML Exporter 与 AI 解耦，TimelineEngine 不依赖具体模型。
- 正式 Workspace 切换仍留到 Phase 10；当前只提供命令行开发入口。
- 打包保护不能整体删除，只允许已声明的专用推理 Python 运行库。

## 当前验证结果

粗剪相关、字幕核心、启动和打包共 23 项已通过。完整 24 项回归中 `SubCueEditorIntegrationTests` 仍存在既有临时目录/导出与内存阈值失败；它不由本轮路由、辅助识别或打包改动触发，需单独清理旧测试稳定性问题。

- `SubCueRoughCutDecisionTests`
- `SubCueCacheWaveformTests`
- `SubCueRoughCutXmlTests`
- `SubCueRoughCutScriptTests`
- `SubCueRoughCutAlignmentTests`
- `SubCueRetakeDetectorTests`
- `SubCueRoughCutTimelineTests`
- `SubCueRoughCutResultModelTests`
- `SubCueRoughCutProjectTests`
- `SubCueRoughCutAuxiliaryTests`
- `SubCueAsrTests`
- `SubCueSourceHygieneTests`
- `SubCueStartupTest`

测试结果：13/13 通过，总耗时约 0.67 秒。另新增 `SubCueRoughCutStartupTest`，粗剪工作区 QML 启动通过；`SubCuePackagingTests` 通过，确认 Qt6Sql 与 QSQLITE 驱动已进入 Windows 包。Worker Python 语法检查通过。缓存、DOCX Worker 和 ASR 测试需要在沙箱外运行，沙箱内会因文件/子进程限制异常退出。

构建使用 Visual Studio 2026 的 NMake 目录：

```text
out/build/phase2-nmake
```

旧的外部 Ninja 进程 PID 32332/30080 曾处于长时间挂起状态，因此本轮没有结束这些进程，也没有继续复用对应 Ninja 构建锁。

## 未完成事项

### Phase 8 后续

- 完成一小时音频的波形 LOD、列表虚拟化、缩放和连续播放真实验证。
- 为完整媒体哈希增加后台进度，避免超长媒体保存/打开时短暂占用 GUI 线程。

### Phase 9：辅助识别

- 在 Fun-ASR 完整权重可用后执行真实短音频验证；当前 `model.pt.incomplete` 仍按失败处理，不模拟。
- 用真实多片段录音检查合并区间返回文本的逐片段归属精度；当前保守策略会在无法区分时保持 REVIEW。

### Phase 10：正式整合与打包

- 手工验证正式工作区按钮往返切换、状态/播放位置恢复，以及 busy 时按钮禁用。
- 验证字幕与粗剪对同一媒体和相同模型参数复用缓存；当前两者使用同一 `AnalysisCache` 实现，但尚无跨工作区集成用例。
- 修复或重新基线 `SubCueEditorIntegrationTests` 的既有临时目录冲突、导出失败和播放内存阈值后，再取得 24/24 完整回归。
- 执行一小时真实音频的波形 LOD、列表虚拟化、缩放、连续播放和 GUI 响应验收。

## 建议新对话的起始指令

可在新对话中直接发送：

> 请阅读仓库根目录 `progress.md`，继续 SubCue 自动粗剪计划。先完成 Phase 8 控制器、真实结果模型、人工决定/撤销重做、源片段试听和 XML 导出连接。保留当前未提交修改，不要重构现有字幕界面；完成针对性测试后再进入 Phase 9。
