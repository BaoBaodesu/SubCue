# SubCue 自动音频粗剪开发进展

> **AI 配音当前阶段（2026-10-17）**：停止功能扩张，进入核心流程稳定化。当前只集中处理 **Qwen 音质、真实云端合成验收、完整配音项目、基本稳定性**。暂停 L3 自动情绪识别精度优化、L4 高级音色管理及完整自动托管、IndexTTS／GPT-SoVITS 高级音质和多语言优化、极限资源压力／复杂性能优化、专属音色微调训练集成。云端存储、本地素材自动托管和百炼临时空间实验继续移出当前计划。保留已实现功能、历史候选和实际 Token／金额记录；历史章节中与本条冲突的下一步和交付门槛由本条覆盖，暂停不代表验收通过。

> **最新验收（2026-10-18）**：当前核心流程稳定化四项已在固定验收条件下完成。固定参考与 12 句文稿的 Qwen 中文整轨已获用户“整体可用”确认，正式中文参数已落地；完整项目及取消后恢复已通过真实专项。新增“梅比乌斯音频1.flac”参考创建的 sc261018b 云端音色，在原生语速 0.85 下完成真实 12 句工程及修改首句共 13 次合成，复用、候选应用、裁切保留、保存重开及导出一致性通过；用户已确认整轨“整体可用”。实际输入 273／输出 843 Token，折算 ¥0.0105255。旧短参考及原速失败记录保留，不推广为所有文本或音色通过。详见末尾记录，历史待办以此为准。

> **后续维护范围（2026-10-18）**：用户再次明确“保持功能冻结，只修复已有问题”。后续仅处理现有流程中有证据的缺陷，暂停扩展不恢复；原生音色、参考、默认参数与已验收工程继续保留。

更新时间：2026-10-18

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

## Voice Studio 统一正式实施计划

### 2026-10-12 21:18：P0 完成，P1 进行中

用户已确认合并计划 v1.0 并要求实施；两份旧计划仅作为历史依据。本节独立记录 Voice Studio，不覆盖前面的粗剪历史。

#### P0 基线登记

- [x] 整体计划已获确认，开始按阶段实施。
- [x] 保留启动前全部改动：根目录及测试 CMake、已有项目层源码与测试、用户开发文档、`third_party/whisper.cpp` 子模块改动；未重置文件。
- [x] 区分已有存储基础与未实现产品功能。启动前项目层未接入应用；声音库、配音界面、真实 TTS、连续试听及费用功能未交付。

#### P1 本次完成

- [x] `ProjectStore::save` 在成功提交后回传 revision，冲突或取消不修改返回版本。
- [x] Adapter 与 ProjectManager 传递统一 SQLite 的保存结果；旧格式不伪造数据库 revision。
- [x] 统一 SQLite 保存接收取消标志，提交前取消回滚；提交成功按成功报告。
- [x] ProjectManager 新增统一 SQLite 工程的新建入口；已有目标拒绝覆盖，新建失败保留当前会话。
- [x] 增加连续检查点、旧版本拒绝、取消不提交、新建失败保留会话及经理层版本回传测试。

代码范围：`src/core/project/project_store.*`、`project_storage_adapter.*`、`project_manager.*`、`tests/cpp/test_project_store.cpp`。没有新增依赖、修改工作区界面或本轮修改 CMake。

#### 本轮验证

在 MSVC x64 开发环境执行：

```text
cmake --build out/build/windows-release-cuda --target SubCueProjectStoreTests SubCueSubtitleProjectTests SubCueRoughCutProjectTests
ctest --preset windows-release-cuda -R "SubCue(ProjectStore|SubtitleProject|RoughCutProject)Tests" --output-on-failure
```

最终结果：构建成功，3/3 测试目标通过（0.61s、0.23s、0.15s）。本轮未运行完整 CTest、应用手工验收或打包，未生成新的发布包；未运行模型或调用云端 API。历史测试中的音频字节仍只是存储 fixture，不能证明有效 WAV 或音质。

#### P1 未完成与风险

- [ ] 应用／控制器接入、统一 dirty、保存期间新编辑保护及真正的串行存储队列。
- [ ] 完整资产清单、关系数据与兼容迁移、转换副本及来源映射。
- [ ] 两种备份模式、完整恢复和缺失资产定位；当前备份仍只覆盖 Take 音频。
- [ ] 旧格式连续保存后指纹的安全回传；当前调用方需重新读取，不将统一 SQLite revision 机制误用到旧格式。

已核对两个控制器仍直接调用各自序列化器，工作区切换仍受任一控制器 busy 限制。接入时必须协调后台操作及取消等待，不能仅增加一个未使用的共享 Manager 成员或直接放宽切换。

P1 整阶段尚未验收，P2～P8 尚未启动。下一步先接入共享生命周期和存储队列，再完成资产及转换；后续真实 Qwen 验收依赖用户配置的独立 TTS 环境，真实云端验收依赖手动选择和授权批次。

### 2026-10-12 21:29：P1 应用级保存队列接入

#### 已实现

- [x] ApplicationContext 持有单线程项目存储队列；线程按任务启动，没有新增常驻模型或推理环境。
- [x] 字幕与粗剪的实际保存流程接入同一队列，仍使用各自的原格式序列化器、编辑快照、保存指纹与错误提示。
- [x] 提交返回可等待的 Future；控制器取消后等待自己的存储任务，应用上下文退出时等待队列结束。
- [x] 测试验证串行排队、两个控制器排队期间取消不发布文件、字幕与粗剪销毁等待以及销毁后完成通知隔离。

代码范围：`src/app/application_context.*`、`app_controller.*`、`rough_cut_controller.*`、`tests/cpp/test_editor_integration.cpp`。本次未修改 CMake、依赖、QML、编辑器或项目设置；未更改两个控制器的数据所有权及工作区切换规则。

#### 验证与构建异常

初次增量构建后，应用控制器测试以 `0xc0000374`、粗剪控制器测试以 `0xc0000409` 退出。检查发现旧测试对象时间为 2026-10-03，Ninja 对应用控制器测试对象记录的头文件依赖数量为 0；新构造函数与旧测试分配布局混用。

仅更新受影响源文件的时间戳，在本次命令环境设置 `VSLANG=1033` 后重新编译，未修改这些文件内容或永久构建设置。随后同一对象记录到 456 个依赖，包含变更的应用头文件，三个控制器／集成测试目标通过。主程序、工作区路由及存储清理测试的相关对象也重新编译，避免继续使用旧布局。

最终执行（MSVC x64；本次命令环境 `VSLANG=1033`）：

```text
cmake --build out/build/windows-release-cuda --target SubCue SubCueEditorIntegrationTests SubCueStorageCleanerTests
ctest --preset windows-release-cuda --output-on-failure
```

结果：主程序构建成功，32/32 测试目标通过，总耗时 70.17s。新增测试覆盖两个控制器；既有媒体循环回归（预热 3 次后 50 次，最后 10 次私有内存增长阈值 32MiB）和两个启动测试通过。没有测量 Voice Studio 的 UI p95 或真实音质，不能据此声明这些指标达标。

本轮没有生成或更新发布包。`SubCuePackagingTests` 在 staging 不存在时使用已有 `dist/windows-x64-cuda`，因此其通过只代表已有包检查通过，不能算本轮代码的打包验收。未下载模型、安装环境、运行模型或调用云端 API。

#### 未完成与已知边界

- [ ] 共享 ProjectManager 的活动工程切换、控制器生命周期集成、统一 dirty 与保存期间新编辑的完整协调。
- [ ] 统一 SQLite 的应用入口、旧格式文件指纹安全回传及完整资产／迁移／恢复功能。
- [ ] 后续任务和生成结果写入同一存储队列；当前队列只接入两个已有工作区的保存。

当前取消由控制器的原有原子标志传播，排队任务执行时检查取消；关闭必须等待安全退出，可能等待前面的存储操作，尚未实现从队列移除待执行任务。Future 负责完成等待，不能把仅调用 Future.cancel 当作业务取消。工作区目前仍按原有 busy 限制切换，等待后续任务状态拆分。

P1 继续进行，P2～P8 尚未开始，不将保存队列接入误记为共享项目层整体完成。

### 2026-10-12 22:15：P1 资产、转换、恢复及旧格式保存版本

用户要求持续执行至完整完成再统一汇报；已开启持续目标，成功标准仍为确认计划的适用功能与验收，不按本批已实现内容缩减范围。

#### 本批已实现并验证

- [x] 新统一工程使用 SQLite Schema v2，新增资产清单：稳定 UUID、类型、路径、SHA-256、大小与外部引用标记。v1 只读打开不写回，正常编辑保存／资产提交时事务升级，旧任务与 Take 保留。
- [x] 媒体、声音参考、派生样本可以登记到项目；同类同内容同路径复用 ID，内容变化使用新资源版本。声音参考和派生样本复制到项目资产目录，外部媒体允许离线；未知指纹使用空值，不伪造 SHA-256。
- [x] Take 完成时先发布项目拥有的音频，再事务提交引用；清单从 Take 表投影生成结果，不重复存储其权威路径。
- [x] 文件复制采用固定缓冲、复制与源文件复核、内容去重和原子发布；现存同哈希文件损坏时拒绝覆盖。提交失败可能留下完整未引用资产，不能直接删除可能被其他引用使用的文件。
- [x] 备份支持默认外部媒体引用与完整便携两种模式，声音参考、派生样本和 Take 始终包含；关键资产损坏或缺失时不发布目标。完整备份重写资产引用，搬迁后模块通过相同 UUID 解析新位置。
- [x] ProjectManager 增加恢复入口，成功后切换会话；失败、取消和目标已存在时保留当前工程与原件。
- [x] 字幕 JSON／粗剪 SQLite 转换为统一工程副本：新项目 UUID、原项目身份与指纹来源映射、原件不改写、模块精确时间与稳定 ID 保留。
- [x] 模块通过 Adapter 打包／读取原格式的版本化快照；媒体路径先抽离为资产 ID，读取时由资产清单解析，不保留可编辑镜像。首版快照内使用原序列化器的数据编码，保存在统一 SQLite 的模块字段中，原格式数据库仅在 Adapter 临时目录内解码。单个模块限制 64MiB；不是音频或模型 BLOB，后续是否关系化按查询和性能证据决定。
- [x] ProjectManager 增加排他会话锁，同一路径重新打开可复用当前占用，关闭后允许另一管理器接管；不会按锁文件年龄夺取仍活跃的工程。取得新锁后重新读取，失败不切换原会话。
- [x] 两种旧序列化器保留原 API，新增返回实际提交内容 SHA-256 的重载；Adapter／Manager 保存结果回传存储版本。连续保存不需要重新读取后盲目采纳可能被外部修改的文件版本；失败不修改回传值。

代码范围：`src/core/project/project_store.*`、`project_storage_adapter.*`、`project_manager.*`、`project_serializer.*`、`src/core/roughcut/rough_cut_project.*`、`tests/cpp/test_project_store.cpp`。

#### 当前验证证据

针对性存储测试经过新增迁移、离线素材、同资源去重、两种备份、声音原件删除后备份、损坏引用拒绝、恢复会话隔离、模块稳定 ID／微秒／采样坐标／人工决定、便携模块重定位和连续保存／外部冲突测试后通过。

期间修复 Windows QTemporaryFile 发布方式（使用其自身 rename，并在成功后关闭自动删除），以及 SQLite 对空 QByteArray 绑定为 NULL 的问题（明确写入零长度 BLOB 表示未知指纹）。丰富粗剪时间轴测试夹具时曾缺少 Schema 已要求的 recording／decision 字段，补齐有效夹具后通过，未放宽业务校验。

最终命令（MSVC x64，本次命令环境 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda --target SubCue SubCueProjectStoreTests SubCueAppControllerTests SubCueRoughCutControllerTests SubCueEditorIntegrationTests SubCueStorageCleanerTests
ctest --preset windows-release-cuda --output-on-failure
```

最终结果：应用及相关目标构建成功，32/32 通过，总耗时 63.97s。包含既有媒体生命周期与两个启动测试。打包测试仍使用已有 dist 包，本轮未更新发布包，不能据此声明新版本打包完成。

所有新增音频／视频字节夹具只验证存储，不证明有效媒体、TTS 音质、试听或导出已通过。未运行真实 TTS、未连接云端 API、未下载模型。

#### 环境与后续依赖

用户提供独立解释器 `E:/AIModels/TTS/.venv/Scripts/python.exe`。本轮只读元数据检查确认 Python 3.12.14 可执行；qwen-tts、Torch、Transformers、SoundFile、Librosa 的安装元数据均未找到。用户明确选择自行安装并在完成后通知，不得在该环境自动安装，也不改动 ASR 环境。

#### P1 尚未完成

- [ ] 应用控制器真正使用共享 ProjectManager 的活动工程、统一 dirty 与工程切换协调，避免丢弃另一个工作区的编辑。
- [ ] 统一工程的打开、保存、另存、转换、备份、恢复和缺失资源定位 UI，以及草稿提交与整体关闭协调。
- [ ] 文稿／声音快照／选中 Take／编排等配音实体，以及完整任务状态、用量与价格数据（依照后续阶段接口补齐）。
- [ ] 把旧模块编辑后的快照提交接入应用级存储队列；当前 Adapter 模块读取与转换接口已实现，已有两控制器仍使用各自原格式保存。

已知边界：会话锁目前仅约束使用 ProjectManager 的调用方，应用生命周期尚未整体接入；Qt SQL 的 VACUUM 步骤不能通过当前取消标志立即中断，文件复制／校验和发布前支持取消，取消延迟待真实大工程测量；未引用完整资产的清理仍需引用预览与正式清理入口。P1 未整体验收，P2～P8 不提前标记完成。

### Voice Studio：2026-10-12 用户提供的三个 TTS 环境复核

本次结果替代前文“Qwen 环境尚未安装依赖”的历史状态；用户自行安装，未由应用或 Agent 安装。只检查解释器、包元数据、导入、声明依赖及 CUDA 设备，未加载模型权重、生成音频或调用云端 API。

- Qwen：`E:/AIModels/TTS/.venv/Scripts/python.exe`，Python 3.12.14，qwen-tts 0.1.1、Torch／Torchaudio 2.10.0+cu126、Transformers 4.57.3、Accelerate 1.12.0。关键模块及 qwen_tts 导入成功；`python -m pip check` 通过。导入提示系统 PATH 无 SoX、无可选 flash-attn；缺少 flash-attn 会使用普通 PyTorch 路径，SoX 是否影响当前 12Hz Base 流程仍需实际验证（发现直接使用位于 25Hz tokenizer）。不能据此认定真实克隆已验收。
- Index：`E:/AIModels/TTS/.venv-index/Scripts/python.exe`，Python 3.11.16，indextts 包版本 2.0.0、Torch／Torchaudio 2.8.0+cu128、Transformers 4.52.1、Accelerate 1.8.1，主要版本与本地 pyproject.toml 一致。`uv --no-cache pip check --python <该解释器>` 通过，`from indextts.infer_v2 import IndexTTS2` 成功。环境未装 pip，但 uv 检查可用，不属于推理依赖缺失；包版本不用于判断实际下载权重的型号。
- GPT：`E:/AIModels/TTS/.venv-gpt/Scripts/python.exe`，Python 3.11.16，Torch／Torchaudio 2.7.0+cu126、Transformers 4.57.6、Librosa 0.10.2、NumPy 1.26.4。关键模块导入成功，本地 requirements.txt 的生效要求未发现缺项或版本偏差。pip check 提示 faster-whisper 1.2.1 要求发行包 onnxruntime，当前安装的是 onnxruntime-gpu 1.31.0；实际 onnxruntime 与 faster_whisper 导入成功，ORT 列出 CUDA／TensorRT／CPU Provider。属于发行包声明差异，不直接判定运行失败，也不自动叠装 CPU／GPU 两套 ORT。
- 三个环境均报告 CUDA 可用，设备为 RTX 3060 Ti，约 8GiB 显存。未执行张量／模型推理，不代表显存、速度或音质验收。

初次导入触发 Numba 缓存权限错误；仅为本次探测设置项目临时 NUMBA_CACHE_DIR 后 Qwen／Index 导入成功，没有修改环境、模型仓库、系统 PATH 或编辑器设置。实际 Worker 应使用可写的应用缓存位置。模型辅助文件完整性、离线加载及真实音质仍待 P6／L1 验证。

### Voice Studio：2026-10-12 23:35 P1 应用生命周期接入（阶段仍在推进）

#### 本次已实现

- [x] 应用创建 ProjectSession，连接已有 ProjectManager／Adapter 和唯一串行存储队列；字幕与粗剪仍拥有原编辑模型，不建立可编辑副本。
- [x] 新工程默认为统一工程，先生成稳定 UUID，首次保存才创建 SQLite；字幕与粗剪共同保存到同一个模块检查点，回传 revision 后可连续保存，生成历史不被重建删除。
- [x] 共享新建、打开、保存、另存及整体关闭协调；旧 JSON／粗剪 SQLite 通过同一个会话入口和固定 Adapter 打开、保存原格式。只读打开不写文件，未知有效元数据保留，正常保存写入身份，外部文件版本冲突不覆盖。
- [x] 保存捕获冻结模型后允许继续改稿及切换工作区；仅更新保存基线，保存期间的新编辑保持 dirty。标题显示所有工作区的未保存状态。成功保存使用状态提示，不弹窗打断编辑；失败提供原因和重试入口。
- [x] 统一工程另存复制已提交历史／资产，UUID 保持不变；旧工程转换副本生成新 UUID，保留原件和来源映射。打开／转换前准备失败保留当前工作区内容，销毁先取消并等待队列任务。
- [x] 增加备份两种模式、恢复到新位置及旧工程转换的界面入口；备份明确只包含已保存编辑及已提交历史。大外部媒体默认引用，已经项目拥有的媒体始终复制。
- [x] 文稿源文件归项目派生资产，第一次保存和另存后更新对应路径；不替换仍在编辑的正文，也不继续依赖原文稿位置。字幕资源映射存放于模块包装层，避免覆盖原格式有效自定义 assetId 字段。
- [x] 工程转换源路径直接传本地 QString，保留中文、空格及 # 等文件名，不手工拼接 URL。

新增 `src/app/project_session.*`；扩展 `application_context.*`、两控制器、main、AppRouter、项目层和既有测试／CMake。保留用户文档、既有修改及 whisper 子模块改动，没有变更编辑器设置、ASR 环境、模型或云端服务。

#### 验证证据

新增集成槽覆盖两模块往返、稳定 UUID、连续保存、Take 历史另存、保存期间新编辑和工作区切换、失败打开、排队保存关闭取消，以及旧 JSON／粗剪 SQLite 只读打开／外部冲突／另存／转换。文稿原件与旧工程资产中的文稿副本删除后，新另存工程仍可继续保存。

新增存储槽覆盖准备版本变化时不切换当前会话及不遗留目标锁、默认引用备份仍包含此前已拥有媒体、资源 ID 去重。原有自定义 assetId 字段保留也有回归断言。

真实 QML 窗口测试绑定 ProjectSession，验证跨视图 dirty 标记、保存后清除、成功保存不进入模态、无 QML 警告，并查看生成截图。CTest 离屏平台的截图没有中文字体；另外在正常 Windows 图形平台重跑该槽（3 passed，1.917s），确认中文文字、状态和无模态布局正常。仍只有字幕／粗剪界面，不代表配音工作区已实现。

最终命令（MSVC x64，本次命令环境 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

结果：全部当前目标构建成功，**32/32 通过，67.21s**。包含新增共享工程／旧格式测试、既有媒体生命周期回归、两个启动测试及本次构建新生成的 CUDA 包验证（不再依靠旧 dist 包）。

中间失败分别来自受限运行下 Qt 临时目录权限、旧格式另存接管时尚未释放目标读写锁，以及文稿仍按外部媒体引用；已采用具备临时目录权限的测试运行、发布后释放目标锁再接管、文稿归项目资产解决。另发现旧工程无 UUID 时重复准备读取会产生不同临时身份，已统一为实际接管会话的身份。

CUDA 包测试运行后实测 **5,099,543,144 bytes／4.749GiB**；Torch **3.547GiB／74.69%**，cuBLAS **1.006GiB／21.18%**（cuBLAS 位于 Torch 内容内，份额不可相加）。满足本阶段 4.8GiB 目标，未把三个外部 TTS 环境或模型复制入包。没有新建 CPU 构建／CPU 包，本轮不声明 CPU 包验收。保留现有 dist，验证后使用清理脚本预览并移除本次 package 暂存。

#### 未完成与后续依赖

- [ ] P1 配音关系实体、声音快照、选中 Take、编排、完整任务／用量／价格等数据尚未补齐；与 P2～P4 的具体契约一起迁移，不把现有模块检查点当作配音数据已交付。
- [ ] P2 完整任务状态、共享 GPU 批次调度、受管理 TTS 进程、恢复和 Mock 尚待实现。
- [ ] P3～P8 声音库、第三工作区、人工裁切保护、定向 JSON 更新、连续试听／导出、本地真实克隆、Flash 费用／软预算和最终性能／音质验收尚未完成。
- [ ] 后续 Index／GPT 等仍保留原计划范围和门槛；环境复核成功不证明模型离线加载与音质通过。

已知边界：旧格式只能提交原模块的数据，检测到另一个工作区未保存编辑时拒绝旧格式保存并要求转换，避免无事务双写或静默漏存；资产预发布／已登记但未引用的资源仍需引用预览清理；发布后若工程接管失败，需补充可追踪目标及恢复动作；SQL VACUUM 的即时取消边界仍保留。当前测试音频存储夹具仍不是有效 WAV，本次没有真实 TTS／云端调用或正式 UI p95 测量，P1 不整阶段勾选完成，完整目标未完成。

### Voice Studio：2026-10-13 00:15 P2 任务状态、恢复与共享 GPU 基础（阶段未完成）

#### 本次已实现

- [x] 统一 SQLite 扩展为 Schema v3，增加 job_details 保存 Provider／批次／重试来源、输入指纹、诊断、结果描述、用量和时间；jobs 仍是输入与状态的唯一来源，没有另建任务队列数据库或可编辑镜像。
- [x] v1／v2 读取和任务分页不写回；首次实际写入在同一事务迁移，旧任务按原输入回填指纹，未知历史时间保持 0、不伪造用量。保存 revision 冲突时连迁移一起回滚。
- [x] 覆盖 queued／running／validating／completed／failed／canceled／interrupted／outcome_unknown；状态条件更新拒绝过期转换，正常 Provider 必须先运行及记录结果，再经过 Take 提交入口完成。
- [x] 任务输入不可覆写；手动恢复／重生成创建新任务，保存 retryOf 来源。未明确标记用户手动确认的恢复请求被拒绝，不自动重发可能付费的请求。
- [x] 用量 known／unknown 与任务及音频状态分离。known 必须具备有效非负输入／输出 Token；取消后的迟到用量可补记，但不复活任务／提交音频，已有实际用量不会被后来的未知或不同值覆盖。
- [x] 提供任务分页读取及恢复状态接口。取得工程会话后才执行恢复：排队／本地运行／校验阶段中断，云端正在请求则标为结果不确定；保留完整输入、结果和已知用量，不增加编辑 revision、不执行生成。恢复失败不释放原会话或切换现有工作区。
- [x] 复用已有 InferenceManager，替换其门禁实现为可取消 FIFO 租约，并支持同线程批次内嵌套调用。整个批次租约释放前，后续 GPU 任务保持排队；不另建一套与 ASR 无关的调度器。
- [x] 本地 ASR 实际进程调用已使用可见队列，应用顶部可查看运行／等待任务；文稿解析明确不占 GPU 门禁。窗口退出仍由任务所有者取消并等待进程，队列本身不创建线程或常驻模型。
- [x] Worker 输出固定块读取：stderr／事件 stdout 尾部各 64KiB、非事件 stdout 上限 1MiB，JSONL 待解析行上限 1MiB；超过边界丢弃整个超长行，保留后续有效事件，明确标记 outputTruncated，不随日志时长保留完整输出。

代码范围：`project_store.*`、`project_manager.*`、`project_session.cpp`、`inference_manager.*`、`local_python_asr_service.cpp`、`script_document.cpp`、应用上下文／main／AppRouter，以及存储、ASR 和编辑器集成测试。

#### 验证证据

- 存储目标本次针对性运行：27 passed，覆盖真实 v2 表结构读取不变、迁移失败回滚、状态拒绝、不可变重试、取消后费用补记、分页、恢复不会启动任务及失败恢复不切换原会话，并保留原有资产／备份／旧格式测试。
- GPU 门禁针对性测试覆盖 FIFO、取消排队、整批持有、嵌套请求及租约释放；这是模拟租约占用，尚未证明真实 ASR 与 TTS 显存或音质。实际 PowerShell／cmd 子进程测试覆盖失败退出、大量 stdout／stderr 和 1MiB+1 字节行边界；未使用 Torch 或真实模型。
- 应用集成验证实际打开工程后持久化中断状态及可见提示；真实 QML 窗口绑定 GPU 队列，验证两项菜单、排队位置、取消后隐藏、跨工作区保存不弹模态。正常 Windows 图形平台该窗口槽 3 passed，2.079s，中文截图已查看；随后日志边界修复不改变界面。

最终命令（MSVC x64，临时命令环境 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

最终结果：**32/32 通过，71.61s**，包含全部当前测试、既有生命周期回归、两个启动测试及当前新构建 CUDA 包。包运行后实测 **5,099,561,064 bytes／4.749GiB**；Torch **3.547GiB／74.69%**，cuBLAS **1.006GiB／21.18%**（重叠占比，不能相加）。没有加入外部 TTS 环境／权重，没有新建 CPU 构建或包；保留已有 dist，按清理脚本预览后移除本次暂存及探测日志。

#### 明确未完成

- [ ] P2 TtsCapabilities／TtsRequest／TtsResult、受管理 TTS Worker、离线模型预检、Mock 故障闭环、项目任务取消／切换协调及可手动继续的任务恢复列表尚未完成。
- [ ] 当前可保存调用方提供的 32 字节输入指纹；仅包含真实生成条件的哈希规范及校验，要由即将接入的 TtsRequest 实现，不能把旧输入整 JSON 哈希当成该规则已验收。
- [ ] finish 的既有存储兼容接口仍只检查非空文件；正常 Provider 的音频硬错误／有效 Take 验证待 P5，不把 validating 状态或存储字节 fixture 当成真实音频质检。
- [ ] 人民币价格快照、折算、预测与软预算尚未实现；本次只完成实际 Token 的独立记录基础。
- [ ] P1 剩余配音实体及 P3～P8 产品功能、真实模型／云端／人工试听／性能验收仍未完成。L1～L4 与 R 的原范围和前置条件保持。

本次没有安装依赖、运行 TTS 模型、下载文件或调用云端 API，没有提前勾选 P2 或完整目标完成。

#### Voice Studio 后续续接重点

先读取本章节及之前已确认的完整计划，继续 P2：补薄 Provider 契约、有效生成条件哈希、受管理 JSONL TTS Worker／Mock、任务所有者与工程代次取消等待，再进入声音库和文稿界面。复用全局推理门禁及串行存储队列；关闭等待不能依赖被同一存储任务阻塞的回调，避免互相等待。三个环境已完成基础复核，模型真实验收仍须在各阶段满足条件后执行；云端需用户配置及手动授权。

### Voice Studio：2026-10-13 P2 TTS 输入契约增量（未接入运行链路）

- [x] 新增 `src/core/tts/tts_types.h`：TtsCapabilities、不可变 TtsRequest 和 TtsResult。请求快照保存项目代次、片段版本、声音版本、参考资产、实际型号与有效参数；结果区分原生规格、请求 ID、已知／未知用量及 Mock 标记。
- [x] 生成哈希仅覆盖朗读内容、语言、Provider／模型版本、不可变声音版本、参考内容指纹与转写、云音色／地域及有效参数。工程代次、片段身份／版本和资产位置不进入音频失效哈希；名称、备注、增益、停顿等编辑信息不进入该契约。
- [x] 基础能力校验拒绝身份缺失、型号／语言不符、缺失参考或转写、混用本地参考与云 Voice ID、云端地域缺失、未知参数及空值／非标量参数。工程代次以字符串保存，避免 quint64 经 JSON 数字传输丢精度。

验证：构建 `SubCueProjectStoreTests`，完整目标 28 passed；随后补非标量参数边界并重新构建，`ctest --preset windows-release-cuda -R "^SubCueProjectStoreTests$" --output-on-failure` 1/1 通过，0.93s。新增断言覆盖相同生成条件不受编辑身份变化影响、改朗读会失效、Base 未声明 instruct 参数不能伪支持、云端不能隐式接受参考资产。该头文件目前只由测试使用，不代表生产 Provider 已接入；未改变打包内容，包体沿用此前 P2 已核验记录，没有在本轮重做全量／打包／真实模型验收。

尚需补模型具体参数类型／范围与默认值规范、恢复快照解析、受管理 Worker、实际 Provider／Mock 运行和项目任务协调；本次基础能力校验不能替代这些实现。P2 及完整目标仍未完成，未安装依赖、下载模型或调用 API。

### Voice Studio：2026-10-13 P2 Python TTS Worker 协议增量（生产链路未接通）

- [x] 新增 `tools/tts_python_worker.py`，单批次进程读取有界 JSON 请求，逐句输出带 jobId／inputHash 的结果或错误；整批输入先校验，重复任务、不支持的控制、非法路径或缺少参考拒绝执行。
- [x] 强制 Hugging Face／Transformers 离线；本地 Qwen 12Hz 0.6B Base 预检检查配置、主要权重文件及安装元数据，明确返回 weightsLoaded=false。缺少文件／依赖直接报错，不安装、不下载、不回退。
- [x] 写入 Qwen Base 的懒加载路径，采用参考音频／转写、SDPA、批次内最多四组克隆提示复用；仅暴露已校验 seed，其它生成参数待具体范围设计。SDK 输出重定向到 stderr，避免破坏 JSON 协议。该真实生成分支尚未运行，不宣称音质／显存或完整离线加载通过。
- [x] Mock 明确标记，不冒充真实 TTS；支持音调 WAV、静音、非法音频、OOM、崩溃及延迟场景。正常 Mock WAV 固定块写入，不保留整批 PCM。临时文件通过不覆盖目标的原子发布产生结果，失败清理自身临时文件。
- [x] 新增 `tests/python/test_tts_worker.py`，7 个测试覆盖正常可解码 Mock WAV、重复发布不覆盖、整批预检、未知指令、OOM 后下一句、真实子进程退出及 kill 后等待、不存在模型拒绝。进程所有权目前由测试的 subprocess 管理，应用 C++ 管理尚未接入。

验证命令（仅本次进程 TEMP／TMP 指向项目 .test_tmp，未改系统设置）：

```text
E:/AIModels/TTS/.venv/Scripts/python.exe -B tests/python/test_tts_worker.py -v
```

最终 7/7 通过，1.270s。最初受限 Windows 临时目录运行出现权限错误；使用项目临时目录及获准的本地子进程验证后通过。另通过 stdin 调用 check，实际目录 `E:/AIModels/TTS/models/Qwen3-TTS/Qwen3-TTS-12Hz-0.6B-Base` 返回 Base、已安装包版本及 weightsLoaded=false；没有导入 Torch／Qwen 模型或加载权重。

本轮没有修改 CMake／打包清单，Worker 尚未被生产代码使用，故未把它作为未使用资源复制入发布包。没有新增依赖、自动安装、模型下载、真实配音或云端 API 调用。未重跑 C++ 全量／包体，沿用上一阶段明确注明的历史证据。

下一步必须接入 C++ Provider（已有 InferenceManager 的批次租约、取消及退出等待）、请求快照恢复与模型参数规范、项目任务所有者及代次隔离，并将本协议测试注册到适当验证入口；此后更新实际运行文件清单并验证打包。P2 及完整计划仍未完成，不能将独立 Worker 文件和 Mock 测试当作应用配音功能交付。

### Voice Studio：2026-10-13 P2 C++ 本地 Provider 与 Worker 连接（应用任务层仍待接入）

- [x] 新增 `local_tts_provider.*`，编译进 Core。固定 Qwen Base／明确标记 Mock 两种本地实现，提供句级任务与批次报告，不引入插件或独立调度器。
- [x] C++ 校验输入能力、UUID、seed 类型／范围、参考路径、批次规模及协议大小；将冻结请求传给独立 Python／JSON Worker。整个进程使用已有 InferenceManager GPU 租约、取消和退出等待，Provider 本身不创建线程。
- [x] 每句输出通过 ID、输入指纹、路径、Provider 标记、非空文件和原生规格检查；拒绝陌生／重复事件、错误指纹和不完整结束。允许单句失败后其余候选保留，Mock 不会冒充真实结果。
- [x] 成功结果可同步回调用于后续实时存储。回调失败保留已生成文件及已收集结果，报告错误；协议错误／提交失败立即请求进程停止并等待退出，避免继续占用剩余批次 GPU。用户取消后不再派发结果回调。
- [x] 新增 C++→Python 实际子进程测试，覆盖 Mock 成功、OOM、非法音频规格与局部成功、Worker 崩溃、回调异常、错误协议后长睡眠及时终止，以及取消后的进程／队列释放。

针对性槽 `localTtsProviderRunsAndContainsMockFailures` 通过；最后协议停止和取消修复后，整个 SubCueAsrTests 目标再次通过（1/1，1.88s）。这些是实际子进程／Mock 验证，未加载 Qwen 权重，不是实际配音音质验收。

最终命令（MSVC x64，临时 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

最终 **32/32 通过，68.17s**，包含所有当前目标、新 Provider 槽、既有媒体生命周期回归、两个启动及当前新 CUDA 包测试。运行后实测包 **5,099,561,576 bytes／4.749GiB**；Torch **3.547GiB／74.69%**，cuBLAS **1.006GiB／21.18%**（重叠占比不可相加）。未新建 CPU 构建／包，未复制外部模型或 TTS 环境；保留 dist，清理脚本先预览再移除本轮暂存及专用日志。

当前 Provider 由测试调用，尚无配音工作区／项目任务所有者入口；Python Worker 仍通过显式源路径运行，未把未被应用使用的脚本复制入包。下一步接通应用任务所有者、请求恢复解析、每句开始／进度和持久状态协调、工程代次隔离／关闭等待，再补实际运行文件及打包清单。源代码存在不等于用户已可配音。

原生规格检查尚不等于完整音频解码质检；损坏但伪造有效规格的文件仍须 P5 的解码／完整性／非有限值检查阻断。模型版本身份／完整离线依赖及真实 Base 生成、人工试听、显存／速度待 P6。P1 配音实体、P2 剩余功能及 P3～P8、L1～L4 与 R 原范围不缩减。本轮未下载、安装、加载模型或连接 API，完整目标未完成。

### Voice Studio：2026-10-13 P2 快照恢复与逐句开始状态增量

- [x] TtsRequest 增加 fromSnapshot：恢复完整生成条件、精确十进制版本／工程代次，校验字段类型、协议版本、参考指纹编码、实际能力和输入哈希。被改稿、损坏或旧格式不完整的任务不能伪装成原输入恢复；原记录保留，需用户创建新任务。
- [x] Worker 每句发送带输入指纹的 job_started。C++ 拒绝重复开始、陌生输入或没有开始事件的结果；开始状态回调失败会立即停止并等待进程。
- [x] 测试连接实际 Mock Worker 与 SQLite：入队冻结输入 → 句级开始更新 running → 结果记录 validating → Take 与 completed 事务提交，保留候选历史。该连接在测试中使用单线程存储，并不代表应用串行队列／项目任务所有者已集成。
- [x] 恢复测试覆盖快照往返、偷偷改稿拒绝、数字代次拒绝、quint64 最大值精确恢复。针对性 CTest 的存储及 ASR 两目标 2/2 通过，2.81s；Python Worker 协议 7/7 通过，1.264s。

代码范围：`tts_types.h`、`local_tts_provider.*`、Python Worker 与对应 C++／Python 测试。最终重新构建所有目标，`ctest --preset windows-release-cuda --output-on-failure` **32/32 通过，68.67s**，含新构建 CUDA 包和两启动测试。包运行后实测 **5,099,561,576 bytes／4.749GiB**，Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠，不相加）；未新建 CPU 包或复制外部环境／模型，验证后预览并清理暂存，保留已有 dist。Worker 仍未被应用任务入口使用，不把未使用脚本加入包。

未自动执行恢复、加载权重、下载安装或调用 API。P2 的应用任务所有者／工程代次隔离／关闭协调、恢复列表、资源和真实引擎验收仍未完成，完整范围继续保留。

### Voice Studio：2026-10-13 P2 应用任务所有者增量（产品入口未完成）

- [x] 新增 TtsTaskController，应用 main 创建并暴露对象。每个批次由受管理单线程池及 Future 持有，不使用 detach；销毁先取消并等待后台与存储，再释放资源。
- [x] 入队冻结输入、句级 running、结果／候选及失败／取消均通过现有 ApplicationContext 串行存储队列提交。后台只等待该队列的操作，不在存储任务内等待生成，避免互相等待。
- [x] 接通 ProjectSession 的切换通知：新建、打开／转换／恢复、另存与关闭前取消并等待旧批次。请求必须匹配当前工程 UUID／代次；GUI 候选及完成通知通过批次 epoch 和 QPointer 隔离，旧批次不能向新工程界面提交。
- [x] 参考音频从本项目资产清单解析，要求已拥有的参考／派生资产及匹配指纹，不直接使用全局库路径；项目身份变化时不入队或提交结果。
- [x] 候选提交不反写正文、不自动选择或改变编辑 dirty。持久文件使用项目资产位置，不能把马上删除的 Worker 临时路径发给界面。提交失败保留可追踪暂存，普通取消清理未提交文件，完成候选保持历史。
- [x] 增加媒体容器探测，保存 container_checked 诊断；这不是完整 PCM／非有限值／完整性质量验收，P5 仍必须实现，不能仅凭 completed 当作可应用 Take。

测试槽 ttsTaskOwnerPersistsAndCancelsBeforeProjectChange 验证真实 Mock 子进程、后台改稿保持、持久候选可读、切工程取消／退出／迟到通知隔离，以及存储队列阻塞时销毁等待而不依赖 GUI 回调。针对性槽 3 passed，0.784s。新增资产解析曾因捕获容器 const 导致编译失败，已修复为受后台任务独占的可变捕获并重构建通过，没有放宽校验。

最终（MSVC x64，临时 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

**32/32 通过，77.81s**，包含新增所有者槽、既有生命周期回归、新 CUDA 包及两启动测试。包运行后 **5,099,564,648 bytes／4.749GiB**；Torch **3.547GiB／74.69%**，cuBLAS **1.006GiB／21.18%**（重叠占比）。未新建 CPU 包、复制外部环境或模型；保留 dist，预览后移除本次暂存与日志。

未完成：配音工作区的生成／恢复列表入口、完整声音及文稿实体、批次持久分组和细化进度、运行配置与 Worker 发布定位／清单仍待接通。当前 start 是 C++ 接口，由测试驱动，用户尚不能在应用完成配音。main 创建空所有者不加载模型；Worker 尚未成为用户可用的发布功能，未添加未使用脚本到包。

风险：切工程前同步等待可能超过 UI p95 目标，须在后续协调／实测阶段处理；提交失败暂存的正式恢复／清理入口待完善；完整音频质检与真实模型／音质尚未验收。P2 不整阶段勾选，P1 剩余实体及 P3～P8、后续 L1～L4／R 保留。未下载安装、加载权重或调用 API，完整目标未完成。

### Voice Studio：2026-10-13 P2 持久批次与任务历史界面增量

- [x] 每个应用配音批次使用稳定 UUID，保存到全部句级任务的 batchId；ProjectStore.jobs 支持按批次筛选和分页，旧未分组工程继续兼容只读查询。
- [x] TtsTaskController 增加异步任务记录读取，通过同一存储队列加载，不在 UI 线程访问 SQLite。只缓存当前只读页，读取错误可见；切工程／销毁等待读取结束，清空页面并拒绝旧页回调。
- [x] 工程菜单接入配音任务记录弹窗，显示状态、朗读输入和失败原因，支持上一／下一页、刷新、查看单个批次及恢复全部批次视图。只读打开记录不会启动模型、下载或重发请求。
- [x] 对中断／结果不确定给出不自动重发说明；completed 显示“已生成候选”，不冒充已应用。该弹窗目前不提供手动重试／下载恢复，相关操作仍须继续实现。

验证：存储批次过滤槽 3 passed；真实窗口与所有者两槽 4 passed，2.394s。最终正常 Windows 窗口槽 3 passed，2.131s，中文任务弹窗／批次筛选截图已查看。中间一次窗口运行出现原粗剪 onUserSeeked 参数注入弃用警告，后一次未出现；未把该原有提示当作新任务界面失败，也未据此声明所有应用 QML 永远无警告。

最终命令（MSVC x64，临时 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

最终 **32/32 通过，69.41s**，含新窗口断言、所有当前目标、原有生命周期、新 CUDA 包和两启动测试。包运行后 **5,099,604,584 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。新增 QML 组件是应用已使用的资源；没有复制外部模型／环境，未新建 CPU 包。保留 dist，预览后移除暂存及专用日志。

本次未加载权重、安装、下载或调用 API。P2 的人工恢复操作、生成入口／配置／Worker 发布定位和细化进度未整体验收；P1 配音实体、全局声音库、正文／Take／编排及其余 P3～P8、L1～L4／R 范围仍保留，完整目标未完成。下一阶段优先接声音库与项目声音快照，再让文稿／生成入口使用现有任务所有者，不把只读任务历史当作完整恢复交付。

### Voice Studio：2026-10-13 P1 项目声音快照与 P3 全局声音库存储增量

- [x] 项目 SQLite 升级到 v4，新增不可覆盖的声音版本快照；本地参考复制到项目资产，纯云音色允许不带参考文件。旧 v1～v3 继续读取、按正常写入迁移；资源复制不改变编辑 revision。
- [x] 本地 TTS 入队要求对应项目声音版本、参考资产、指纹和需转写型号的参考文本一致，拒绝借用其它声音版本的参考。
- [x] 新增固定全局 VoiceLibrary SQLite：稳定声音 UUID、名称、停用／恢复、不可变版本、参考文件以及分页查询。重命名、停用与后续新增版本不改变项目副本；目前不提供物理删除接口。
- [x] 全局库通过明确复制操作接入 ProjectStore，不使用跨库外键、引用计数或双写同一业务记录。参考原件删除后项目仍保有副本；全局参考缺失时新复制明确失败。
- [x] 提取现有资产流式复制／哈希校验／完整发布到 asset_file，项目与全局库共用，保留原来的固定缓冲和取消行为。复制后事务／写入失败的未引用完整文件清理仍待正式入口，不自动删原件。
- [x] 声音版本元数据拒绝常见密钥字段（含嵌套对象／数组），凭据只存引用；这是存储基础检查，Provider 绑定的完整类型校验仍需后续接入。

验证：SubCueProjectStoreTests **30 passed，1.039s**，其中新增两个声音存储槽覆盖不可变版本、重命名／停用、取消、分页、纯云版本、参考复制及项目便携性。使用的是存储字节 fixture，不代表可解码音频或音质验收。受限权限直接运行 Qt 临时目录测试时 create 失败，授权运行后通过；未把受限探测失败归为业务修复。

最终命令（MSVC x64，临时 VSLANG=1033）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
```

最终 **32/32 通过，68.88s**，含既有媒体生命周期、新 CUDA 包及两启动测试。包运行后实测 **5,099,605,608 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠占比不可相加）。未创建 CPU 包、复制外部 TTS 环境或模型。按清理脚本先预览再移除本次 package 暂存，保留已有 dist。

本次环境重新导入检查确认三个解释器 CUDA 可用；Qwen／Index 依赖检查通过，IndexTTS2 导入成功；GPT 本地 requirements 无缺项或偏差，onnxruntime-gpu／faster-whisper 导入成功，pip check 仍有发行包名 onnxruntime 差异。Qwen 普通导入耗时较长并主动停止；仅设置本次可写 NUMBA_CACHE_DIR 和离线变量后导入成功。SoX 缺失与可选 flash-attn 提示保留，未安装或修改环境，未加载模型权重。

P3 尚未完成：全局库当前仅核心存储，应用异步所有者、声音库界面、批量切分／转写／质检、审核、默认参考及非破坏性 A/B 处理待实施。文稿、Take 选择／裁切／编排、费用、连续试听／导出、真实引擎及 P8 性能验收均仍保留完整范围。完整目标未完成，不把本增量标为整阶段交付。

### Voice Studio：2026-10-13 P3 声音库应用所有者与手工建档界面增量

- [x] 新增 VoiceLibraryController，按需初始化全局库，所有存储操作走现有串行队列；启动不读取声音资产、不加载模型。自身持有 Future／取消标志，切工程与销毁取消并等待，代次拒绝迟到界面通知。
- [x] 工程菜单接入声音库弹窗，沿用主题，提供档案新建／重命名／停用、版本查看、本地参考加转写、Flash 北京系统音色／已有 Voice ID 的版本保存与复制到当前项目。
- [x] 新版本不可覆盖旧版本；复制仅提交项目资源，不自动更改文稿声音、选中 Take 或 dirty。全局重命名与停用不改项目副本。云音色保存仅本地持久化，不上传、注册或调用云端。
- [x] 本地入口拒绝 URL、空转写及无音频流文件，保存原件独立副本，版本明确 pending。TTS 所有者拒绝 pending 参考进入生成；当前容器探测不代替完整解码、指标检查或人工审核。
- [x] 测试覆盖异步建档、云版本、项目复制／dirty、重命名／停用、拒绝 URL、排队取消、取消后拒绝迟到回调。正常 Windows 窗口测试实际打开弹窗并显示中文角色与云版本，截图 `.test_tmp/voice-library-dialog.png` 已查看。

针对性命令：`SubCueEditorIntegrationTests.exe voiceLibraryOwnerMaintainsProjectCopies qmlSharedProjectWindow -o <临时日志>,txt`，**4 passed，3.043s**。完整重新构建后 `ctest --preset windows-release-cuda --output-on-failure` **32/32 通过，69.21s**，含当前所有者／窗口测试、既有媒体生命周期、新 CUDA 包与两启动测试。构建期间有 Qt 生成代码 C4702 警告，编译成功；本次窗口测试未报告 QML 警告。

包运行后实测 **5,099,688,552 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。没有新建 CPU 包、下载安装或复制模型／TTS 环境；预览后清理本次 package 暂存，保留已有 dist。应用已使用新 QML 资源，不增加未使用的发布资源。

未完成：界面当前最多显示 200 个活动档案与选中档案最新 50 个版本，需补显式分页；停用恢复界面、声音预设／完整 Provider 绑定、默认参考、批量切分／中文转写／去重／质量审核、试听与非破坏性派生处理待实施。参考探测仍在存储队列中，完整音频处理需要独立的有界任务调度；切工程同步等待的 UI 响应实测仍待完成。P3 未整体勾选，P4～P8 与后续完整范围保留；本次未加载权重或进行真实音质／云端验收。

### Voice Studio：2026-10-13 P3 声音库分页与停用恢复增量

- [x] 应用声音库按页读取 50 个档案及 50 个版本，增加各自上一／下一页与较新／较旧版本入口；切换档案不重置档案列表页，版本页重新从首屏读取。
- [x] 增加显示停用档案、停用标签和恢复入口。恢复仅变更全局档案状态，旧版本不改写、不重新生成；项目副本仍独立。
- [x] 页偏移和停用筛选随成功读取回传，失败／取消不冒充页面更新；显式负偏移规范为 0。分页只保留当前页，不一次加载完整版本历史。
- [x] 测试用 52 个档案、51 个版本验证跨页无重复、末页条数、筛选、恢复、版本保留及负偏移；同时复核已有所有者取消和窗口渲染。

针对性验证：`SubCueEditorIntegrationTests.exe voiceLibraryPagingAndRestore voiceLibraryOwnerMaintainsProjectCopies qmlSharedProjectWindow -o <临时日志>,txt`，**5 passed，3.795s**；更新后的声音库截图已查看，分页按钮与停用筛选可读。完整重新构建及 `ctest --preset windows-release-cuda --output-on-failure` **32/32 通过，70.24s**，含既有生命周期、新 CUDA 包及两启动测试。

包运行后实测 **5,099,695,208 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未创建 CPU 包、复制外部环境／模型或调用云端；保留 dist，使用清理脚本预览后移除本次 package 暂存。

已检查现有 Demuxer／Decoder／AudioResampler 可复用。后续流式质检需要明确读错、解码错与正常 EOF 的区分，加入取消和本地协议边界；当前仅容器检查仍不足以判定完整性、非有限值或削波，不宣称已完成质检。P3 默认参考、样本审核／批处理／转写／派生试听，以及文稿、生成、费用、试听导出与 P8 验收仍未完成；总体范围保持不变。

### Voice Studio：2026-10-13 P3／P5 流式音频质检及候选提交保护增量

- [x] 新增 AudioQuality，逐帧检查原生样本，统计采样率／声道／每声道样本数、峰值、RMS、各声道最大直流偏移、近满幅比例与静音比例。保留当前帧与固定通道统计，不保留整篇 PCM；不重采样、裁切或改变原文件。
- [x] 完整读取及解码错误、损坏数据包／帧、空样本、非有限值／异常幅度、规格变化和超限明确失败；近满幅、高静音、低电平与直流疑点作为试听提示，不能据此承诺自然度或读音正确。
- [x] Demuxer 保留原 open 行为，增加带取消中断的本地入口，限制为 file 协议，避免本地音频质检隐式访问嵌套网络流。取消标志由调用者持有到 close。
- [x] 质检前后校验文件 SHA-256。生产 TTS 提交传入质检指纹和取消标志，资产复制校验指纹不一致时回滚；保留旧 finish API 供旧基础调用兼容，其本身不代表已执行音频质检。
- [x] TTS 所有者在后台执行质检，比较 Provider 规格与实际样本；硬错误／全静音不产生可用 Take，记录明确失败及用量，后续句继续生成。正常候选保存完整指标与指纹，任务记录界面显示质量提示，仍不自动应用。
- [x] 有效 PCM16／Float WAV 测试覆盖样本数、峰值／RMS、削波／静音提示、NaN、截断、空音频、URL 拒绝、预置取消和 1s／100s 音频的相同最大样本帧大小。该帧边界证据不等于应用整体内存／p95 已达标，解码中的取消时序与更多格式边界仍待加强。

针对性测试：SubCueAudioQualityTests **5 passed，143ms**；存储指纹回滚槽 **3 passed，37ms**（存储字节 fixture，不代表 WAV）；TTS 质检继续批次及已有取消／切工程槽 **4 passed，1.155s**（明确 Mock）。截断／空 WAV 的 FFmpeg 诊断是故意的负向 fixture，不宣称没有底层日志。

最终命令：MSVC x64、临时 VSLANG=1033 下 `cmake --build out/build/windows-release-cuda` 与 `ctest --preset windows-release-cuda --output-on-failure`，**33/33 通过，73.81s**，含新增质检目标、既有媒体生命周期、新 CUDA 包和两启动测试。本增量第一轮 33/33、70.83s 在指纹扩展前通过，仅作中间证据，最终以上述 73.81s 为准。

包运行后实测 **5,099,696,744 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未添加运行依赖、新建 CPU 包或复制外部环境／权重；保留 dist，使用清理脚本预览后移除本次 package 暂存。

未完成：参考库的异步质检／人工审核、默认参考、批量整理／ASR／去重及 A/B 派生仍需接入。完整音频处理的全局并发 2 调度、预览缓存、裁切／停顿／增益、冻结编排导出、真实 Qwen／Flash 与费用、P8 性能和人工音质验收均仍未交付。提交失败可能留下未引用的完整资产，其正式恢复／清理入口待完成；未下载、安装、加载真实模型或调用 API，总体目标保持未完成。

### Voice Studio：2026-10-13 P3 有界音频调度与参考审核版本增量

- [x] ApplicationContext 增加最多 2 个并发的音频处理队列，与单线程存储队列独立；共用简单的受管理 Future 提交逻辑，不创建常驻模型或无所有者线程。TTS 结果质检与新参考检查接入该队列，后续导出／预览及其它音频入口仍需逐项接入。
- [x] 声音库先在存储队列冻结版本，再在音频队列完整检查；UI 仅接收小型指标与版本快照。读取／解码异常有明确错误；取消与销毁等待两阶段 Future，拒绝旧代次回调。
- [x] 新增参考审核弹窗，显示峰值／RMS、采样率和疑点，允许修正转写并确认疑点。保存生成新的 approved 版本，保留 sourceVersionId、指标／指纹和疑点接受记录，旧 pending 版本不覆盖。
- [x] 全局库创建审核版本与项目复制均核对审核指纹。生产本地生成只接受明确 approved 参考，旧无审核标记版本仍可读取，但需要重新检查／确认才能生成；不凭“文件存在”认定参考可用。
- [x] 测试覆盖 2 个音频任务运行时第 3 个排队、存储仍能独立完成、审核产生新版本／保留旧版，以及销毁时取消排队音频且没有迟到通知。审核弹窗实际渲染截图 `.test_tmp/voice-reference-review.png` 已查看。

针对性测试：`SubCueEditorIntegrationTests.exe audioProcessingIsBoundedAndDoesNotBlockStorage voiceReferenceReviewCreatesNewVersion voiceReviewDestructionCancelsQueuedAudio qmlSharedProjectWindow -o <临时日志>,txt`，**6 passed，3.615s**。样本是仓库测试音频，审核动作是测试模拟，不是人声克隆或人工音质验收。内置参考试听尚未接入，弹窗的确认路径不能替代真实试听证据。

最终重新构建所有目标并运行 `ctest --preset windows-release-cuda --output-on-failure`，**33/33 通过，72.92s**，含既有媒体生命周期、当前新 CUDA 包及两启动测试。包运行后实测 **5,099,733,608 bytes／4.749GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未创建 CPU 包、安装、下载或复制外部环境／模型；保留 dist，清理脚本先预览再移除本次 package 暂存。

未完成：内置参考试听／A-B、默认参考、批量同说话人导入／切分／中文转写／去重、派生处理、声音预设与完整绑定。旧手工导入的容器探测仍走存储队列，需随样本整理流程移到音频队列；全局处理并发目标不据此宣称所有旧入口已统一。文稿、编排、云端费用、试听导出、真实引擎与 P8 验收继续保留完整范围，P3 尚未整体完成；未调用模型或 API。

### Voice Studio：2026-10-13 P3 默认声音版本选择增量

- [x] 全局声音库 v2 增加 profile_defaults，保存明确选择的版本 UUID。v1 读取默认项为空，正常初始化按事务补新表，保留原档案、版本与参考资产，不迁移其它工程数据。
- [x] 默认项必须属于当前活动档案。本地参考需要已审核、非空转写与参考文件存在；云版本需要明确音色绑定。待审参考、跨角色版本及停用档案的修改被拒绝。
- [x] 声音库版本页提供设为默认和默认标记。更新仅影响全局默认选择，不改写不可变声音版本、不更换项目已有快照、不自动生成；停用／恢复保留默认选择。
- [x] 测试覆盖本地待审拒绝、已审核设置、跨角色拒绝、云默认、项目快照独立、停用保护及 v1 升级保留版本。界面测试实际选择云默认并显示标记，更新截图已查看。

针对性测试：默认及迁移两槽 **4 passed，122ms**（存储参考字节 fixture，不代表真实音频审核）；正常窗口槽 **3 passed，3.500s**。最终重新构建所有目标并运行 `ctest --preset windows-release-cuda --output-on-failure`，**33/33 通过，72.52s**，含既有媒体生命周期、新 CUDA 包及两启动测试。

包运行后实测 **5,099,742,312 bytes／4.750GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未创建 CPU 包、安装、下载或复制外部环境／模型；保留 dist，清理脚本先预览再移除本次 package 暂存。

未完成：默认项尚未接入未实现的文稿声音分配入口，不能据此宣称完整默认生成流程交付。参考内置试听／A-B、批量同说话人整理／ASR／去重、派生处理、预设／完整绑定，文稿与编排、云费用、试听导出、真实模型和 P8 验收仍需继续；总体目标保持完整且未完成。

### Voice Studio：2026-10-13 P3 参考试听准备／P5 播放末尾完整性增量

- [x] 调查确认可以复用 PlaybackEngine、IAudioDevice 与既有播放转交 API；参考试听不另引入 Qt Multimedia 或第二套播放依赖。
- [x] 修复正常 EOF 未向解码器提交刷新包的问题；每代只发送一次结束包，跳转重置，避免编码器尾帧遗失。
- [x] 音频解码结束时排出重采样延迟样本，更新时间坐标；正常结束后移除设备取样的 2ms 保留余量，使末尾样本可以全部消费。跳转代次检查防止旧结束状态覆盖新播放。
- [x] 增加 audioFinished 查询（解码已结束、帧队列和设备前缓冲均已排空），用于后续参考试听自动停止；不依靠估计墙钟时间裁掉最后几毫秒。
- [x] 新增有效 WAV 播放测试，验证 44.1kHz→设备采样率的完整帧数及 written／consumed 相等，关闭后不再报告音频结束状态。原有播放、跳转和硬件回退测试继续通过。

初次测试失败是测试循环反复调用 VirtualAudioDevice.resume、重置其计时器导致未消费音频；按真实预泵入转态只恢复一次后通过，未通过修改虚拟设备掩盖该测试错误。最终 SubCuePlaybackTests **17 passed，2.041s**。

最终重新构建所有目标并运行 `ctest --preset windows-release-cuda --output-on-failure`，**33/33 通过，74.80s**，包含当前播放测试、既有 50 次媒体生命周期、新 CUDA 包及两启动测试。包运行后 **5,099,742,312 bytes／4.750GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未创建 CPU 包、安装、下载或复制外部环境／模型；保留 dist，清理脚本先预览再移除本次 package 暂存。

未完成：本增量是参考试听所需的播放基础修复，声音库尚无试听按钮／播放所有者和工作区设备转交接入，不能勾选完整参考试听。下一步接入受管理的参考播放、关闭／切工程／销毁停止与转交测试，再推进批量整理和文稿。其它首版及后续范围保持原计划，未执行真实模型、云端或人工音质验收。

### Voice Studio：2026-10-13 P3 参考试听所有者与工作区转交增量

- [x] 新增 AudioPreviewController，复用 PlaybackEngine 与共享音频设备；后台在现有最多 2 个离线音频处理线程中检查资产／指纹，单独持有可取消 Future、播放引擎和 50ms 状态计时器。实时播放继续使用原有受管理解码线程与缓冲，不另建播放依赖。
- [x] WorkspaceRouter 接入试听转交：准备成功后暂停字幕／粗剪播放并转交设备，结束／停止归还当前工作区，原素材保持暂停，不自动恢复。切工作区、切工程与销毁停止试听并等待自身准备任务，拒绝迟到开始。
- [x] 审核弹窗提供试听参考／停止，关闭审核或声音库弹窗停止试听。播放状态更新不触碰审核转写草稿；正常音频结束由真实排空状态自动释放资源。
- [x] 指纹不匹配、初始化／设备不可用有可见错误。设备停止时退出试听；虚拟设备明确标记无可听输出，不能冒充真人试听。未增加 Qt Multimedia、模型、API 或其它运行依赖。
- [x] 测试覆盖素材暂停与设备转交、自动结束、切工作区／工程停止、错误指纹、设备停止、排队准备销毁后无迟到开始、实际按钮触发及草稿保留。

针对性测试初次 **5 passed，4.604s**；最终正常 Windows 平台复核同三槽 **5 passed，4.674s**。CTest 的离屏截图出现字体回退方框，未用作视觉通过证据；显式仅设置该测试命令的 QT_QPA_PLATFORM=windows 后重新渲染，中文试听控件、虚拟输出说明和草稿截图已查看。未修改系统／编辑器设置，未进行硬件听感验收。

最终重新构建所有目标并运行 `ctest --preset windows-release-cuda --output-on-failure`，**33/33 通过，76.11s**，包括当前设备边界、既有媒体生命周期、新 CUDA 包及两启动测试。此前 76.71s 是设备说明扩展前中间回归，最终以 76.11s 为准。

包运行后 **5,099,756,136 bytes／4.750GiB**；Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**（重叠）。未创建 CPU 包、安装、下载或复制外部环境／模型；保留 dist，清理脚本先预览再移除本次 package 暂存。

未完成：A/B 派生样本、批量同说话人整理／切分／ASR／去重、声音预设／完整绑定及文稿／Take／编排流程仍需继续。准备取消当前同步等待 Future、播放器打开仍沿用原同步入口，UI p95 和长参考极端耗时未实测；关闭多所有者须继续优化为先请求所有取消再等待，不能凭当前测试宣称所有性能约束满足。未加载真实 TTS 权重、调用 API 或进行人工音质验收，整体计划仍未完成。

## 历史新对话起始指令（当前范围以页首为准）

可在新对话中直接发送：

> 请阅读仓库根目录 `progress.md`，继续 SubCue 自动粗剪计划。先完成 Phase 8 控制器、真实结果模型、人工决定/撤销重做、源片段试听和 XML 导出连接。保留当前未提交修改，不要重构现有字幕界面；完成针对性测试后再进入 Phase 9。

### Voice Studio：2026-10-13 P3 样本整理与派生审核（功能实现补齐，真实录音验收待完成）

本次依据用户“继续完成 P3”推进声音库；未进入 P4 文稿／第三工作区，不缩减已确认后续范围，也不将自动化通过当作真实音质验收。

- [x] 复用已实现的声音 UUID、不可变版本、项目快照、分页／停用恢复、默认参考和参考试听。
- [x] 批量同说话人文件整理预览：用户明确选择本地文件；不识别或分离说话人。先完整流式质检，检测分块最多 12 秒，优先在连续 200ms 低电平区切分，持续讲话按长度切分并要求复核；源采样率、原始样本坐标和指纹保存到版本。
- [x] 原文件和派生文件分别保存。相同原件／相同 24kHz 派生内容去重；相同转写只提示，不删除不同表演。批次限 100 个文件、2000 段、512MiB 临时派生数据；不保留整篇 PCM。
- [x] 本地中文批量转写入口复用现有 Qwen／FunASR 和共享 GPU 门禁，整批持有 GPU。通常每个原件识别一次，时间范围跨切口或粗时间戳时对短样本单独识别，避免猜配整句。已有手工转写不覆盖，输入法提交后冻结批次，失败／取消保留已完成转写，可继续手工建档。
- [x] 样本 ASR 请求显式启用 offlineOnly：Worker 在导入模型前设置离线环境并拒绝网络连接，不复用云 ASR，不隐式下载依赖。缺模型有明确错误，未配置本地 Provider 保留手工路径。
- [x] 勾选样本保存前要求非空转写；保存后仍为 pending，必须逐个试听审核后才能设为默认。稳定 sampleId 和已提交版本去重保护重复保存及部分提交后重试；跨档案保存被拒绝。整理／复制／派生前后检测源指纹，防止保存变动原件。
- [x] 非破坏性裁切、20Hz 去直流和保守低电平软门（最大衰减 6dB），输出 24kHz 单声道派生 WAV。原样本／派生样本 A/B 试听；不承诺去除所有背景噪声。不合法裁切拒绝，不静默截断。裁切后的转写复核由 UI 和 C++ 同时要求明确确认，质量疑点另行确认；保存新版本并保留处理参数和来源。
- [x] 短于 3 秒／长于 30 秒的参考给出待审建议，不自动认定合格。手工单文件入口也移到有界音频队列做完整质检，存储队列仅负责复制和提交。
- [x] 中文声音预设名称与用户声明的素材使用范围保存为新版本；Flash 绑定增加业务空间和安全凭据引用，固定模型／北京地域／已有 Voice ID，未读取密钥或发起请求。全局变更不替换项目快照或默认版本。
- [x] 新任务沿用取消、等待、弱回调和代次保护；批量整理取消／销毁后无迟到通知，未引用临时文件随所有者释放。界面沿用现有主题，批量预览和逐样本审核分开。

代码范围：`src/core/tts/reference_samples.*`、`audio_quality.*`、`voice_library.cpp`、`src/app/voice_library_controller.*`、`VoiceLibraryDialog.qml`；ASR 增加请求级离线标志与 Worker 网络阻断；新增功能／所有者／离线测试并注册核心源码。既有未提交代码、文档和子模块修改保留。

验证（MSVC x64，VSLANG=1033；使用现有 windows-release-cuda）：

```text
cmake --build out/build/windows-release-cuda
ctest --preset windows-release-cuda --output-on-failure
SubCueEditorIntegrationTests.exe voiceBatchMissingAsrPreservesManualPathAndBindings voiceBatchDestructionCancelsAndRemovesTemporarySamples voiceBatchAndDerivedVersionsPreserveManualReview qmlSharedProjectWindow -o D:/DSH/SubCue/.test_tmp/p3-final-targeted.txt,txt
SubCueEditorIntegrationTests.exe qmlSharedProjectWindow -o D:/DSH/SubCue/.test_tmp/p3-final-windows-ui.txt,txt
E:/AIModels/TTS/.venv/Scripts/python.exe -B tests/python/test_asr_offline.py
```

最终完整构建成功，**33/33 CTest 通过，77.80s**，包含最新的后端裁切转写复核保护、批次取消／销毁、旧媒体生命周期、打包和两个启动测试。此前针对性测试 **6 passed／4.445s**；最后正常 Windows 界面复核 **3 passed／3.559s**，已查看批量预览和参考审核截图，中文、质量提示和按钮可读。离线 Python 测试 **1 passed／0.016s**，在真正连接前阻断 socket；没有模型推理。Qt 生成的 QML C++ 有 C4702 警告，构建成功，不宣称无警告。

CUDA 暂存包在验证后 **5,099,864,751 bytes／4.750GiB**，Torch **3.547GiB／74.69%**、cuBLAS **1.006GiB／21.18%**，统计重叠。无新增运行库，未生成 CPU 包，未复制独立 TTS 环境或模型。使用清理脚本先预览再移除 package 暂存，保留当前构建及现有 dist；dist 未作为本次新版本发布。

- [ ] 待用户提供有权使用的中文单人录音，验证真实本地转写、切分边界、去直流／软门对气口和尾音的影响，并进行真实可听设备 A/B 审核。本轮的音频 fixture 和虚拟设备测试不能替代这些验收，P3 尚不整阶段勾选。
- [ ] UI p95、超长原件耗时、真实 ASR 显存／速度及真实听感未测；现有同步关闭等待风险保留。软门是保守处理而非完整噪声分离器；持续讲话长度切分可能切到字词，需要人工复核。复制发布后未提交的完整资产仍由后续统一资产清理入口处理。

未下载安装环境、下载模型、加载真实 ASR／TTS 权重或调用云端 API。P4～P8 与 L1～L4／R 保持原计划，下一步在补真实 P3 验收的同时可继续独立的 P4 实施。

### Voice Studio：2026-10-13 P3 用户录音真实离线验收（人工听感仍待确认）

用户提供 `D:/DSH/SubCue/tests/测试音频1`。目录有男声、女声及女声日语素材；性别分组不等于同一说话人。本轮仅选择男声 `男声/中文音频1.mp4` 和女声 `女声/中文/电量不足.mp3` 两个中文源文件，每个文件独立建立临时验收档案，未将目录整体混入同一角色。日语保留到 L2，未自动识别说话人或情绪。

- [x] 使用现有本地 Qwen3-ASR 和 Forced Aligner、受管理 Worker／共享 GPU 门禁与 offlineOnly，实际加载已有权重进行转写、短片段重识别、待审样本保存和派生质检；无模型下载、环境安装或云端调用。
- [x] 男声原件实际解码时长约 359.915 秒、48kHz 双声道，整理得到 32 个有声样本，32 段均有转写。女声约 7.584 秒、48kHz 双声道，得到 2 段，均有转写；例如“感觉耗电耗得更快了”“是你看错了吧”。非空转写不等于人工准确率通过，专有名词和混合英文仍需复核。
- [x] 源文件前后 SHA-256 一致；分别导出首个待审样本及去直流／保守软门后的 A/B 文件。男声 A/B 约 5.284 秒，女声约 3.000 秒；派生为 24kHz 单声道，解码及有限值／削波等指标检查通过，无质量告警。
- [x] 保存数据仍为待审核，不调用人工批准接口；报告 `humanApproved=false`。测试声音库使用临时数据库，不更改用户全局档案、默认声音、工程或设置。
- [x] 推理完成后 Worker 退出；RTX 3060 Ti 8GB 一次运行中采样到整卡占用约 3763MiB，完成后约 1466～1522MiB。该值包含其它进程，不是模型峰值显存，也不据此承诺速度／显存指标。

新增 `voiceRealChineseSamplesWhenRequested` 显式验收测试；普通回归未设置环境变量时跳过，不会自动加载模型。每次创建独立输出目录，可重复运行而不覆盖上次音频。

```powershell
$env:SUBCUE_P3_REAL_SAMPLES='D:/DSH/SubCue/tests/测试音频1'
$env:QTEST_FUNCTION_TIMEOUT='1200000'
SubCueEditorIntegrationTests.exe voiceRealChineseSamplesWhenRequested -o D:/DSH/SubCue/.test_tmp/p3-real-retry.txt,txt
```

首次测试被 Qt Test 默认 300000ms 的函数级超时中断，**1 passed／1 failed，300.012s**，保留该失败记录；已确认推理进程退出。只在本次命令设置 20 分钟上限，未修改编辑器或项目设置。[Qt 官方超时说明](https://doc.qt.io/qt-6/qtest-overview.html#increasing-test-function-timeout)

随后真实验收 **3 passed／58.225s**（复用了首次已提交的识别缓存，并实际完成剩余识别）。输出隔离改动重新编译后，缓存复用重复验收 **3 passed／5.047s**，未重新推理，不将其当作冷启动吞吐量。普通 P3 回归 **5 passed／1 skipped，1.000s**，验证审核、销毁和缺模型手工路径。此前完整回归 33/33／77.80s 保留为上一轮记录，本轮没有重跑完整 CUDA 包／全量 CTest；仅修改测试代码，无新增运行库，包体沿用上一轮测量，不标为本轮重新打包。

待人工审核的音频和报告保存在 `out/validation/voice-p3-20261013/`：`speaker-1-A.wav`、`speaker-1-B.wav`（男声），`speaker-2-A.wav`、`speaker-2-B.wav`（女声），`audit.json` 包含原件指纹、分段范围、转写与指标。它们是本地验收资料，不发布或打包；移出临时探测目录以供用户试听，重复验收生成的副本按清理脚本预览后清理。

- [ ] 用户试听两组 A/B，复核转写、切口、气口和尾音，确认处理是否可接受。无告警不等于声音自然度／转写准确率通过，P3 人工验收仍未勾选。
- [ ] 全部目录／其它女性录音尚未逐个验收；日语属于后续 L2。真实长文 UI p95、冷启动完整耗时和峰值显存仍需专门测量。P4～P8、L1～L4／R 原范围不变。

### Voice Studio：P3 用户确认代表性 A/B 处理效果

用户针对两组男／女声 A/B 试听反馈回复“可以”，记录为代表性样本处理效果接受。

- [x] 两组代表性 A/B 处理效果获用户确认，P3 功能实现与本项人工验收完成。
- [x] 验收报告增加 `audioProcessingAccepted=true`、反馈原文与明确范围；保留 `humanApproved=false`，它表示完整参考转写尚未审核，不将本次反馈扩大为全部录音／所有台词准确率通过。
- [ ] 34 段转写的专有名词、混合英文及全部目录仍需按样本审核。这属于实际建档的内容审核，未批量批准声音版本或修改用户全局库、默认声音及项目。

本次仅更新进度和本地验收报告，未修改业务代码或运行新测试。下一实施阶段为 P4 文稿、JSON 与配音工作区；其余性能／跨语言／真实 TTS／云端验收保留原阶段要求。

### Voice Studio：2026-10-13 P4 文稿、JSON 与配音工作区（阶段功能及自动化验收完成）

依据用户“继续 P4 阶段／继续执行”，在既有 P1～P3 基础上实现本阶段。保留所有既有未提交修改、历史文档和子模块变更；未重置工作区、修改编辑器设置、安装环境、下载模型或调用云端 API。

- [x] 接入第三个“AI 配音”工作区与共享工程生命周期。左侧声音、中央正文与段落、右侧朗读／参数／候选、底部独立配音序列；分栏可调整、折叠，沿用现有主题和控件。声音可分配到当前句、本段或全部未绑定句子，引用项目不可变声音版本。
- [x] 正文、手工朗读和表演备注分开保存；未手工修正的朗读跟随正文，单句修正优先于项目替换表。首次按标点建议分段，确认后使用稳定 UUID；拆分、合并、删除和本段重新分段有明确预览，旧句、裁切与 Take 归档，其它段落不变。支持句子／段落顺序调整与项目撤销重做。
- [x] 输入法未确认时不采集半句；检查区草稿的正文、朗读、seed 与编排参数一次提交，任何字段无效均不部分提交。编辑框自身处理未提交草稿撤销，提交后进入项目命令。保存、切换、生成等关键操作先提交草稿；后台候选通知不重置正文草稿。保存期间继续编辑保留 dirty，保存只标记被冻结的检查点。
- [x] 中文数字、日期、百分比和常见单位只提供朗读建议预览，确认后成为单句修正；项目替换表逐条预览，非法规则报错，不静默忽略。当前有效参数只暴露 seed，其它表演控制作为备注，不假称 Mock／Base 已执行情绪指令。
- [x] TXT 与版本化 JSON 新增导入创建新 ID；本项目来源标识加稳定 ID 支持定向更新。导入展示正文、参数、型号、语言、声音映射及手工保护；陌生／重复／已删除 ID 整体拒绝，不按行号或相似文字覆盖。已有人工朗读、选中 Take、裁切、增益和停顿保留。定向更新引用不存在的声音版本时明确要求先绑定或修改映射，不留下非法外键。
- [x] 导入内容按纯文本显示，不执行代码、不读取内容中的 URL／路径。JSON 导出为冻结的文稿快照，保留来源标识但不保存 Take 的绝对音频路径；原子写出并保护当前工程、素材、脚本、项目资产、设置及全局声音数据库。
- [x] 字幕引用保存项目、字幕、轨道、文本、微秒时间和来源指纹；文本／时间分别预览同步，手工朗读不覆盖，不反写字幕。仅同步时间不使音频失效；来源删除保留独立内容及历史，并支持用户明确选择重新关联，禁止猜配。
- [x] 统一 SQLite 升级为 v5，增加文稿元数据、段落和片段关系表，声音／选中 Take 具有外键。正式模块键为 `voiceStudio`，不覆盖旧的未知 `voice` 模块。文稿实体不重复写入工程 modules JSON，读接口组合成快照；检查点 revision 条件提交、回滚和现有任务／生成历史实时提交继续复用。读取检测损坏实体及不一致关系，v4 迁移保留旧模块和任务。
- [x] Mock 支持当前句、本段、全部和仅失效句生成；不可变输入包含实际朗读、声音版本、有效参数及输入版本。候选分页、当前／过期标记、归档历史查询与原音试听；应用检查输入版本和哈希，过期只保留历史。安全批量应用跳过人工裁切并在提交时复查最新编辑；人工裁切必须明确保留时间或重置，范围不合法拒绝，增益、停顿和顺序保留。比较旧 Take／新候选复用 P3 试听所有者，取消与切工程拒绝迟到播放。
- [x] 编辑正式模型与撤销由 C++ 拥有，QML 仅持有草稿和交互状态；磁盘处理复用串行存储队列、受管理 Future、取消和等待。新增运行脚本 `inference/tts_python_worker.py` 是实际 Mock／后续本地 Provider 使用的桥接文件，已列入包清单，不复制外部 Python 环境、模型或源仓库。

代码范围：`src/core/tts/voice_document.*`、`src/core/project/project_store.*`、`src/app/voice_studio_controller.*`、`project_session.*`、`workspace_router.cpp`、`main.cpp`、`VoiceWorkspace.qml`、`AppRouter.qml`、CMake／打包清单及文稿／应用集成测试。

验证命令（MSVC x64，VSLANG=1033，现有 CUDA 构建；Qt Test 日志使用 `-o 路径,txt`）：

```text
cmake --build out/build/windows-release-cuda
SubCueVoiceDocumentTests.exe -o D:/DSH/SubCue/.test_tmp/p4-core-final.txt,txt
SubCueProjectStoreTests.exe -o D:/DSH/SubCue/.test_tmp/p4-store.txt,txt
SubCueEditorIntegrationTests.exe voiceDocumentSharesLifecycleAndPreservesEditsDuringSave voiceDocumentMockCandidatesProtectManualCropAndVersions voiceDocumentImportDestructionHasNoLateCallback voiceDocumentSubtitleSyncAndJsonProtectIndependentEdits qmlSharedProjectWindow -o D:/DSH/SubCue/.test_tmp/p4-windows-final.txt,txt
SubCueSourceHygieneTests.exe -o D:/DSH/SubCue/.test_tmp/p4-hygiene-fixed.txt,txt
ctest --preset windows-release-cuda --output-on-failure
```

最终完整构建成功，**34/34 CTest 通过，79.50s**，包含打包、两个启动测试和既有 50 次媒体生命周期回归。文稿核心 **8 passed／70ms**，旧存储 **33 passed／1.246s**；正常 Windows 平台专项 **7 passed／5.606s**，验证保存期间编辑、Mock 候选与裁切／版本保护、导入销毁、字幕差异与 JSON 拒绝非法声音、输入法合成中不提交和候选到达保留草稿。已查看正常 Windows 截图，中文、段落组织及分栏可读；截图属于界面验证，未作真实音质验收。

首次完整回归 **33/34 通过，78.90s**；源码扫描把 QML 占位提示里的 `python.exe` 识别为禁止模式。已改为等义的“Python 可执行文件完整路径”，没有放宽检查规则；单独源码检查 **9 passed／35ms**，随后重跑完整回归得到上述最终结果。Qt 生成的 QML C++ 仍有 C4702 警告，不能宣称无编译警告。

最终 CUDA 包运行后 **5,100,299,137 bytes／4.750GiB**；Torch **3.547GiB／74.68%**、cuBLAS **1.006GiB／21.18%**（统计重叠），低于 4.8GiB 目标。未创建 CPU 包，不能据此宣称其包体通过。日志、包体统计和正常 Windows 截图保存在 `out/validation/voice-p4-20261013/`。验证后的 package 暂存按清理脚本先预览再移除；保留当前构建和既有 dist，dist 不作为本次新版本发布。

后续边界与已知风险：

- [ ] P5：真实波形、句子／波形双向定位、按裁切编排播放、连续试听磁盘缓存、冻结编排 WAV 导出及响度处理。当前底部序列可定位／编排、比较的是原生候选音频，不冒充这些尚未实现的音频功能。
- [ ] P6／P7：真实本地 Qwen 与 Flash、人民币预测／软预算验收。当前工作区生成明确为 Mock 测试音，未加载 TTS 权重或发起付费请求。
- [ ] P8：万字／约 500 句压力、UI p95、文稿撤销快照内存及历史查询性能仍需测量。手动保存目前在单一事务中重写文稿实体，未重建任务／资产数据库；批量候选查询采用分页但可能多次加载工程，应按测量结果优化。未将已有单项回归等同于全部性能目标达标。
- [ ] 粗剪范围／顺序显式导入和带波形的时间轴复用仍需在音频编排阶段衔接；不改写原粗剪人工决定。跨语言、情绪识别、云端注册及其它后续／预留范围继续保留。

P4 的功能任务与自动化验收完成，下一阶段为 P5；整体 Voice Studio 首版尚未完成。

### Voice Studio：2026-10-13 P5 音频编排、波形、试听缓存与导出（实现已接入，最终验收未完成）

- [x] 新增 VoiceAudio／VoiceAudioController，冻结选中 Take、裁切、增益及停顿；复用 FFmpeg、粗剪流式解码和 P3 试听所有者，不加载模型、不调用云端 API、不新增音频运行库。
- [x] 校验原 Take 的指纹、原生采样率和样本数，处理后再次检查源指纹；缺句／损坏／变动资产报错定位，不静默漏句。流式转换为 48kHz、24-bit PCM、单声道 WAV，保留自然停顿及原 Take；RIFF 奇数字节填充与大小限制已处理。
- [x] 单句／连续编排试听、内容键磁盘缓存复用、取消与进度；缓存上限 1GiB，回收仅限缓存目录内的内容键文件，保护活动资源，拒绝链接目录／文件。增益、裁切、停顿、应用 Take 和文稿顺序变化使对应试听失效；未应用候选不改变缓存。
- [x] 每句最多 256 对真实峰值，复用 WaveformPeak 存储原生有界波形，暴露 waveformBytes，切工程／失效释放。一次波形准备最多 1000 句，整轨导出不受此波形数量限制；不在内存保存整篇 PCM，处理使用临时磁盘流及固定转换缓冲。
- [x] 底部真实波形、播放位置指示、句子／波形定位和 Shift 拖动裁切。播放跟随避开焦点、输入法草稿和弹窗；其它原音试听不会冒充编排播放。编排试听复用音频设备转交，退出工作区不迟到自动播放。
- [x] 冻结编排整篇 WAV、可选句级新目录导出、过期 Take 明确确认、取消／失败保持原输出；句级文件使用临时目录整体发布，拒绝覆盖现有目录。提供默认关闭的整轨 RMS 平均电平处理：目标 −20dBFS、样本峰值上限 −1dBFS，明确不等于 LUFS／真峰值；统一增益只改变导出派生文件，不逐句拉齐。编排增益导致削波给出提示。
- [x] 粗剪顺序／原始范围显式预览后追加为独立文稿，保留项目／媒体资源／分析版本／源样本坐标及采样率；来源映射使用 UUID，不永久引用行号。原人工决定、已有文稿及 Take 不变，不强制拉伸新音频；重叠或过长间隔拒绝，不静默截断。

代码范围：`src/core/tts/voice_audio.*`、`src/app/voice_audio_controller.*`、`audio_preview_controller.*`、`voice_studio_controller.*`、`project_session.*`、`VoiceWorkspace.qml`、`main.cpp`、粗剪 WAV 流式接口、CMake 注册及音频／应用集成测试。现有改动和子模块保留。

已运行验证：

```text
cmake --build out/build/windows-release-cuda --target SubCueAudioQualityTests SubCueEditorIntegrationTests
SubCueAudioQualityTests.exe -o D:/DSH/SubCue/.test_tmp/p5-audio-all.txt,txt
SubCueEditorIntegrationTests.exe voiceAudioCacheExportAndCancellationProtectFrozenArrangement voiceAudioDestructionHasNoLatePublication voiceRoughCutImportPreservesSourceDecisionsAndExistingEdits qmlSharedProjectWindow -o D:/DSH/SubCue/.test_tmp/p5-final-targeted.txt,txt
SubCueSourceHygieneTests.exe -o D:/DSH/SubCue/.test_tmp/p5-hygiene.txt,txt
cmake --build out/build/windows-release-cuda
```

音频测试 **8 passed／473ms**；正常 Windows 平台专项 **6 passed／4.744s**，覆盖缓存复用／超过容量后的回收、冻结导出期间编辑、过期确认、句级发布、取消／销毁无迟到回调、粗剪来源与人工决定保护、波形点击和 Shift 裁切。源码检查 **9 passed／35ms**。已查看 `.test_tmp/voice-waveform.png` 的中文真实波形界面。

中间验证曾发现并修复 Qt 峰值对列表被展开、导出弹窗布局循环，以及测试 fixture 缺少粗剪引用／使用错误工程扩展名和动态委托查找失败；保留失败日志，不将中间失败写为通过。最后又补了音频取消向 FFmpeg 传播及新处理／试听所有者等待前的取消，**最终完整 CUDA 构建成功**，打包报告 **5,099,039,400 bytes**（尚未运行后的最终统计）。Qt 生成代码有 C4702 警告。

执行在最终验收前被中断，P5 **不勾选整阶段完成**。尚需：

- [ ] 对最终构建运行完整 `ctest --preset windows-release-cuda --output-on-failure`，包含打包、两个启动测试及 50 次媒体生命周期回归；针对最终取消改动复核上述专项。
- [ ] 测量最终包体及 Torch／cuBLAS 占比，保存准确验收日志和正常 Windows 截图到 `out/validation/voice-p5-20261013/`，再按清理脚本预览后清理 package 暂存和本轮临时日志。当前 package 暂存仍保留，未复制到 dist。
- [ ] P8 仍需长文／UI p95／内存与磁盘压力测量；临时浮点磁盘流会占用额外空间，文件权限／磁盘不足报错但未注入所有故障。闭环音频 fixture 与虚拟设备测试不等于真实 TTS 音质／硬件听感验收；P6、P7 与后续功能范围保留。

### Voice Studio：2026-10-13 P5 最终验收完成

用户要求继续执行后，复查最终构建及取消传播改动，完成上节中断时未完成的验收事项。上一节保留为执行过程记录，本节为 P5 当前状态。

- [x] 最终构建的正常 Windows 专项 **6 passed／4.741s**：缓存超限回收、冻结导出期间编辑、取消和销毁保护、过期 Take 确认、句级导出、粗剪人工决定保护、波形点击定位及 Shift 裁切全部通过。已重新查看正常 Windows 波形截图，虚拟试听明确显示“不产生可听输出”。
- [x] 运行 `ctest --preset windows-release-cuda --output-on-failure`，**34/34 通过，82.58s**，包含音频边界、P4 兼容、源码约束、打包、两个启动测试及既有 50 次媒体生命周期回归。没有将已有通过日志代替本轮验证。
- [x] 最终 CUDA 包运行后 **5,100,406,657 bytes／4.750GiB**；Torch **3,808,916,676 bytes／3.547GiB／74.68%**，cuBLAS **1,080,248,544 bytes／1.006GiB／21.18%**（统计重叠）。低于 4.8GiB 目标；无新增运行库、模型或外部环境。未创建 CPU 包，不宣称其包体通过。
- [x] 本轮测试日志、历史失败日志、正常 Windows 截图、最终 CTest 日志和包体统计已保存到 `out/validation/voice-p5-20261013/`。通过 `tools/clean-workspace.ps1 -Only ...` 先预览，再 `-Apply` 清理 package 暂存和已归档的 P5 临时日志／截图；保留当前 CUDA 构建、既有 dist 及模型目录。

本轮没有继续修改业务代码、加载模型、安装环境或调用云端 API。P5 的功能与自动化验收完成；整轨处理明确为可选 RMS，不冒充 LUFS／真峰值。真实 TTS 音质和硬件听感进入 P6；云端、人民币预测及软预算进入 P7；长文压力、UI p95 与整体内存指标保留 P8，未宣称这些尚未测量的指标通过。下一实施阶段为 P6 本地 Qwen 真实接入与验收。

### Voice Studio：2026-10-13 P6 本地 Qwen 完成（音质暂通过）

本阶段按已确认计划接入真实本地 Qwen Base，复用现有 ProjectStore、TtsTaskController、受管理 Worker 与 GPU 批次门禁。保留此前代码、文档和其它未提交改动；未安装依赖、下载模型、修改 ASR 环境或调用云端 API。

- [x] 配音工作区增加 Qwen／Mock 明确选择、独立 Python 与模型目录输入、离线预检、范围生成和任务状态。Base 不执行自由表演指令；未预检、缺少已审核参考或路径变化时不派发，不自动回退到 Mock。
- [x] 预检十个必需配置／权重文件的存在、大小和 SHA-256，核对 Base／0.6B／12Hz、运行包及 CUDA。模型版本包含文件、包版本和实际推理策略。预检不加载权重；SDK／模型实际加载在用户选择的生成批次中执行。
- [x] Worker 使用严格离线环境和网络连接阻断；临时编译／HF／CUDA 缓存由所属操作管理。参考使用本地已审核转写与文件指纹，最多缓存四组克隆条件；批次结束／取消／关闭释放整个进程树，不后台常驻。
- [x] 显式生成 Qwen 条件形成编辑命令，旧 Take 和人工编排保留。结果只进入候选；存储记录原生规格、真实质检、耗时、运行版本及显存。单句重新生成不改变其它句子；人工裁切必须明确保留或重置。候选的质检疑点展示为试听提示。
- [x] 修复 Windows 虚拟环境启动器派生 Python 的生命周期风险：使用创建时加入的 Job Object 管理进程树，取消／超时结束子进程并等待后才释放 GPU 门禁。真实运行开始后取消以及嵌套子进程取消／超时测试均通过。

实际环境：`E:/AIModels/TTS/.venv/Scripts/python.exe`，Python 3.12.14、qwen-tts 0.1.1、Torch／Torchaudio 2.10.0+cu126、Transformers 4.57.3、SoundFile 0.14.0、Librosa 1.0.0；NVIDIA GeForce RTX 3060 Ti 8GB。模型为 `E:/AIModels/TTS/models/Qwen3-TTS/Qwen3-TTS-12Hz-0.6B-Base`。SoX 和可选 flash-attn 缺失提示保留；当前本地 WAV 数组参考／SDPA 路径实测可运行，未因此自动安装。

参考为用户确认本轮可用的 `out/validation/voice-p3-20261013/speaker-2-B.wav`，转写“感觉耗电耗得更快了”，SHA-256 `6cb7bae01e6dd6da5cbd25094ba1a5cea785fa6d7d4df631798ea29e3615e9ea`。只在隔离验收工程中标记该参考通过，不更改全局声音库或其它样本审核结果。

**音质调查与确认**

用户初听反馈基本可用，但不够清晰、有杂音和类似削波感，并允许暂时通过。实际文件质检未发现满幅削波；这不能排除主观失真或模型伪影。以同一参考、同一 seed=12345 对比 BF16 声学解码、FP32 声学解码和 FP32 整句输入，用户明确选择“FP32 解码版本更好，保留当前节奏”。最终固定声学编码／解码 FP32，主合成模型保持 BF16（设备不支持时 FP16）、SDPA，保留当前模拟文本流节奏（不宣称真实流式输出）。推理策略纳入模型版本，旧结果不会被覆盖。没有添加强制降噪或响度处理掩盖音质问题。

**本轮准确验证**

- 完整 CUDA 构建：`cmake --build out/build/windows-release-cuda` 成功。随后对新增测试目标重新编译成功；Qt 生成代码 C4702 警告保留。
- `E:/AIModels/TTS/.venv/Scripts/python.exe -B tests/python/test_tts_worker.py`，使用项目临时目录：**8/8，1.533s**；涵盖协议、原子发布、不覆盖、缺文件、模型身份变化、离线阻断、崩溃及取消。Mock 不作为真实配音验收。
- `SubCueAsrTests.exe inferenceCancellationAndTimeoutStopChildProcessTree localTtsProviderRunsAndContainsMockFailures`：**4 passed，3.331s**；含初始化／清理槽。
- 显式设置 `SUBCUE_P6_REAL_QWEN=D:/DSH/SubCue/out/validation/voice-p6-20261013/run-5`、`QTEST_FUNCTION_TIMEOUT=1800000` 后运行 `SubCueEditorIntegrationTests.exe voiceRealQwenWhenRequested voiceQwenMissingEnvironmentPreservesEditing`：**4 passed，69.670s**。真实生成两句、局部重新生成一条，三条有效非 Mock Take；模型开始运行后取消第四条，不增加候选。48kHz／24-bit／单声道整篇导出、候选裁切保护、其它句子不变、连续保存、模型十项指纹及参考哈希前后相同通过。普通 CTest 默认跳过真实模型专项，不在日常测试中自动加载权重。
- 最终三条原生 24kHz Take 的生成耗时（含该句克隆条件处理，不含批次预检／模型加载）为 **10.139s、5.921s、9.889s**；最大 Torch allocated **2,998,733,824 bytes／约 2.79GiB**，reserved **3,347,054,592 bytes／约 3.12GiB**。这些不是整机总显存，也不代表所有 8GB 平台或长文速度。取消／完成后确认没有 TTS Worker 残留。
- 首轮完整回归因源码检查把 Qt `CreateProcessArguments` 类型名误报为系统 API 失败；已保留失败日志并改成识别直接 API 调用，不增加整个文件豁免。修正后 `SubCueSourceHygieneTests` **9 passed，50ms**。
- 最终 `ctest --preset windows-release-cuda --output-on-failure`：**34/34，82.74s**，包含既有 50 次媒体生命周期、源码约束、打包和两个启动测试。
- 正常 Windows `SubCueEditorIntegrationTests.exe qmlSharedProjectWindow`：**3 passed，4.783s**；确认中文显示、草稿／输入法、波形及生成区实际滚动布局。完整回归的 offscreen 截图不作为正常字体显示证据；最终正常截图单独保存。
- 最终 CUDA 包 **5,100,445,021 bytes／4.750GiB**；Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（与 Torch 统计重叠）。低于 4.8GiB，未复制外部 TTS 环境或模型；未生成 CPU 包，不宣称 CPU 包体通过。

证据位于 `out/validation/voice-p6-20261013/`：`run-5/audit.json`、隔离 SQLite 工程与资产、整篇及局部重生成 WAV、`quality-comparison/`、`manual-review.json`、准确测试日志、正常 Windows 截图及包体 JSON。前两次验收夹具错误（缺质检指纹、误用绑定范围）与中间源码检查失败不写成通过。

**当前边界与后续**：P6 功能及真实 8GB 平台闭环完成，人工音质按本轮短中文／女声对照暂通过；不宣称高音质、所有角色或长文语料通过。清晰度、杂音、参考长度与代表性语料仍需后续持续优化。P7 的 Flash／人民币预测／软预算、P8 的长文／UI p95／内存与整体发布，以及 L1～L4／R 的已确认范围均保留。

- [x] 验收日志、试听对照脚本／日志和正常截图已归档；通过 `tools/clean-workspace.ps1 -Only ...` 预览后 `-Apply` 清理 package 暂存与本轮已归档临时文件。保留当前 CUDA 构建、既有 dist、验收工程／音频及全部外部模型和环境。

### Voice Studio：2026-10-14 P7 开发与 Mock 验收完成，真实校准待用户确认

本阶段复用现有任务控制器、项目存储队列、最多两个任务的音频处理队列及系统安全凭据存储；没有调用真实云端合成、读取 ASR 凭据、安装环境或下载模型。用户已表示具备北京 Workspace ID 和 Flash Voice ID，将稍后在应用填写并手动确认校准。此答复不代表已授权任何具体付费批次，P7 整阶段仍未完成真实验收。

- [x] 增加固定 Flash HTTPS Provider：北京业务空间、已有 Voice ID、中文、24kHz WAV、可选 seed；校验实际能力，不上传参考或注册声音，不自动回退。表演备注仅保存，不发送到当前引擎。
- [x] 配音工作区增加云端选择、专用凭据安全保存、人民币软预算、发送文本／音色／预测预览及显式校准确认。密钥只走 `SubCue/VoiceStudio/` 专用引用，不进入项目、日志、备份或请求诊断；保存后清空输入框。
- [x] 首次无匹配历史时校准 1～3 句（默认 1 句），结果成为正常候选；再次预览复用已完成候选，不重复收费。用户显式勾选重新付费后才允许重复生成。结果不自动应用，不覆盖裁切或编排；预览后文稿发生变化则拒绝派发。
- [x] 保存不可变价格快照：Flash 北京输入 ¥1.5／百万 Token、输出 ¥12／百万 Token，核验日期 2026-10-14。实际 Token 按当时原价折算，未来预测按当前价格；不假定优惠、免费额度或未知费用为零。以相同实际条件的真实用量、字数和音频时长预测，显示样本数与可信程度。
- [x] 下一句派发前比较已知金额与预测，超预算或无法判断时暂停；校准金额计入批次预算，更新已知费用及剩余预测。软预算不能保证在途请求不超额，不冒充账单实付金额。
- [x] 请求 ID、用量及价格先事务保存，再下载与质检；下载／取消／损坏音频失败仍保留已知成本。超时、TLS、取消中的结果不确定不自动重发；存储层阻止匹配输入的未知或已计费失败请求自动重复，显式新尝试保留原输入及 retryOf 关联。
- [x] 音频下载不携带合成密钥，限制北京 OSS HTTPS、重定向和响应／下载大小。仅重试同一下载 URL 两次，不重新合成；有效期、HTTP 错误、未知用量、音频损坏、磁盘写入及取消明确失败。云端解码／校验走已有有界音频处理队列，关闭等待与项目代次保护继续适用。

代码范围：`src/core/tts/cloud_tts_provider.*`、共享 `http_client.*`、`project_store.cpp`、`tts_task_controller.*`、`voice_studio_controller.*`、三个配音 QML 视图、Core CMake 注册以及 ASR／编辑器集成测试。保留其它已有代码与子模块改动，无新增运行库。

官方核验依据：[HTTP 接口](https://help.aliyun.com/zh/model-studio/qwen-audio-tts-http-api)、[Flash 模型与原价](https://help.aliyun.com/zh/model-studio/qwen-audio-3-1-tts-flash)、[OSS HTTPS](https://help.aliyun.com/zh/oss/user-guide/access-oss-by-https-protocol)。

**本轮最终验证与证据**

```text
cmake --build out/build/windows-release-cuda
SubCueEditorIntegrationTests.exe voiceFlashCalibrationAndUnknownResultsAreProtected
SubCueAsrTests.exe cloudTtsPricingAndPaidRequestProtection qtNetworkClientDownloadsAndPosts
ctest --preset windows-release-cuda --output-on-failure
SubCueEditorIntegrationTests.exe qmlSharedProjectWindow
```

- [x] 最终完整 CUDA 构建成功；最终集成专项 **3 passed／972ms**。覆盖未确认不上传、校准候选复用、真实有效 WAV 夹具、改稿保护、已知费用保存、结果不确定及显式重试关联；使用 Mock HTTP，不是云端音质验收。
- [x] HTTP／计价专项最终通过（含初始化／清理 **4 passed**）；覆盖真实本地回环 Qt Network 响应／下载边界及 Mock 预算、未知费用、过期 URL、限流、损坏响应、下载重试不重复合成和参数校验。准确逐次耗时见归档日志，不以中间测试代替最终回归。
- [x] 最终完整 CTest **34/34，81.62s**，包含既有 50 次媒体生命周期回归、源码约束、打包和两个启动测试。正常 Windows QML 检查 **3 passed／4.787s**，正常中文云端生成界面截图单独归档。Qt 生成代码 C4702 警告保留；没有据此修改无关业务。
- [x] 最终 CUDA 包 **5,100,557,149 bytes／4.750GiB**；Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（统计重叠）。低于 4.8GiB；外部 TTS 环境和模型未入包。未构建 CPU 包，不宣称 CPU 包体通过。
- [x] 日志、正常 Windows 截图和最终包体 JSON 归档到 `out/validation/voice-p7-20261014/`，包含中间验证记录；临时日志与 package 暂存按清理脚本先预览后清理。

**未完成与已知边界**

- [ ] 用户在应用配置北京业务空间、已有 Flash Voice ID 及专用安全凭据，并手动选择校准批次；再验收真实返回字段、音色、有效音频、Token／费用与下载过程。无 Key／网络仍可编辑和保存。
- [ ] P7 真实小批次验收通过后才勾选阶段完成；P8 长文压力、UI p95、整体内存、发布验收以及 L1～L4／R 保持原范围，未宣称通过。
- 预测只查询最近最多 500 条历史任务、按实际条件匹配；无足够匹配样本转入校准，低样本不提供精确保证。`http-wav24k-20261014` 是本应用 API 配置版本，不是服务器模型权重版本。
- 下载失败有同 URL 重试，但尚无跨重启独立的签名 URL 下载恢复入口；最终失败保留费用与请求 ID，显式重新合成可能再次收费。旧声音版本若使用旧凭据引用，应建立新绑定版本，不静默修改不可变历史。
- Mock HTTP、有效 WAV 夹具及本轮普通回归均不能证明真实 Flash 音质、官方账单、长文性能或所有网络故障。不存在自动重试付费合成或未授权上传。

### Voice Studio：2026-10-14 P7 测试连接与凭据状态补充

- [x] 云端生成区增加“测试连接（不合成）”、北京 Workspace ID 输入和“API Key 已保存（不回显）／未保存或不可读取”状态。重新打开时按当前专用引用检查安全凭据；Key 不回填、不显示尾号，不进入诊断或工程。保存状态不代表鉴权成功。
- [x] 按[官方模型列表接口](https://help.aliyun.com/en/model-studio/list-models)执行显式 GET `/api/v1/models`，不发送文稿、不调用合成。只接受固定北京域名、禁用重定向，15 秒传输超时、1MiB 响应上限；验证官方 success／output.models 结构。401／403 提示鉴权与权限，其它失败只显示安全摘要，不回显服务端正文或错误对象。成功明确提示 Voice ID／合成仍需校准。
- [x] 复用受管理音频任务队列及现有取消／等待／代次保护，销毁或切工程不留下迟到回调；新增输入框纳入编辑焦点保护。测试凭据仅返回布尔状态，不向 QML 暴露已保存密钥。
- [x] 最终构建成功；正常 Windows 专项 `SubCueEditorIntegrationTests.exe voiceFlashConnectionDoesNotSynthesizeOrRevealCredentials voiceFlashCalibrationAndUnknownResultsAreProtected qmlSharedProjectWindow`：**5 passed，6.095s**。覆盖未保存不联网、保存状态、GET 无正文、成功／401、拒绝任意地址、取消与原有校准保护；已查看正常中文截图。
- [x] 最终 `ctest --preset windows-release-cuda --output-on-failure`：**34/34，80.15s**。中间打包目录变化导致打包测试失败，随后稳定目录回归中凭据保存失败；直接探针确认 Windows CredWrite 错误码 8。隔离区累计 714 条 `SubCue/Test/` 自动化假凭据，限定该前缀清理后恢复通过，未读取密钥内容或修改正式凭据。新连接测试结束时删除自身条目；其它既有凭据测试的长期清理仍需改善，未以改生产凭据逻辑规避问题。
- [x] 构建／专项／完整回归／中间失败日志、正常 Windows 截图和包体统计归档于 `out/validation/voice-p7-20261014/connection-*`。CUDA 包仍约 **4.750GiB**，Torch **74.68%**、cuBLAS **21.18%**（重叠）；准确 bytes 见 `connection-package-size.json`。无新增依赖、CPU 包、模型或运行环境。打包暂存按清理脚本预览后移除。

本轮真实连接与付费合成均未执行。用户在应用安全保存 Key、输入北京 Workspace ID 并点击测试；连接通过后手动校准 1 句，仍是 P7 真实验收门槛。P8 及后续规划保持，未因本次按钮开发宣称整阶段或完整计划完成。

### Voice Studio：2026-10-14 连接结果就地显示

- [x] 测试按钮右侧增加独立结果：未测试／测试中／连接成功／连接失败，失败使用现有危险色；悬停显示安全摘要。文稿编辑、试听及其它底部消息不覆盖结果；变更业务空间或凭据引用不展示旧条件的成功，重新保存 Key 后清空旧结果。取消等待将进行中结果结束为取消提示。
- [x] 最终 `cmake --build out/build/windows-release-cuda --target SubCue SubCueEditorIntegrationTests` 成功；正常 Windows `SubCueEditorIntegrationTests.exe voiceFlashConnectionDoesNotSynthesizeOrRevealCredentials qmlSharedProjectWindow` **4 passed，5.031s**，覆盖结果独立于其它编辑状态及成功／失败／取消；已查看截图。中间 Label 悬停属性错误已修正为 HoverHandler，失败日志保留。
- [x] 验证日志与正常截图位于 `out/validation/voice-p7-20261014/connection-inline-*`。本轮仅局部界面／状态调整，没有运行全量 CTest、重新打包、真实 API 或模型；上一节完整回归不作为本次全量验证。

### Voice Studio：2026-10-14 P8 自动化压力与发布基线（真实云端门槛待完成）

P7 的真实 Flash 校准未收到验收结果，按依赖关系不勾选 P8 整体发布完成。先完成不依赖付费调用的自动化部分，未改写业务代码或缩减原计划。

- [x] 新增显式压力专项 `voiceLongNarrationStressWhenRequested`，仅指定 `SUBCUE_P8_REPORT` 时运行；普通 CTest 跳过，不自动推理。有效 WAV 夹具构建 **11,000 字／500 句／每句 3 个 Take（共 1500）**，验证历史分页、独立选中结果、保存、500 句试听缓存、波形上限及整篇 48kHz 单声道导出。
- [x] 长文专项 **3 passed／65.116s**：导入 26ms，初次保存 237ms，夹具历史创建 45.062s，带历史编辑保存 185ms，连续试听准备 9.707s，导出 8.696s。导出 31,200,000 样本；波形 **1,024,000 bytes**，磁盘缓存 **99,231,019 bytes**，分别低于 64MiB／1GiB 上限。控制器选择 p95 0.1671ms，不作为完整 UI 响应指标。
- [x] 正常 Windows 窗口显式 `SUBCUE_P8_UI_REPORT` 测量 500 句、50 次选择＋事件处理＋窗口帧回读，最终 **p95 33.1832ms**。已查看正常中文截图；该指标是当前机器、该操作和该文稿负载的基线，不代表所有操作、硬件或带 1500 个音频候选的同时渲染。
- [x] 既有媒体生命周期专项完成 **53 次**：热身后最终十次前／后私有内存 **159,100,928 → 173,248,512 bytes**，增长 **14,147,584 bytes／约 13.49MiB**，低于 32MiB 目标。正常 Windows UI＋生命周期专项 **4 passed／19.831s**；本轮没有用该指标替代长文推理或配音全流程峰值内存测量。
- [x] 完整 CUDA `cmake --build out/build/windows-release-cuda` 成功；最终测试目标重编译成功。最终 `ctest --preset windows-release-cuda --output-on-failure` **34/34，80.97s**，包含前后端保护、取消、源码、打包和字幕／粗剪两个启动测试。普通回归跳过真实模型与显式长文压力，不冒充真实音质验收。
- [x] CUDA 包约 **4.750GiB**，Torch **3,808,916,676 bytes／74.68%**、cuBLAS **1,080,248,544 bytes／21.18%**（统计重叠），准确 bytes 在 `package-size.json`。未增加依赖／模型／外部 Python 环境，未构建 CPU 包，不宣称 CPU 包体达标。按清理脚本先预览后移除打包暂存，保留当前构建。

代码范围仅 `tests/cpp/test_editor_integration.cpp`：显式压力测试、正常窗口性能采样及既有内存回归的可选数值报告。日志、性能 JSON、正常截图和包体统计位于 `out/validation/voice-p8-20261014/`。性能读数采用实测；初轮省略事件处理的 17.6ms 只保留为中间结果，最终以 33.1832ms 为准。

准确专项命令（运行前设置正常 Windows 平台、项目临时目录和 Qt／vcpkg PATH）：

```text
SUBCUE_P8_REPORT=D:/DSH/SubCue/out/validation/voice-p8-20261014/long-narration.json
SubCueEditorIntegrationTests.exe voiceLongNarrationStressWhenRequested
SUBCUE_P8_UI_REPORT=D:/DSH/SubCue/out/validation/voice-p8-20261014/ui-response.json
SubCueEditorIntegrationTests.exe qmlSharedProjectWindow playbackRepeatedOpenCloseDoesNotGrowUnbounded
```

**剩余验收与风险**

- [ ] P7 用户手动真实校准、费用／用量／音色验收，完成后再判定 P8 整体发布。
- [ ] 代表性真实长文音质、硬件连续试听，以及同时生成／编辑／滚动／历史分页等更多 UI 操作的 p95；本轮使用虚拟音频设备和重复有效音频夹具，不证明无听感接缝或真实长文韵律。
- [ ] 配音全流程峰值内存／磁盘压力与故障注入继续补充；已有流式实现、有界缓存及媒体回归不等于所有资源指标均已测量。没有为通过压力基线新增缓存或调整产品限制。
- [ ] L1～L4／R 后续功能仍按原依赖推进，本轮未验证这些引擎或接口，不宣称已交付；全部既定范围保留。

### Voice Studio：2026-10-14 L1 启动：后续本地引擎离线预检

用户要求暂不测试云端，继续下一阶段。按原依赖进入 L1，不等待 P7 真实校准；P7／P8 未完成项保留。本轮不安装依赖、下载或加载模型，不修改外部仓库。

- [x] 复查独立环境：Index Python 3.11.16、Torch／Torchaudio 2.8.0+cu128、Transformers 4.52.1、SoundFile 0.13.1；GPT Python 3.11.16、Torch／Torchaudio 2.7.0+cu126、Transformers 4.57.6、SoundFile 0.14.0。包元数据可用不等于 CUDA 加载或真实生成成功。
- [x] 调查本地实际代码：IndexTTS2 默认 `aux_paths=None` 会调用自动下载／迁移逻辑；接入必须显式提供本地辅助路径。GPT 默认配置仍不能当成 v2ProPlus 配置；已有 s1v3／s2Gv2ProPlus 和 sv 权重保留，需要显式型号及参考依赖。
- [x] 增加 `tools/tts_engine_preflight.py`，只读核对两个引擎必要文件、包元数据、小文件哈希及大小，支持指定辅助目录；不导入模型、不创建缓存、不调用下载。兼容 Transformers bin／safetensors 两种权重格式；报告明确区分文件／包存在与运行验收，缺失退出码 2。
- [x] 当前模型根目录内仍未找到 Index 的 w2v-bert（3 文件）、semantic codec、CAMPPlus、BigVGAN（2 文件），共 7 个；GPT 的 Chinese Roberta（config／权重／vocab）和 Hubert（config／权重），共 5 个。确切路径见 `out/validation/voice-l1-20261014/index-preflight.json`、`gpt-preflight.json`。这仅是已查目录结果，不声称其它位置不存在。
- [x] `E:/AIModels/TTS/.venv-index/Scripts/python.exe -B tests/python/test_tts_engine_preflight.py` 最终 **3/3，0.022s**，覆盖缺失显式报告、不创建缓存、不修改小文件、拒绝相对路径。首轮沙箱临时目录权限错误保留，正常权限复验通过；真实预检退出码 2 是缺失依赖的预期结果。本轮不涉及 C++、QML、CMake 或打包，未运行其构建／全量回归。

- [ ] 等待用户提供已有辅助模型目录或自行准备；已发出路径询问，不自动下载。
- [ ] 接入受管理 Worker／能力参数、情绪／发音／语速与专属权重绑定，验证离线加载、音质、显存、取消、模型／参考切换不串用。当前预检脚本不是已完成 Provider 或界面功能，不勾选 L1 完成。

本轮代码：`tools/tts_engine_preflight.py`、`tests/python/test_tts_engine_preflight.py`。声音训练、跨语言及自动情绪识别仍按原阶段独立规划，不混入本次预检。

### Voice Studio：2026-10-14 L1 辅助依赖补齐后的真实离线加载

用户确认已补齐模型，明确授权重新预检及 RTX 3060 Ti 8GB 真实加载测试；不授权自动下载或修改主权重。按实际文件重新调查，纠正上一节初始预检的结构假设，未移动／复制／修改模型文件。

- [x] Index 使用 `E:/AIModels/TTS/models/IndexTTS-2.5/hf_cache`，GPT 使用源码 `GPT_SoVITS/pretrained_models`。Index 实际 2.5 入口是 `indextts/infer_v2_5.py`、自带 `checkpoints/codec.pth` 和 tiktoken；2.0 的 `infer_v2.py`／MaskGCT 路径不用于本次加载。GPT Roberta 提供 `tokenizer.json`，真实 AutoTokenizer 已加载成功，不要求重复下载 vocab.txt。
- [x] 修正预检：检查 JSON 结构、w2v-bert／Bert／Hubert model_type、Index YAML version 2.5、实际 2.5 主权重／词表／辅助文件、GPT v2ProPlus 显式权重与 Hubert 前处理配置；兼容本地 bin／safetensors／tokenizer.json。两个环境的文件及包检查通过；现有默认 GPT 配置不直接使用。
- [x] 增加 `tools/tts_engine_load_probe.py` 显式真实加载探针：两个环境顺序执行，严格离线环境＋socket 连接阻断；不调用下载／迁移，Index 缺缓存回调仅返回已检查本地路径。每个必要文件加载前／后流式 SHA-256，缓存及 GPT 派生配置写验证目录，不覆盖源码或模型配置；不复制主权重。
- [x] Index 2.5 在 **RTX 3060 Ti 8192MiB** 上加载成功，加载耗时 **119.500s**（不含加载前后全部文件哈希）；Torch peak allocated **5,346,367,488 bytes／4.979GiB**，reserved **5,400,166,400 bytes／5.029GiB**，释放后 allocated **0**，前后哈希一致。关闭 QwenEmotion、CUDA 自定义 kernel、DeepSpeed、编译和额外加速，使用 BF16；只验证基础模型加载，不代表全部情绪能力或生成显存通过。
- [x] GPT v2ProPlus 显式 s1v3／s2Gv2ProPlus／Bert／Hubert／SV 加载成功，VITS `<All keys matched successfully>`。加载 **14.750s**；Torch peak allocated **1,830,563,328 bytes／1.705GiB**，reserved **1,851,785,216 bytes／1.725GiB**，释放后 allocated **0**，前后哈希一致。使用 CUDA 半精度，无自动 CPU 回退。
- [x] GPT 首轮因上游 i18n `relpath(__file__)` 跨 E:／D: 失败，保留日志；探针仅导入只读 i18n 时临时使用源码 cwd，随后恢复验证目录，并在进程内适配语言资源及 SV 绝对路径，未更改外部源码。第二轮成功。两个探针退出，无本轮探针残留；GPU 开始已有约 3005MiB 占用，结束约 2661MiB，桌面占用有波动。Torch 数值不等于整卡总占用，不宣称空载或真实生成余量。
- [x] `E:/AIModels/TTS/.venv-index/Scripts/python.exe -B tests/python/test_tts_engine_preflight.py` 最终 **5/5，0.072s**，覆盖只读／不创建缓存、路径拒绝、错误配置版本、tokenizer.json 与错误模型系列区分。

真实命令：

```text
E:/AIModels/TTS/.venv-index/Scripts/python.exe -B tools/tts_engine_load_probe.py --engine index --repository E:/AIModels/TTS/models/IndexTTS-2.5 --output D:/DSH/SubCue/out/validation/voice-l1-20261014/index-load
E:/AIModels/TTS/.venv-gpt/Scripts/python.exe -u -B tools/tts_engine_load_probe.py --engine gpt --repository E:/AIModels/TTS/models/GPT-SoVITS --output D:/DSH/SubCue/out/validation/voice-l1-20261014/gpt-load-2
```

证据：`out/validation/voice-l1-20261014/*preflight-corrected.json`、`index-load/load-report.json`、`gpt-load-2/load-report.json`、对应日志及测试结果；加载报告包含必要文件前后哈希。报告 `runtimeVerified` 仅表示该探针构造／加载完成，不是完整生成验收。

**当前 L1 状态**：离线预检与两引擎真实基础加载完成；未运行真实合成、使用参考克隆或做音质、生成阶段 OOM／取消、模型切换与专属权重验收，应用 Provider／界面尚未接入，L1 仍未完成。Index 的 GenerationMixin 兼容警告保留，须在生成接入时验证，不能仅因加载成功认定生成 API 可用。后续继续受管理 Worker 接入及逐引擎生成验收，云端仍暂缓。未修改 C++／QML／CMake 或运行本轮打包，不声称完整回归通过。

### Voice Studio：2026-10-14 L1 基础生成入口与真实中文验证（阶段仍未完成）

继续复用已有任务、候选、质检和编辑保护，将 Index 2.5／GPT v2ProPlus 接入应用。以下记录更新上一节的当前状态，不覆盖历史调查。用户自行补齐 resampy、语言检测和 G2PW 后，本轮没有安装依赖、联网下载、修改主权重、外部源码或其它虚拟环境；云端继续暂缓。

- [x] `LocalTtsProvider` 增加两种固定引擎；QProcess＋JSONL 复用受管理任务生命周期。界面增加引擎选择、对应环境／仓库路径、离线预检及候选生成，明确只支持基础中文克隆与 seed，不宣称情绪、时长或专属权重已经生效。Index 不将未使用的参考转写计入生成条件；GPT 要求已审核转写。
- [x] 新增 `tools/tts_external_engines.py`，固定使用 Index `infer_v2_5.py`／BF16，以及 GPT 显式 v2ProPlus 权重／CUDA FP16；模型按批次复用，完成或取消后退出。下载入口进程内改为只读本地路径适配，网络禁止；缓存只写任务输出目录，不改外部仓库。
- [x] 预检补中文生成所需 `lid.176.bin`、G2PW ONNX 与字表，并将实际使用资源、包版本及后端配置纳入内容版本。内容身份使用名称／大小／SHA，不因仓库搬迁而失效；实际 G2PW 后端与预检不同则明确失败。错误保留完整 Traceback，失败不产生可用候选或自动切换引擎。
- [x] GPT requirements 审计 **40 条适用声明全部版本匹配**；另审计实际入口及中文／SV 延迟导入。resampy 是此前未显式列出的运行需求，现已由用户安装；语言检测／G2PW 数据仅查包清单无法发现，现纳入生成预检。真实调用成功加载 full-local fast-langdetect（`high_memory`）和 G2PW，不仅检查文件存在。本轮不证明其它语言入口均可用。
- [x] 当前环境同时登记 onnxruntime／onnxruntime-gpu 1.31.0，但实际导入只提供 Azure／CPU 后端，没有 CUDA。GPT 主模型仍使用 Torch CUDA，**G2PW 实际 CPUExecutionProvider**；预检、界面和生成诊断明确记录，未偷偷更改环境。包覆盖是可能原因，尚未确认，不据此自动重装。
- [x] 完成结果提交、候选应用、人工裁切及取消回归：生成一条真实候选后设置裁切／增益，再请求错误引擎及取消第二批，原选中结果和编辑快照不变，历史仅保留已提交的候选。清理在完成通知／Future 完成前执行；Windows 硬取消后仅对已清空的本任务目录等待最多 2 秒，仍有文件或清理失败则报告并保留路径。

**最终真实专项（RTX 3060 Ti 8GB，同一已审核女声参考，seed 12345）**

- [x] Index `index-generation-6`：**3 passed／97.121s**（包含预检、模型加载、生成、第二批加载及取消）；合成耗时 **6.718s**，22,050Hz 单声道，102,912 样本／约 4.667s，峰值 **0.575409**，削波比例 0，质检无警告。Torch peak allocated **5,633,093,632 bytes**，reserved **5,876,219,904 bytes**。
- [x] GPT `gpt-generation-6`：**3 passed／37.341s**；合成 **5.875s**，32,000Hz 单声道，174,080 样本／5.440s，峰值 **0.138672**，削波比例 0，质检无警告。Torch peak allocated **1,949,441,536 bytes**，reserved **2,044,723,200 bytes**。确认 v2ProPlus 实际配置及 G2PW／语言检测实际后端；可解码并成功持久化。
- [x] 用户试听：Index 没有漏字、重复或明显读音错误，但杂音、尖锐和齿音仍存在；GPT 大致可接受，后半句“记录生活中的小变化”有偏音和发虚，用户允许暂保留。分别记为读音通过／音质待优化、暂可接受／已知问题，不以自动质检冒充完全音质通过。未给原件加增益、降噪或默认去齿音；已试听文件保持不变。

Torch 峰值是本进程分配／保留，不等于整卡总占用。此前 GPT 观测整卡峰值 3890MiB 包含桌面和其它应用，而且监测从加载后开始；不能作为本轮完整独占峰值。仅证明当前短句可运行，不承诺任意长句或其它负载的 8GB 余量。

**失败与中间证据保留**：GPT 初轮缺 resampy／fast-langdetect 资源失败；用户补齐后成功。Index 一次无 seed 重复生成异常慢且显存接近整卡上限，停止本轮拥有的测试进程并保留中断记录，原因仍未完全定位。初轮真实取消发现空目录清理竞态，Index 4／GPT 3 专项失败，不记通过；修复后最终两个专项均无 `.subcue-tts-*` 残留。最终 Index 首次沙箱尝试受 Windows 权限拒绝，正常权限重跑通过。所有失败都不替代最终验证。

**准确验证与交付范围**

- [x] `E:/AIModels/TTS/.venv-index/Scripts/python.exe -B tests/python/test_tts_engine_preflight.py`：**6/6，0.088s**；`tests/python/test_tts_worker.py`：**8/8，1.586s**。
- [x] 正常 Windows `SubCueEditorIntegrationTests.exe qmlSharedProjectWindow voiceQwenMissingEnvironmentPreservesEditing ttsTaskOwnerPersistsAndCancelsBeforeProjectChange ttsTaskRejectsSilentAudioAndContinuesBatch`：**6 passed／6.863s**；已查看 GPT 控件截图，保留 Index／GPT 正常截图。此普通窗口专项不运行真实模型。
- [x] 完整 CUDA `cmake --build out/build/windows-release-cuda` 成功；最终 `ctest --preset windows-release-cuda --output-on-failure`：**34/34，81.42s**，包含打包及字幕／粗剪两个启动测试。真实模型专项通过显式 `SUBCUE_L1_ENGINE=index|gpt`、全新 `SUBCUE_L1_OUTPUT` 运行 `voiceRealExternalEngineWhenRequested`；普通 CTest 不自动推理。
- [x] CMake 和包清单纳入两个实际使用的 Python 辅助模块，脚本变更触发重新复制；不打包模型、源码仓库和外部 TTS 环境。最终 CUDA 包 **5,100,596,814 bytes／4.7503GiB**，Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（统计重叠）。未构建 CPU 包，不宣称其包体达标。

本轮代码范围：`tools/tts_python_worker.py`、`tools/tts_external_engines.py`、`tools/tts_engine_preflight.py`、本地 Provider、VoiceStudio／任务控制器、VoiceWorkspace QML、相应 CMake／打包与 Python／编辑集成测试。证据位于 `out/validation/voice-l1-20261014/`：最终两套 `*-generation-6/audit.json`、`final-real-generation-summary.json`、依赖／ONNX 审计、测试日志、正常截图及 `final-package-size.json`。清理使用 `tools/clean-workspace.ps1` 先预览再执行，只移除本轮缓存／失败临时目录及打包暂存，保留验证工程、音频、报告、当前构建和既有修改。

**下一步与未完成项**

- [ ] L1 Index 情绪、发音／语速／时长控制及实际能力验收；GPT 用户专属权重选择、依赖记录和切换保护。当前基础入口只接 seed，**不勾选 L1 整体完成**。
- [ ] Index 杂音／齿音与 GPT 偏音／发虚优化另做可比较、非破坏性的专项；无 seed 慢生成继续定位，扩大参考／参数／长句语料后再判断稳定性。
- [ ] P7 真实云端校准和 P8 整体发布仍暂缓／未完成；L2～L4 和 R 保留原计划，不将本轮短句验收视为跨语言、情绪识别或训练支持。

### Voice Studio：2026-10-14 L1 Index 参数控制与 GPT 基础零样本复验

**本轮决策更新**：用户明确当前只有 GPT `s1v3.ckpt`／SoVITS `s2Gv2ProPlus.pth` 基础权重，没有角色微调权重；专属 GPT／SoVITS 微调权重接入改列后续可选功能，不阻塞基础零样本验收。原计划此功能保留，不自动训练或下载。云端真实验收继续暂缓。

- [x] 根据本机 Index 2.5 实际 `infer_v2_5.py` 与官方 WebUI 入口，接入 `duration_factor` 0.5～2、八类单一情绪向量及强度，并支持手工朗读中的 `<行|XING2>` 发音标注。时长倍率是近似语速／时长控制，不承诺精确目标时长；情绪向量沿用模型归一化，不承诺线性强度。仍关闭 QwenEmotion，不将表演备注发送或冒充自动情绪识别。
- [x] Qt 检查区增加时长／情绪／强度控件，复用主题与草稿提交方式；编辑后切换引擎仍保留未提交的 Index 参数。未知或越界参数拒绝，情绪与强度必须成对；清除情绪与默认时长恢复参考／默认条件。参数保存、撤销、JSON 往返和输入指纹复用现有文稿结构；只影响对应句子的生成条件，不自动替换选中 Take、裁切、增益或停顿。
- [x] C++ 与 Worker 分别校验能力和参数边界；非 Index 引擎拒绝 Index 参数和发音标注，不静默朗读特殊标记。消除本地 Provider 重复 fatal 分支，使真实失败的完整 Traceback 能进入任务诊断；不自动换引擎或伪成功。
- [x] GPT 诊断补实际 `loadedVersion`、语义词表尺寸和配置最大秒数。推理入口实际构造语义模块并加载 state_dict，SoVITS 使用实际内容版本检测和模块加载；`model.configs.version` 不等于 v2ProPlus 时拒绝。兼容性依据真实加载＋合成及文件内容指纹，不仅看文件名。判别器 s2D 不用于本次推理，SV 特征权重沿用已有本地文件。

**真实验证（现有独立环境，RTX 3060 Ti 8GB，已审核女声参考与转写，seed 12345）**

- [x] Index 控制专项 `index-controls-real`：时长倍率 **1.15**、“喜”强度 **0.2**，朗读文本为“今天我们用清晰自然的声音，记录生活中的小<变化|BIAN4 HUA4>。”；**3 passed／293.662s**，包括两批加载、真实候选、错误引擎拒绝、手动裁切及取消。合成 **53.594s**，22,050Hz 单声道，119,296 样本／约 **5.410s**，峰值 **0.695282**，削波比例 0，可解码、质检无警告。Torch allocated 峰值 **5,648,351,232 bytes**，reserved **5,932,843,008 bytes**。候选与取消记录持久化，临时任务目录和 Worker 均释放。
- [x] 用户试听 Index 控制版：整体发音和情绪表达可接受；“清晰”的“晰”和“声”仍有尖锐／齿音，允许暂保留。记录为本轮样本暂通过，不代表八类情绪全语料或每项控制已分别做音质对照。本轮同时改变三个条件，不将时长变化或听感完全归因于单一参数。
- [x] 按用户新要求复验 GPT `gpt-zero-shot`：显式使用 `E:/AIModels/TTS/models/GPT-SoVITS/GPT_SoVITS/pretrained_models/s1v3.ckpt` 和 `v2Pro/s2Gv2ProPlus.pth`，参考来自项目声音快照，转写“感觉耗电耗得更快了”。**3 passed／100.454s**；实际型号 **v2ProPlus**、语义词表 **1025**、配置 maxSeconds **57**，真实合成成功。合成 **17.937s**，32,000Hz 单声道、174,080 样本／**5.440s**，峰值 **0.138794**，削波比例 0，完整可解码、质检无警告。Torch allocated **1,949,441,536 bytes**，reserved **2,044,723,200 bytes**；G2PW 仍明确记录 CPU 后端，语言检测 full-local。取消与人工裁切保护通过，无 Worker 或临时目录残留。
- [x] GPT 音质沿用用户此前对同条件短句“暂可接受，后半句偏音／发虚暂保留”的反馈；本轮新音频已展示，尚未另获人工试听结论，不将自动检测写成人工通过。

本轮两个引擎均比此前基础样本耗时更长，记录真实数值，不宣称提速或确定原因。Torch 峰值不等于整卡总占用；短句成功不保证所有长句和桌面负载的 8GB 余量。没有给原 Take 加降噪、去齿音、增益或削波处理，没有修改模型／源码／Python 环境或连接云端。

**验证与打包**

- [x] `E:/AIModels/TTS/.venv-index/Scripts/python.exe -B tests/python/test_tts_worker.py`：最终 **9/9，1.450s**；同环境 `tests/python/test_tts_engine_preflight.py` **6/6，0.084s**。
- [x] `SubCueVoiceDocumentTests.exe` **9 passed／0.064s**，包含参数往返、局部输入失效、越界／类型／不完整情绪拒绝及其它引擎拒绝标注。正常 Windows `SubCueEditorIntegrationTests.exe qmlSharedProjectWindow` 最终 **3 passed／5.425s**，包含草稿跨引擎提交、波形手势、正常中文截图；已查看 `index-controls-ui.png`。
- [x] 最终 `cmake --build out/build/windows-release-cuda` 成功；`ctest --preset windows-release-cuda --output-on-failure` **34/34，80.29s**，包含打包及两个启动测试。普通回归不自动推理；真实专项显式指定 `SUBCUE_L1_ENGINE`／新 `SUBCUE_L1_OUTPUT`，Index 额外指定 `SUBCUE_L1_CONTROLS=1`，执行 `voiceRealExternalEngineWhenRequested`。
- [x] 最终 CUDA 包 **5,100,612,115 bytes／4.750315GiB**；Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（统计重叠）。无新增依赖／模型／外部环境入包；未构建 CPU 包，不宣称其尺寸通过。按清理脚本先预览再移除打包暂存，保留当前构建、所有验证项目／音频和历史日志。

中间失败保留：重编译时真实测试尚在运行，Windows 锁定可执行文件导致 LNK1104；待测试退出后顺序重编译通过。一个正常窗口单项在模拟裁切断言失败，而完整回归通过；补手势前窗口激活／布局准备后，最终单项与完整回归均通过，不删除保护断言。CTest 的无窗口截图会覆盖同名临时截图，因此正常 Windows 截图在全量回归结束后单独保存，不将乱码的无窗口截图作为视觉验收。

代码范围：`tts_types.h`、`voice_document.cpp`、`local_tts_provider.cpp`、`voice_studio_controller.cpp`、`VoiceWorkspace.qml`、两个 TTS Worker 模块，以及文稿／编辑集成／Python 测试。证据目录 `out/validation/voice-l1-controls-20261014/`，包含 Index／GPT `audit.json`、`real-controls-summary.json`、`gpt-compatibility-summary.json`、实际 WAV、正常截图、准确测试日志和 `package-size.json`。

**当前边界与下一阶段**

- [x] 本轮 Index 控制入口及 GPT 基础零样本闭环通过；角色专属权重为后续可选功能，未实现、未验收，不能以基础权重冒充专属音色。
- [ ] 多参考／更多参数与八类情绪的代表性人工对照、长句性能稳定性及尖锐／齿音／偏音持续优化，保留为 L1 扩大验收事项，不宣称整个引擎能力或所有语料已通过。
- [ ] 下一阶段可进入 L2 中英日跨语言／混语克隆，先复核实际型号能力、明确参考语言与目标语言并分语言记录结果；L3 自动情绪识别与本轮向量控制分开。P7 真实云端／P8 整体发布、L4 与 R 均保留原范围和未完成状态。

### Voice Studio：2026-10-14 L2 语言输入、Index 三语言闭环与 Qwen 环境阻塞

本轮进入 L2；保留各引擎单独验收，未把 Index 结果当作 Qwen 回退或成功证明。未安装依赖、修改环境、下载模型、训练权重或连接云端；没有改项目／编辑器设置。

- [x] 检查实际入口：Qwen 0.6B Base 配置 `talker_config.codec_language_id` 包含 Chinese／English／Japanese，SDK 支持显式目标语言；Index 2.5 原生 `lang=zh/en/ja`，含日语处理器。GPT 的语言能力虽有源码入口，但日／英资源与处理路径尚未独立验收，应用暂保留中文能力声明，不按系列宣传直接启用。
- [x] 每句目标语言选择接入草稿、撤销、稳定 ID、JSON 定向更新及原有输入指纹；混语先拆为独立句段，每段显式选择中／英／日，再按文稿顺序编排。未启用自动翻译或猜测语言；单段混语仍待实验。更换目标语言仅影响该句生成条件，旧 Take／人工裁切保留。未显式改变语言时不把未知 JSON 语言改成中文；中文数字规范化建议拒绝其它目标语言，避免输出错误语言的修正文案。
- [x] 声音审核与派生审核增加参考语言，生成新不可变版本；项目快照复制该元数据，全局更新不改变旧项目。旧样本无语言字段时明确默认中文，并在审核界面提示核对；其它语言使用手工标注，不将中文 ASR 冒充多语言审核。
- [x] `TtsRequest` 保存独立参考语言；旧 v1 中文参考不改变已有音频输入指纹，旧快照缺字段仍可恢复。非中文参考语言纳入指纹，类型错误／篡改／引擎不支持均拒绝。Qwen 按实际目标语言调用，克隆条件缓存计入参考语言；Index 按实际语言映射调用。Mock、GPT 和首版云端仍按各自已接入能力限制，不悄悄换引擎。
- [x] 增加显式 `voiceCrossLanguageWhenRequested` 专项：用户审核过的中文女声参考与转写 → 三句稳定片段 → 三语言同批生成 → 候选 → 连续试听／导出 → 保存。普通 CTest 跳过真实推理；`SUBCUE_L2_ENGINE=qwen|index` 指定独立引擎，`SUBCUE_L2_OUTPUT` 必须为新目录。

**Index 2.5 真实结果（中文参考，seed 12345，RTX 3060 Ti 8GB）**

- [x] 干净构建最终专项 `index-cross-language-clean` **3 passed／56.097s**。原始语言顺序为中文“今天的天气很好。”、英语“The weather is nice today.”、日语“今日はいい天気です。”；模型在同一批次复用，不因语言切换重启三次。
- [x] 三句均为 22,050Hz 单声道、可解码、削波比例 0、质检无警告。中文 49,152 样本／约 2.229s，合成 **4.405s**，峰值 **0.506592**；英语 50,176 样本／约 2.276s，合成 **2.109s**，峰值 **0.291992**；日语 44,032 样本／约 1.997s，合成 **2.047s**，峰值 **0.560944**。本批 Torch allocated 峰值 **5,611,768,832 bytes**，reserved **5,853,151,232 bytes**，不是整卡独占峰值。
- [x] 显式语言序列按文稿顺序导出 48,000Hz、24-bit PCM、单声道 WAV；355,277 样本／约 7.402s，包含各段原有停顿，完整可解码，无静默漏段。任务、Take、语言输入和资产引用保存成功，参考原件 SHA 保持一致，Worker 与临时任务目录释放。
- [x] 首轮 Index 专项 **3 passed／69.250s**；最终重编译后重跑。两轮三句解码 PCM 逐样本相同；文件 SHA 不同未被当成声音不同，比较结果在 `index-repeat-pcm-comparison.json`。不将该样本的重复性外推为全部语料确定性。
- [x] 用户随后反馈“可以听得清中英日语言，且准确”，但声音“不够清晰、不够干净，尤其是中文和日语”。本轮中英日语言／读音准确度暂通过；**音质未通过**，中文与日语的清晰度、干净程度待优化。未据此确认口音、音色相似度或全部自然度指标；自动质检无警告不代表主观音质达标。

**Qwen 实际阻塞，未伪通过**

- [ ] 首轮真实预检失败 **2 passed／1 failed，40.077s**；尚未加载合成模型或产生可用音频。当前独立环境：qwen-tts 0.1.1、Torch／Torchaudio 2.10.0+cu126、Transformers 4.57.3、SoundFile 0.14.0、Librosa 1.0.0、huggingface-hub **2.2.0**、Tokenizers 0.22.2、Safetensors 0.8.0、Accelerate 1.12.0。
- [x] 只读依赖审计发现 Transformers 要求 `huggingface-hub>=0.34.0,<1.0`；完整 `pip check` 另显示 Tokenizers／Gradio／Gradio-client 也被同一个 2.2.0 版本违反，未发现另一个不同的缺失包。已请求用户自行调整该独立环境或明确授权；尚未获回复，不自动修复、不修改其它环境或绕过版本检查。
- [x] Qwen 预检增加直接运行依赖声明与实际版本校验，Hugging Face／Tokenizers／Safetensors 进入环境指纹；不匹配先于大权重哈希和模型加载拒绝，保留完整 Traceback。环境修复后需重新预检并创建新尝试；旧任务／历史不覆盖。`qwen-environment-audit.json`、`qwen-pip-check.txt`、`qwen-check-full-traceback.jsonl` 保存真实证据。

**构建失败调查与最终验证**

- [x] 首轮完整回归出现 Asr Mock 崩溃和编辑集成失败。定位为 Agent 构建命令设 `VSLANG=1033`，而现有 Ninja 缓存依赖前缀为中文“注意: 包含文件:”；测试对象记录 `#deps 0`，`test_asr.cpp.obj` 仍是修改 `TtsRequest` 前的旧布局。没有以重试掩盖 ABI 不一致。
- [x] 用临时探针确认 `VSLANG=2052` 与现有前缀匹配，再执行现有 CUDA 构建 `cmake --build out/build/windows-release-cuda --clean-first` 全量重编译。未改 CMake／构建脚本／用户配置；Asr 对象恢复 **325 项依赖**，包含 `tts_types.h`；此前崩溃专项最终 **3 passed／1.078s**。以后此缓存下的构建命令必须保持编译输出语言与前缀一致，不再强制英语。
- [x] 一个早期正常窗口组合未创建波形控件；增加截图与 QML 警告上下文后，单项和原组合复验通过，没有据此修改业务布局或删除保护断言。该次间歇性失败保留，尚无独立的运行时根因结论。
- [x] 最终 Worker Python **10/10，1.490s**，离线引擎预检 **6/6，0.085s**；干净构建文稿 **10 passed／0.064s**，正常窗口语言／审核／派生保护 **5 passed／6.567s**。已查看正常中文跨语言控件截图。
- [x] 干净构建 `ctest --preset windows-release-cuda --output-on-failure` 最终 **34/34，80.99s**，包含媒体生命周期、打包与字幕／粗剪启动测试；不是用早期失败前或历史日志代替验收。普通回归不自动调用真实模型或付费服务。
- [x] 最终 CUDA 包 **5,100,625,391 bytes／4.750328GiB**；Torch **3,808,916,676 bytes／74.68%**、cuBLAS **1,080,248,544 bytes／21.18%**（统计重叠）。没有新增运行依赖或将模型／外部 TTS 环境放入包；未构建 CPU 包，不宣称其尺寸验收。按清理脚本先预览再移除打包暂存及本轮编译探针，保留当前构建、验证音频、项目、报告和历史失败记录。

代码范围：TTS 输入／本地能力、VoiceStudio／声音库控制器及两个 QML 界面、Worker 语言／依赖预检，以及文稿、Python、真实跨语言和窗口测试。证据在 `out/validation/voice-l2-20261014/`，最终以 `index-cross-language-clean/audit.json`、`index-clean-summary.json`、`clean-rebuild.txt`、`asr-header-dependencies.txt`、`clean-ctest.txt` 和 `package-size.json` 为准。

**L2 未完成项与下一步**

- [ ] 修复用户 Qwen 环境后，完成独立中英日真实生成、语言切换、缓存及音质验收；不能用 Index 结果勾选 Qwen。
- [ ] 优化 Index 跨语言清晰度与干净程度，重点比较中文／日语，并进行对照试听；扩大英语／日语参考来源、不同角色与方向的读音／口音／相似度语料。本轮只实测中文参考到三种目标语言，不承诺完全无口音或所有组合通过。
- [ ] GPT 中英日离线资源和推理路径分别接入与验收；单段混语保留独立实验，不自动启用。
- [ ] L2 整体仍未完成；L3 自动情绪识别、L4 云端声音管理、P7 真实云端／P8 整体发布及可选专属权重保持原范围，不提前跳过依赖或宣称交付。

### 2026-10-15：L3 音频情绪识别——人工审核基础完成，自动识别未接入

**阶段状态：进行中，不能标为 L3 完成。** 本轮未下载模型、安装环境、加载情绪模型或调用云端 API；保留全部既有工作区改动。

**实际调查与识别器评审**

- [x] 检查 `E:/AIModels/TTS/models` 与 `E:/AIModels/ASR/models` 的文件清单，在已检查位置未找到 emotion2vec／SenseVoice 等独立音频情绪识别权重；没有据此断言其它目录也不存在。用户随后明确回复“未准备”；L3 真实自动识别等待独立模型准备，不自动下载或安装，已完成的人工标签功能保留。
- [x] 现有 Index QwenEmotion 是合成用文本情绪控制，不作为音频情绪识别。现有三个 TTS 主权重和中文 ASR 能力也没有被伪装为情绪分类器。
- [x] 查询 [emotion2vec 官方仓库](https://github.com/ddlBoJack/emotion2vec) 与 [emotion2vec+ Base 模型卡](https://huggingface.co/emotion2vec/emotion2vec_plus_base/raw/main/README.md)：可作为候选的 Base 约 90M 参数，输出九类标签与 scores，输入要求 16kHz 音频。没有把原始 emotion2vec 特征模型视为已微调分类器，也没有将 scores 当成已校准准确率。
- [x] 模型卡声明为 `other / model-license` 并指向 FunASR；查阅 [FunASR MODEL_LICENSE](https://github.com/modelscope/FunASR/blob/main/MODEL_LICENSE)。暂未最终选定或核实本地识别型号对应许可，不把代码许可当成权重许可，不宣称许可验收完成。官方示例会自动下载，后续须显式本地加载、离线预检与受管理 Worker，不能直接执行在线示例。
- [ ] 待模型明确后，核对本地配置、标签顺序、权重、准确许可、运行环境与依赖；再评估 CPU 批次或共享 GPU 调度。没有安装 FunASR 或修改三个用户 TTS 环境，也没有预先承诺速度／显存。

**本轮实现**

- [x] 声音参考审核增加人工情绪：未分类、愤怒、厌恶、恐惧、高兴、中性、其它、悲伤、惊讶、无法判断。`manualEmotion` 是版本元数据，缺字段的旧版本仍可读取；显式未分类用于取消原标签，不将缺标签推断为中性。
- [x] 保存标签随审核创建新不可变声音版本，不修改旧版本、默认版本或已复制的项目快照。标签描述参考音频，不自动转换成 Index 情绪向量、不自动改变合成参数，也不编造模型预测、置信度或模型版本。
- [x] 派生音频单独人工标注，准备派生时重置为未分类；后端不继承原音频的 `emotionPrediction`／`manualEmotion`，避免裁切、处理后的音频沿用旧结论。派生标签改变后需要重新确认 A/B 审核。
- [x] 存储与控制器拒绝未知标签及非法类型；审核接口增加默认参数，既有 C++ 调用仍可使用。声音版本元数据扩展未新增数据库表、依赖、线程或通用识别器框架。
- [x] 沿用现有深色审核界面和控件，显示“自动音频识别尚未接入”；正常窗口验证标签选项、默认未分类和试听期间标签／转写草稿保留，已检查真实中文截图。

**验证与证据**

- [x] `SubCueProjectStoreTests.exe manualEmotionVersionsPreserveProjectSnapshot`：**3 passed／85ms**，验证新旧版本隔离、项目快照保护及非法元数据拒绝。该项使用存储字节 fixture，不宣称有效音频或真实识别通过。
- [x] 正常窗口 `SubCueEditorIntegrationTests.exe voiceReferenceReviewCreatesNewVersion voiceBatchAndDerivedVersionsPreserveManualReview qmlSharedProjectWindow`：**5 passed／6.889s**，验证人工标签保存、派生审核、草稿保护及实际 UI；截图已保存在验证目录。
- [x] 首次受限构建无进度，停止后使用正常 Windows 权限构建。首次有效编译发现新界面测试变量名与已有 `emotion` 冲突，已改为 `referenceEmotion`。最终保持 `VSLANG=2052` 执行 `cmake --build out/build/windows-release-cuda` 成功；失败记录保留，不当成最终通过。
- [x] `ctest --preset windows-release-cuda --output-on-failure`：**34/34／84.01s**，包含存储、编辑、媒体生命周期、打包与两个启动测试；本轮无真实情绪模型推理，不以这些回归代替识别准确性验收。
- [x] CUDA 包 **5,100,633,071 bytes／4.750335GiB**；Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（重叠统计）。未新增运行依赖，未打包外部权重／TTS 环境；未构建 CPU 包。打包验收完成后按清理脚本先预览再移除暂存，保留当前构建与验证证据。

代码范围：`voice_library` 元数据校验、声音库控制器审核参数、`VoiceLibraryDialog.qml` 和存储／编辑集成测试。证据：`out/validation/voice-l3-20261015/` 中的 `build.txt`、`build-final.txt`、`emotion-storage.txt`、`emotion-editor.txt`、`voice-reference-review.png`、`ctest.txt` 与 `package-size.json`。

**未完成事项与风险**

- [ ] 音频情绪识别 Worker、模型预检、取消／销毁、预测标签／分数／模型版本、输入资产指纹绑定尚未实现；不把本轮人工标签当成自动识别交付。
- [ ] 自动识别关闭、低置信待审、预测不覆盖人工标签、模型切换／派生后失效、批量流程，以及实测阈值与错误类型仍需在识别器明确后完成。阈值须用审核语料校准，低分或未知不能强行分类。
- [ ] 真实识别语料应包含不同说话人、录音质量及中英日语言，分别记录错误边界；现有单段参考不够证明情绪准确性。
- [ ] L2 Index 中文／日语清晰度和杂音问题仍未通过；Qwen 的 huggingface-hub 环境修复仍待用户，未重新验收。真实云端、L4 和 P8 保持原未完成状态，未自动跳过或启用。

### 2026-10-15：L3 emotion2vec+ Base 已提供，文件预检通过，独立环境待提供

- [x] 用户提供 `E:/AIModels/AudioEmotion/emotion2vec_plus_base`，此前“模型未准备”条件已解除。本轮只读检查该目录，未改权重、配置或其它 Python 环境，未自动下载模型。
- [x] 主权重 **1,118,245,678 bytes**，SHA-256 **60710b5aae1dbe69bdac8920028fb05882d4314fd09031922b4b61ee9e7aadbd**，与 [emotion2vec 官方权重页面](https://huggingface.co/emotion2vec/emotion2vec_plus_base/blob/main/model.pt) 一致。四个必需文件（model.pt、config.yaml、configuration.json、tokens.txt）均存在，已保存大小与 SHA；checkpoint ZIP 容器包含 data.pkl，未执行 pickle 反序列化。
- [x] 配置声明 Emotion2vec、depth=8、embed_dim=768；本地配置引用的文件存在，词表是九类，末项 `<unk>` 应映射 unknown。configuration.json 中仍为 `iic/emotion2vec_base` 的旧 hub 标识，后续必须显式加载本地权重／配置／词表，不能按旧标识触发下载。静态检查通过不等于参数兼容性和模型加载通过。
- [x] 只读核对现有 ASR 环境中的 FunASR 1.3.1 源码和依赖声明：已有 Emotion2vec 实现及本地模型配置入口，支持复用现有受管理进程和 InferenceManager GPU 门禁。FunASR 顶层批量导入可能吞掉子模块异常，实际预检须直接导入情绪模块并保留完整 Traceback；没有擅自用 ASR／TTS 环境执行本轮情绪推理。
- [x] AudioEmotion/.venv 与 .venv-emotion 均无 python.exe，已询问用户是否在其它位置准备独立环境，尚未得到路径。按用户本轮“缺少独立环境先报告安装方案”要求，先提供安装方案，不自动安装或修改现有三个 TTS 环境。
- [x] 本机 Launcher 清单包含 CPython 3.11.16；建议新建 `E:/AIModels/AudioEmotion/.venv`，起始组合为 Python 3.11、Torch／Torchaudio 2.6.0+cu124、FunASR 1.3.1、NumPy 1.26.4、OmegaConf 2.3.0、Hydra Core 1.3.2、SoundFile 0.13.1、Librosa 0.11.0 及相关声明依赖。完整 PowerShell 命令和后续验收步骤在 `out/validation/voice-l3-20261015/model-preflight/environment-setup.md`。这些版本是待实测方案，不宣称已完成依赖安装或加载兼容性验收。
- [x] 本地 README 标为 Apache License 2.0，HF 模型卡为 model-license，已保留来源口径差异，不覆盖原文或重新许可权重；不复制入发布包。
- [ ] 独立环境提供后完成：依赖导入、参数映射／分类头匹配、离线实际加载、中英日音频识别、耗时／显存、取消／退出、预测不覆盖人工标签、低置信待审与阈值审核。三语言本轮均 **not_run**，尚不能以文件预检或既有 L2 TTS 音频证明情绪识别准确性。

验证：以系统 Python `-I -S` 的标准库检查 JSON、词表、配置关键字段、checkpoint 容器与 SHA，全部断言通过，结果保存于 `out/validation/voice-l3-20261015/model-preflight/model-files.json`。仅新增验证证据、安装方案及本进度记录，未改业务代码、运行构建或重新宣称历史 CTest 结果；现有人工情绪标签保持原实现，自动识别和低置信审核仍待完成。

### 2026-10-15：L3 独立环境、真实分类及审核接通；情绪准确性未通过

**状态：离线运行与审核基础通过，模型准确性失败，L3 整体未完成。** 用户已经提供独立环境；此前“环境待提供”条件解除。本轮不安装／下载任何依赖或模型，不改三个 TTS 环境和模型原件。

**环境与实际兼容性**

- [x] 使用 `E:/AIModels/AudioEmotion/.venv/Scripts/python.exe`，Python **3.11.16**、Torch／Torchaudio **2.7.0+cu126**、FunASR **1.4.16**、ModelScope **1.40.2**、NumPy **2.4.6**、OmegaConf **2.3.1**、Hydra Core **1.3.7**、SoundFile **0.14.0**、Librosa **0.11.0**。`pip check` 无损坏依赖，直接导入成功，CUDA 识别 RTX 3060 Ti 8GB；没有为迎合旧安装建议调整已可用版本。原安装方案已注明仅供历史参考，不应重建或覆盖当前环境。
- [x] 实际目录仍为 `E:/AIModels/AudioEmotion/emotion2vec_plus_base`。Worker 读取并比对四项文件哈希，主权重锁定此前核验的官方 Base SHA。网络连接在进程内禁用，显式加载本地配置、九类词表和权重；不使用 configuration.json 的旧 hub 标识，不调用在线 AutoModel 下载入口。
- [x] 首轮严格参数检查发现配置中的预训练重建 decoder 没有分类权重；核查已安装 FunASR 源码后，使用其 `remove_pretraining_modules()` 移除未参与 extract_features 的 decoder。剩余 **185 项 state 参数**逐项映射并验证形状，strict=True 装载，推理模型 **93,178,133 个参数**，不允许缺失推理参数以随机初始化继续执行。
- [x] 发现 PyYAML 将官方 `norm_eps: 1e-05` 读作字符串，造成 layer_norm TypeError。适配层显式转换并检查该已知字段的数值范围，未改本地 config.yaml 或外部 FunASR 源码。两次早期失败和完整 Traceback 均保留。

**实现与编辑保护**

- [x] 新增 `tools/audio_emotion_worker.py`：受管理进程、单批次本地 Base 分类，支持 1–100 条输入；每条限 0.25–30 秒，验证解码、有限值、非静音及源指纹，转换至 16kHz 单声道后调用 FunASR 原生 `model.inference`。模型和音频结束时重新核对哈希；不下载或写回参考、模型及外部环境。
- [x] 通过现有 `InferenceManager::run` 共用 GPU 门禁与可见队列，整个批次占用单个 GPU 任务，复用现有取消、终止／等待、会话代次和销毁保护。独立 Worker 结束释放模型，不增加常驻服务或新调度器。
- [x] 参考样本审核增加手动、默认关闭的“实验”识别入口，用户填写独立 Python 与本地模型路径；不硬编码用户环境路径、不修改应用／编辑器设置。结果显示标签、原始分数、类别差距及待审状态；窗口可滚动，已经检查正常中文截图。
- [x] 预测与人工 `manualEmotion` 分离，预测保存模型文件版本、运行包版本、音频 SHA 和九类分数，不能自动覆盖人工标签或改变 TTS 参数。结果只先进入当前审核草稿，保存新审核版本才持久化；旧声音版本和项目快照不变，派生音频不继承原预测。
- [x] 暂定低分策略为 top 分数 <0.60、前两类差距 <0.15，或 other／unknown；策略版本随结果保存。这些阈值未做准确率校准。由于真实样本出现高分误判，**所有建议（包括高分）均须明确人工审核后才能随新审核版本保存**。用户可“忽略当前模型建议，保留人工标签”继续建档，识别失败不阻止人工路径。
- [x] 存储拒绝损坏分数分布、标签／top／差距／待审标记不一致、错误哈希以及与参考资产不匹配的预测；项目复制保留不可变元数据，不双写其它业务数据库。
- [x] CMake 和打包必需文件列表纳入实际使用的 Worker，package-manifest.txt 由现有流程生成；不把独立 Python 环境、FunASR 包或权重复制进便携包。

**真实中英日结果与用户审核：准确性不通过**

- [x] 使用此前 L2 Index 合成并已审核读音的三段天气旁白（每段约 2 秒），本轮执行真实 emotion2vec GPU 推理；它们是合成音频，不能冒充天然录音语料。
- [x] 原生接口最终批次冷加载／预检 **8.032s**，含收尾哈希的批次 **8.938s**；中文分类 **0.219s**、英文 **0.016s**、日文 **0.031s**。Torch allocated 峰值 **445,163,520 bytes**，reserved 峰值 **562,036,736 bytes（536MiB）**；这是进程内 Torch 统计，不是整卡占用，机器原有桌面程序显存没有被算成模型峰值。
- [x] 原始预测：中文 **surprised / 0.999899**，英文 **fearful / 0.999612**，日文 **fearful / 0.999961**。用户明确反馈“我听到的情绪都是正常情绪”，本次人工真值记录为 **neutral**，三条均不匹配（仅此三个样本 0/3，不外推总体准确率）。
- [x] 为排除封装计算偏差，对照并切换到 FunASR 原生分类入口，三条九类分数最大差值 **0.0**，仍是上述误判。未改标签顺序、映射或阈值来伪装通过，也未擅自训练或下载替代模型。完整原始分数及人工审核在 `human-emotion-review.json`；高分不等于判断正确。

**验证证据**

- [x] 明确授权的 `SUBCUE_L3_OUTPUT=.../native-api`，执行 `SubCueEditorIntegrationTests.exe voiceEmotionWhenRequested voiceEmotionReviewAndCancellation qmlSharedProjectWindow`：**5 passed／26.364s**。覆盖三语言实际分类、真实控制器预测／保存、人工标签不被改写、Worker 启动阶段取消、GPU 队列释放及窗口。运行通过仅证明执行链路，不代表情绪准确性通过。
- [x] Python `test_audio_emotion_worker.py` **5/5**，覆盖低分、小差距、unknown／other、高分未审核与非法分布；这是合成分数策略测试。存储测试验证非法／过期预测拒绝、版本及项目快照保护；审核测试验证低分拦截、忽略建议、人工保留和排队取消。
- [x] 最终路径输入／滚动窗口专项 **3 passed／6.544s**，源码卫生专项 **9 passed／52ms**。已查看正常窗口 `reference-emotion-controls.png`；最终移除硬编码路径后截图为 `reference-emotion-portable.png`。
- [x] 首轮完整回归 **33/34／72.15s**，失败为新增 QML 中写死本机 python.exe 路径触发原有源码卫生检查。按既有 TTS 界面约定改为用户输入，不修改／弱化断言。最终保持 **VSLANG=2052** 构建成功；`ctest --preset windows-release-cuda --output-on-failure` **34/34／71.99s**，包含媒体生命周期、打包与两个启动测试。普通 CTest 不自动运行真实模型。
- [x] 最终 CUDA 包 **5,100,670,729 bytes／4.750370GiB**；Torch **3,808,916,676 bytes／74.68%**，cuBLAS **1,080,248,544 bytes／21.18%**（重叠统计）。未构建 CPU 包，不宣称 CPU 包体积已验收。完整验收后只按清理脚本预览／移除 package 暂存，保留当前构建和证据。

本轮代码范围：声音库控制器／元数据校验、参考审核 QML、独立识别 Worker、编辑／存储／Python 测试及 CMake 打包登记。证据在 `out/validation/voice-l3-20261015/real-emotion/`，以 `native-api/result.json`、`native-api-tests.txt`、`human-emotion-review.json`、`ctest-verified.txt`、`package-size-final.json` 和源码卫生最终专项为准；历史失败不删除。

**仍未完成**

- [ ] L3 情绪准确性／阈值校准未通过，保持默认关闭的实验入口和人工优先；后续需要更广的带人工情绪标签的天然中英日语料、不同角色／录音条件，再定位是合成域差异还是模型泛化问题。本轮没有足够证据断言具体根因。
- [ ] 尚未验收全部情绪类别、长段分布、批量声音库的完整 UI 工作流及 GPU kernel 执行中的取消；本轮取消覆盖排队和真实 Worker 启动阶段，不夸大为全部生命周期压力场景。
- [ ] L2 音质／Qwen 环境问题、P7 真实云端、L4 与 P8 仍按原状态保留，没有被本轮执行链路通过替代或自动开启。

### 2026-10-15：L2 继续——Qwen 环境修复与三语言复验，Index 齿音派生对照

**状态：Qwen 环境阻塞解除，中文／英文短句人工暂通过，日文未通过；L2 整体未完成。** 本轮没有修改业务代码、模型权重、参考原件、选中 Take 或 Index／GPT 环境；未调用云端。

- [x] 重新执行 Qwen `pip check`，确认 huggingface-hub 2.2.0 仍违反 Transformers／Tokenizers／Gradio 的依赖范围。用户明确授权“修复这一依赖并复验”。先执行 `pip install --dry-run --no-deps 'huggingface-hub>=0.34,<1.0'`，确认只需更换该包，再在 `E:/AIModels/TTS/.venv` 执行 `pip install --no-deps huggingface-hub==0.36.2`。仅卸载 2.2.0、安装 0.36.2（约 566kB 包），未更新其它依赖、安装模型或修改其它 TTS 环境。随后 `pip check` 无损坏依赖；保留安装预览／实际安装与检查日志。
- [x] 通过现有 GPU 调度，设置 `SUBCUE_L2_ENGINE=qwen`、`SUBCUE_L2_OUTPUT=.../qwen-cross-language`，运行 `SubCueEditorIntegrationTests.exe voiceCrossLanguageWhenRequested`：**3 passed／105.126s**，覆盖离线模型预检、中文参考→中英日生成、候选／应用、连续试听、WAV 导出和工程保存。沿用 BF16 主模型、FP32 声学编码／解码、seed 12345 及现有分句节奏；无模型下载，不用 Mock 代替真实输出。
- [x] 三句原生输出均 24kHz 单声道、可解码、削波比例 0、质检无警告。中文 51,840 样本／2.16s、生成 **16.061s**、峰值 **0.236470**；英文 51,840 样本／2.16s、生成 **4.406s**、峰值 **0.218513**；日文 32,640 样本／1.36s、生成 **2.781s**、峰值 **0.421170**。Torch allocated 峰值 **2,865,543,680 bytes**，reserved **3,143,630,848 bytes**；不是整卡显存峰值。参考指纹、队列释放等由真实闭环测试检查。
- [x] 用户试听反馈“中文和英文可以，日文暂时不行”：分别记录中文／英文在本轮短句通过，日文未通过。已请求日文具体问题，不自行推断读音、漏字、节奏或音色根因，不把三语言全部标为通过。完整标记在 `qwen-human-review.json`。
- [x] Index 源码只读核对：当前 BF16 主要用于 GPT 部分，声学生成和 BigVGAN 输入已按浮点处理；没有机械套用 Qwen 的 FP32 tokenizer 修复，也没有为试验修改外部源码或贸然将整模型升为 FP32。
- [x] 对原参考及三语言 Index 输出做 Welch 频带测量。各文件均无削波；中文输出 6–8kHz 的相对频带占比约 11.95%，参考约 1.30%，日文输出 8–11kHz 占比也较高。但文本／发音不同，不能仅据占比判断齿音根因、噪声水平或自然度。
- [x] 仅在验证目录生成可撤销的 A/B 派生文件：1024 点 STFT、768 重叠，按 6–10kHz 相对能量控制高频最多 **3dB** 衰减，并对频率及时间过渡平滑。没有全局增益、限幅、硬噪声门或自动改原件；三句及整序列采样率／样本数不变、有限、无削波，原文件 SHA 均保持。实验参数与每文件指纹／峰值保存在 `index-sibilance-ab/audit.json`。
- [ ] Index A/B 已提供试听，效果待用户判断。未将验证脚本加入默认合成、实时试听或导出链路；未经确认不以高频衰减代替模型音质达标。

证据：`out/validation/voice-l2-20261015/` 的 `hub-dry-run.txt`、`hub-install.txt`、`pip-check.txt`、`qwen-real-tests.txt`、`qwen-summary.json`、`qwen-cross-language/audit.json`、`qwen-human-review.json`、`index-spectrum.json` 和 `index-sibilance-ab/`。本轮未改业务代码或 CMake，未重新构建或打包，不能将此前完整 CTest／包体结果写成本轮重新验收。

未完成：Qwen 日文定位与人工验收；Index 中文／日文清晰度和齿音；更多参考方向和天然语料、GPT 跨语言。L3 准确性仍失败，P7 真实云端暂缓，P8／L4 等后续状态不因本轮 Qwen 执行通过而改变。

### 2026-10-15：L2 日文节奏与尾段杂音继续定位（等待新对照试听）

- [x] 用户补充 Qwen 日文问题为节奏偏快、音量和语气持续下降；Index B 仍有杂音，尤其末尾。日文与 Index 音质均不据此标为通过，B 未进入默认处理。
- [x] 25ms 短时测量显示 Qwen 日文原版后部有效帧 RMS 低于前部；但音素／自然韵律会影响 RMS，不将客观音量直接当作语气，也不实施自动逐句增益拉齐。Index 原序列与 B 最后 250ms 追加静音的 RMS／峰值都为 **0**，残留声音位于原 Take 末段，不能简单当成追加停顿的底噪裁掉；是否与句尾擦音重叠尚未确认。记录在 `tail-diagnostics.json`。
- [x] 核对已安装 Qwen SDK 的说明，`non_streaming_mode=False` 是模拟流式文本输入，不是真实流式音频 API。仅在忽略的验证目录复制现有 Worker，将推理配置中的 `nonStreamingMode` 改为 True；业务源码、应用默认值、外部模型源码及权重保持原样。该配置纳入独立预检和模型版本哈希，没有以旧模型版本冒充新输入。
- [x] 对同一日文文本“今日はいい天気です。”、已审核中文女声参考／转写、seed 12345、BF16 主模型与 FP32 tokenizer 做单变量完整文本输入试验。离线预检通过；CLI 验证进程单独运行并等待退出，没有并行启动 TTS 模型，没有下载模型／依赖。此次是受工具管理的独立验证进程，不把它写成经应用 GPU 队列执行。
- [x] 完整文本输入候选 **24kHz／53,760 样本／2.24s**（原版 32,640 样本／1.36s），生成 **5.063s**，峰值 **0.785439**，有限、可解码、无削波。Torch allocated **2,868,810,752 bytes**、reserved **3,196,059,648 bytes**；声学提示缓存 1 份。已生成 `japanese-full-text-ab/Japanese-full-text.wav` 供 A/B，未替换工程、Take 或已通过中文／英文的节奏。
- [x] 首次准备验证副本时断言预期有硬编码调用参数失败，发现实际调用读取推理配置；修正为仅替换配置字段后离线预检与生成通过。原失败日志保留，不改业务代码或绕过模型版本检查。
- [ ] 新日文候选的节奏、尾部音量、语气与完整读音等待用户试听。时长增加不等于主观质量通过；若仍不自然，需要进一步评估参考韵律与 0.6B Base 跨语言能力边界，不用强行裁切／归一化掩盖。

证据仍在 `out/validation/voice-l2-20261015/`，新增 `tail-diagnostics.json`、`japanese-full-text-ab/check-verified.jsonl`、`request.json`、`generation.jsonl`、`generation-stderr.txt`、`audit.json` 与候选音频。本轮未改业务代码／打包配置、未运行构建或完整 CTest；L2 及其它未完成阶段状态保持。

### 2026-10-15：L2 日文完整文本输入对照——节奏略改善，音色一致性失败

- [x] 用户反馈 B“改善了一点，但是声音完全不一样了”。记录为节奏有限改善、音色一致性失败；B 不采用，不能视为日文验收通过，应用仍保持原模式。
- [x] 复查实际安装 SDK：`generate_voice_clone` 两种模式均传入同一参考 speaker embedding、参考文本与参考音频 code；`generate_icl_prompt` 的 `non_streaming_mode` 分支改变文本／codec embedding 的上下文组织方式，并非单纯速度控制。相同参考与 seed 不保证跨生成模式音色一致，不能将该开关当作只改变节奏的旋钮。
- [x] 本轮 A/B 的参考、转写、语言、文本、seed 和解码精度保持一致；候选较响，音量差也可能影响主观音色判断，但目前没有响度匹配试听或客观说话人相似度证据，不将其当成音色漂移的已确认根因。
- [ ] 后续须同时满足节奏、句尾表现及音色一致性，不通过改变音色来换取时长增加；当前实验候选保留作失败对照，不替换现有 Take、不修改全局默认值。Qwen 日文、Index 尾噪及 L2 整体仍未完成。

### 2026-10-15：统一体验改造后继续 P8 离线集成验收

**状态：P8 新增长文备份恢复验收通过，整体发布尚未完成。** 用户明确三模块与设置页已经重新改造，后续不得退回旧交互。本轮先读取 `docs/UnifiedExperienceValidation.md` 与实际实现，保留未命名工程生成／恢复、默认音色与局部覆盖、批量候选复用、统一设置草稿、后台切换以及人工裁切规则。历史计划中的“首次生成前必须保存”等旧规则不重新引入。

- [x] 启动时保存所有现有 QML 的 SHA-256，执行后逐项比对完全一致；本轮没有修改业务控制器、工作区、设置页、模型、外部 Python 环境或凭据，也没有调用云端。仅扩展现有集成测试及追加文档说明。
- [x] 基线执行 `set VSLANG=2052` 后运行 `tools\\build-windows.bat cuda`：构建成功，完整 CTest **34/34，79.38s**，包括媒体生命周期、打包与两个启动测试。此结果是新增测试编译前的当前业务基线；新增长文检查随后单独编译和执行，不把原有测试输出冒充新增检查结果。
- [x] 扩展 `voiceLongNarrationStressWhenRequested`，在原 11,000 字／500 句／每句 3 个候选的有效音频夹具上，通过实际 ProjectSession 接口执行便携备份、恢复、历史及资产归属检查、再次导出。恢复后正式文稿完全一致、dirty 为 false、历史末页与各句 3 个 Take 完整，全部恢复音频位于新工程自己的 `.assets`，不依赖夹具源路径或备份目录。恢复前后 WAV SHA-256 一致。
- [x] `cmake --build out/build/windows-release-cuda --target SubCueEditorIntegrationTests` 成功；正常 Windows 插件执行 `voiceLongNarrationStressWhenRequested recoveryPreservesUnnamedDocumentAndFieldDrafts voiceSaveWaitsWithoutCancellingGeneration voiceUnsavedWorkspaceGeneratesExportsAndMigratesAssets voiceDocumentMockCandidatesProtectManualCropAndVersions`：**7 passed，0 failed／92.600s**（含初始化／清理）。覆盖字段草稿、外部冲突、保存等待不取消生成、未命名工程生成／导出／首次保存资产迁移、裁切与版本保护。
- [x] 本机夹具压力数据：初次保存 **205ms**、含历史保存 **255ms**、历史构造 **42.800s**、预览 **9.514s**、首次导出 **8.490s**、便携备份 **4.968s**、恢复 **5.120s**、恢复后导出 **10.723s**；成稿 **31,200,000 samples／48kHz 单声道**。波形 **1,024,000 bytes**，试听缓存 **99,231,019 bytes**，均低于既有限额。控制器选句 p95 **0.2159ms** 不含渲染，不能替代 UI 帧响应指标；统一体验改造已有的原生窗口性能记录保留。
- [x] CUDA 包 **5,101,007,625 bytes／4.750684GiB**，Torch **3,808,916,676 bytes**（含部分 cuBLAS），cuBLAS **1,080,248,544 bytes**（重叠统计），低于 4.8GiB 目标。未新增依赖、未构建 CPU 包。打包暂存按清理脚本预览后移除，保留当前构建、原有 dist 与验证证据。

证据：`out/validation/voice-p8-after-unified-20261015/` 的 `ui-baseline.json`、`ui-unchanged.txt`、`build-ctest.txt`、`test-build.txt`、`long-and-protection-tests.txt`、`long-backup-restore.json`、`package-size.json` 及清理日志。

**未完成与下一步边界**：上述是夹具／Mock 流程验收，不是新一轮真实音质验收。P7 真实云端仍按用户要求暂缓；P8 尚缺真实长文音质与未覆盖的磁盘／发布故障注入等最终验收。L2 Qwen 日文音色／节奏、Index 尾噪和 GPT 跨语言仍未通过或未验收；L3 准确性仍未通过；L4 的声音上传授权／留存／费用设计需单独确定后再接入，不能因本次离线测试通过而自动开启云端。后续继续复用统一设置、GPU 调度和现有工作区，不退回旧布局或旧生命周期。

### 2026-10-15：P8 导出故障与损坏备份恢复保护验收

**状态：本轮故障用例通过，P8 整体仍未完成。** 继续保持三模块与设置页统一体验；所有 QML 文件执行前后的 SHA-256 一致。本轮仅改 `tests/cpp/test_audio_quality.cpp`、`tests/cpp/test_editor_integration.cpp` 和验收文档，不改业务接口、默认交互、模型、依赖或用户环境，不调用云端。

- [x] 新增数据驱动的导出故障测试：第一句已处理后取消、第一句处理后修改第二句 Take、Windows 目标 WAV 在发布前被其它句柄占用。通过真实取消标志、有效 WAV 改写和禁止写入／删除共享的 Windows 文件句柄注入，不给产品代码添加测试开关。用例分别验证处理已开始、返回失败与明确错误、原有完整 WAV SHA-256 不变、浮点／发布临时文件没有残留；解除故障后同一编排可以重新导出完整 96,000 样本音频。
- [x] 扩展现有打开失败测试，覆盖损坏备份的实际 `ProjectSession::restoreProject`：失败后项目身份与代次、字幕／粗剪文本、配音正文及独立人工朗读、字段恢复草稿、dirty 状态保持；会话解除 busy／changingProject，未发布目标数据库／资产目录，无备份临时目录残留。随后执行撤销可正常恢复之前的朗读内容，未破坏既有命令历史。
- [x] 受管理 Windows 句柄由作用域守卫负责异常路径释放，正常路径也显式解锁后再检查和重试；无新线程、后台任务或运行依赖。
- [x] 定向构建 `cmake --build out/build/windows-release-cuda --target SubCueAudioQualityTests SubCueEditorIntegrationTests` 成功。`SubCueAudioQualityTests.exe`：**11 passed／608ms**（含三个故障数据行）；正常 Windows 平台执行 `sharedProjectFailedOpenPreservesCurrent voiceAudioCacheExportAndCancellationProtectFrozenArrangement voiceAudioDestructionHasNoLatePublication`：**5 passed／908ms**（含初始化／清理）。损坏 WAV 测试的 FFmpeg 诊断是预期夹具输出，不是测试失败。
- [x] 使用 `VSLANG=2052`，最终 `tools\\build-windows.bat cuda`：构建成功，**34/34 CTest／79.45s**，包括新增故障用例、媒体生命周期、打包和两个启动测试。`git diff --check -- tests/cpp/test_audio_quality.cpp` 通过。
- [x] CUDA 包 **5,101,007,625 bytes／4.750684GiB**，Torch **3,808,916,676 bytes**（含部分 cuBLAS），cuBLAS **1,080,248,544 bytes**（重叠统计）；低于 4.8GiB，未新增包内容，未构建 CPU 包。验证后通过清理脚本先预览再移除仅本轮打包暂存，当前构建、原有 dist 和验证证据保留。

证据：`out/validation/voice-p8-failures-20261015/` 内的 `final-test-build.txt`、`audio-failure-tests-final.txt`、`session-failure-tests.txt`、`build-ctest.txt`、`ui-baseline.json`、`ui-unchanged.txt`、`package-size.json` 与清理日志。`docs/UnifiedExperienceValidation.md` 同步追加保护范围。

**未完成事项与风险**：没有填满实际磁盘、断电或强制终止数据库写入，不将文件占用／取消测试冒充磁盘耗尽和断电恢复验收。真实长文音质、其它未覆盖的系统故障和 P8 最终发布仍待完成。L2 音质／GPT 跨语言、L3 情绪准确性保持原未完成状态；真实云端继续暂缓，L4 上传授权与费用设计不被本次测试自动确认或启用。后续实现继续以统一体验改造后的行为为基线。

### 2026-10-16：P8 数据库写入进程突然退出恢复

**状态：新增进程退出事务验收通过，P8 整体未完成。** 本轮仅修改存储测试和进度记录，保留三模块／统一设置交互，不改业务代码、用户工程、模型、外部环境，不调用云端。

- [x] 在实际 ProjectStore 创建的临时 SQLite 工程中，用受管理 QProcess 启动独立 Python 标准库 sqlite3；固定 DELETE 回滚日志、FULL 同步和小页缓存，写入 1MiB 模块数据，断言回滚日志已超过 512 bytes，然后以 `os._exit(73)` 退出，跳过连接析构及解释器清理。分别覆盖事务提交前退出和提交成功后立即退出。
- [x] 提交前退出时，重新由 ProjectStore 打开后人工模块与 revision 保持；提交后退出时，新数据及新 revision 保留。两种情况项目 UUID 不变，均可继续保存并生成可读取备份，结束时没有回滚日志残留。
- [x] 子进程由测试所有，超时／断言失败时作用域守卫负责终止并等待。测试通过 `SUBCUE_CRASH_TEST_PYTHON` 显式启用，不硬编码用户 Python 路径；本轮使用项目既有 `.venv-inference/Scripts/python.exe` 的标准库，未导入 Torch 或修改环境。进程返回值必须为 73，脚本异常不能冒充预期退出。
- [x] 定向构建 `SubCueProjectStoreTests` 成功；显式启用故障测试后执行完整存储组：**37 passed／1.601s，0 skipped**。`git diff --check -- tests/cpp/test_project_store.cpp` 通过。
- [x] 保持故障测试环境变量，使用 `VSLANG=2052` 执行 `tools\\build-windows.bat cuda`：构建成功，**34/34 CTest／90.88s**，包含新增故障用例、既有媒体生命周期、打包及两个启动测试。
- [x] CUDA 包 **5,101,007,625 bytes／4.750684GiB**；Torch **3,808,916,676 bytes**，cuBLAS **1,080,248,544 bytes**（重叠统计）。没有新增运行依赖，未构建 CPU 包。仅本轮打包暂存经清理脚本预览后移除，当前构建与原有 dist 保留。

证据：`out/validation/voice-p8-crash-20261016/` 中的 `build.txt`、`store-tests.txt`、`build-ctest.txt`、`package-size.json` 和清理日志。

**边界与下一步**：本轮证明 SQLite 事务在进程直接退出后的回滚／提交可恢复性，子进程直接写入测试数据库，不等同于在应用保存或资产发布的每一阶段强制退出。没有实际断电、填满磁盘或测试存储设备故障；这些系统故障及真实长文音质仍未验收。P7 云端实测继续暂缓，L2 音质与 GPT 跨语言、L3 准确性、L4 授权设计保留原状态，不能据此宣称完整计划完成。

### 2026-10-16：P8 应用层另存失败与继续编辑验收

**状态：新增另存失败保护通过，P8 整体未完成。** 只扩展编辑器集成测试与本记录，未修改三模块、设置页、业务控制器、模型或环境，未调用云端。

- [x] 新增 `sharedProjectFailedSaveAsPreservesEdits` 三个数据行：目标工程文件已存在、目标配套资产目录已存在、目标父路径为文件。通过真实文件状态注入，使用实际 ProjectSession 另存接口，不增加产品测试开关。
- [x] 失败后原工程磁盘 SHA-256 不变，目标哨兵字节不变；当前工程路径、存储路径、项目 UUID 和代次不变。字幕／粗剪未保存文本、独立手工朗读与备注、字段恢复草稿保持；dirty 仍为 true，busy 解除。撤销／重做仍正常；随后另存有效新路径成功，dirty 归零，持久文稿与当前编辑一致。
- [x] 定向构建 `SubCueEditorIntegrationTests` 成功。正常 Windows 平台执行 `sharedProjectFailedSaveAsPreservesEdits sharedProjectFailedOpenPreservesCurrent recoveryPreservesUnnamedDocumentAndFieldDrafts`：**7 passed／1.407s，0 skipped**，包含三种新故障、损坏恢复和跨会话恢复保护（含初始化／清理）。
- [x] `VSLANG=2052` 下执行 `tools\\build-windows.bat cuda`：构建成功，**34/34 CTest／89.29s**，包含媒体生命周期、打包和两个启动测试。本轮全量未设置数据库进程退出探针的显式环境变量，该可选探针的实际通过证据仍以前一节为准，不冒充本轮重新执行。
- [x] CUDA 包 **5,101,007,625 bytes／4.750684GiB**，Torch **3,808,916,676 bytes**、cuBLAS **1,080,248,544 bytes**（重叠统计）；无新运行依赖，未构建 CPU 包。打包暂存按脚本预览后清理，当前构建、原有 dist 与证据保留。

证据：`out/validation/voice-p8-save-failure-20261016/` 的 `build.txt`、`session-tests.txt`、`build-ctest.txt`、`package-size.json` 和清理日志。

**未完成与阶段边界**：本轮验证应用层可控保存失败，不是应用进程写入中途被杀、真实断电或磁盘耗尽。P8 的真实长文音质与最终发布仍未完成，不能通过继续添加夹具测试代替。L2 音质／GPT 跨语言、L3 准确性保持原未完成状态；云端实测继续暂缓，L4 上传、留存及费用决策不自动确认。后续仍按统一体验改造后的行为接入，不恢复旧布局和旧保存限制。

### 2026-10-16：P8 真实中文连续旁白首轮——执行通过，等待人工音质审核

**状态：转入真实模型验收，不再仅补夹具边界测试；P8 未整体完成。** 本轮使用已批准的女声参考和转写，未修改业务代码、三模块／设置页、模型或外部环境，未联网下载或调用云端。

- [x] 扩展现有 `voiceRealQwenWhenRequested` 显式验收入口：可通过 `SUBCUE_P8_REAL_TEXT` 读取 UTF-8 文稿，原短句默认入口保持；按实际句数检查结果、所有非目标句及任务计数。新增实际连续试听缓存准备检查，仍复用现有任务所有者、GPU 队列、音频处理和项目存储，不增建工具框架。
- [x] 本轮文稿 **264 字符／12 句**，包含日常叙述、长短句和“九点三十分”；使用 Qwen 0.6B Base、BF16 主模型、FP32 tokenizer 和 `nonStreamingMode=false`，沿用当前节奏，不采用日文失败实验的完整文本模式。此文稿是约一分钟多句验收，不能冒充 1 万字真实长文验收。
- [x] 真实应用控制器完成预检、12 句生成／候选应用、连续试听缓存准备、整篇 WAV 导出、第一句修改／重新生成、人工裁切确认保护、其余 **11 句完全不变**、取消和存储保存。最终 **13 个完成任务、1 个取消任务**；完成任务均检查 FP32 tokenizer，参考 SHA 与模型 manifest 不变，结束时 GPU 调度队列为空。
- [x] 原始成稿 `first-batch.wav`：**48kHz／24-bit PCM／单声道、3,098,880 samples／64.56s**，峰值 **0.382092**，削波比例 **0**，质检 warnings 为空。静音占比含自然停顿及编排，不用其证明无漏字或听感流畅；人工审核尚未通过。局部重生成版本另存 `regenerated.wav`，不覆盖原始对照。
- [x] 示例完成任务记录 Torch allocated 峰值 **2,961,698,304 bytes**、reserved **3,303,014,400 bytes**；这些是任务诊断，不冒充全机显存或全流程内存峰值。生成期间整卡一次采样 5,969MiB，结束一次采样 2,849MiB，包含其它桌面程序，不当作模型独占显存。
- [x] `cmake --build out/build/windows-release-cuda --target SubCueEditorIntegrationTests` 成功。显式设置 `SUBCUE_P6_REAL_QWEN=.../qwen` 与 `SUBCUE_P8_REAL_TEXT=.../manuscript.txt`，正常 Windows 平台执行 `voiceRealQwenWhenRequested`：**3 passed／260.442s**（包含初始化／清理、预检、全部批次、局部重生成和取消，不是纯合成耗时）。本轮未重跑完整 CTest 或打包，未变更运行文件／依赖；不重复引用前轮包体为新测量。
- [ ] 已提供原始成稿并询问漏字／重复／错读、清晰度、音色／音量稳定性和接缝，等待用户试听；`humanAudioAccepted` 保持 false。

证据：`out/validation/voice-p8-real-20261016/` 中的 `manuscript.txt`、`build.txt`、`real-tests.txt`、`summary.json` 和 `qwen/audit.json`；实际工程、资产及两个 WAV 保留在 `qwen/`。

**剩余**：先取得本轮人工结论并针对问题处理，再扩大到代表性长文和更多参考角色；不能凭可解码／无削波宣布音质达标。P7 云端实测继续暂缓，P8 最终发布、L2／L3 已知问题及 L4 仍保持原状态。现有统一体验保持不变。

### 2026-10-16：真实中文旁白人工反馈——首句音色与第五句异响未通过

- [x] 用户反馈首句“清晨，我推开窗户，看见阳光落在对面的屋顶上”音色明显不同；25–28 秒有异响，其它基本正常。本轮音质不标为整体通过。
- [x] 核对全部已完成任务：实际模型版本、项目声音版本、参考音频 SHA 各只有一个唯一值；均为同一 Qwen Base 和同一批准女声参考，没有请求级切换声音的证据。相同模型与参考不保证每句生成音色一致，尚不能断言漂移的具体原因。
- [x] 按原生生成时长与每段 300ms 停顿定位：首句约 0–5.52s，第五句约 20.08–28.00s，因此 25–28s 异响落在“翻开第一页，那些熟悉的笑容，又让我想起了从前”内部，而非后面的追加段间停顿。原始结果与逐句时长保存在 `completed-review.json`、`timing-review.json`，人工反馈在 `human-review.json`。
- [ ] 下一步重点检查第一、第五句原生 Take 与导出对应部分，区分生成异常和处理链路；尚未修复、未采用降噪／音色替换或更改默认模式。未改用户 Take、业务界面、模型或环境，未运行新推理或新一轮构建。

### 2026-10-17：P7 真实 Flash 单句定位与 WAV 占位长度兼容修复

**状态：云合成／下载已实际执行，解码根因定位并以真实原件离线验证修复；P7 整体未完成。** 保留当前三模块和统一设置界面，未改用户文稿／声音分配、模型或外部环境。

- [x] 用户反馈确认后没有候选。只读检查当前工作库发现失败码 download_failed，已有输入 17／输出 40 Token；旧任务没有保存下载 URL，不能恢复旧结果。本轮未重发该任务。
- [x] 修复 CloudTtsProvider 丢失错误上下文：保留 download／decode 阶段、网络错误码、请求 ID、具体错误说明；从说明中隐藏当前密钥及 HTTP URL，限制长度。既有 download_failed 错误码兼容保留，用量不清零，不自动重发合成。
- [x] 用户分别明确授权两次各 1 句测试，均为“你好，这是 SubCue 云端配音测试。”、当前北京 Flash／baiqinglan_v3.1、软预算 ¥0.10。实际各发送一次合成，各输入 15／输出 37 Token；每次按保存的原价快照折算 ¥0.0004665，合计 ¥0.000933（实际扣费以服务商账单为准）。没有第三次真实合成，也没有读取／输出密钥值。
- [x] 两次均成功返回合成和下载，失败发生在严格解码，提示损坏数据包。第一次失败清理删除了原始音频，探针随后增加诊断原件留存；第二次经独立授权保留原件。真实探针必须显式指定新输出目录和 Voice ID，先写 attempted 标记；同一目录拒绝再次付费，不在普通 CTest 中运行。
- [x] 实际原件 142,124 bytes：标准 44 字节 PCM WAV 头、24kHz／16-bit／单声道；RIFF 长度字段为 0x7fffffbf、data 长度为 0x7fffff9b，均为约 2GiB 占位值，实际 PCM 142,080 bytes。FFmpeg 因声明长度超过实际数据而标记损坏。
- [x] 新增固定 Flash 容器适配：只有上述两个精确占位值、标准 PCM／24kHz／单声道／16-bit／块对齐和字节率均匹配，且下载成功、文件有完整偶数字节采样、总量不超过 128MiB 时，才按实际字节数修正两个长度字段。其它 WAV 保持原严格解码路径；不会全局忽略 corrupt 标记或补造音频。
- [x] 用第二次已下载原件通过 Mock HTTP 离线回放真实 Provider，无新增网络请求。修复后 AudioQuality 通过，24kHz／71,040 samples／2.96s；测试逐字节比较修复前后的 PCM 区域完全一致，只有容器长度改变。试听文件 `live-single-raw/repaired.wav` 已展示，人工音质尚未确认。
- [x] Mock 增加下载重试两次不重复 POST、解码失败、用量保持、错误阶段和请求 ID 检查。首次新增用例受旧过期状态及 QJsonArray.last 值写回问题影响，已修复并保留失败日志。最终 `cloudTtsPricingAndPaidRequestProtection` **3 passed／27ms**；控制器 `voiceFlashCalibrationAndUnknownResultsAreProtected` **3 passed／1.605s**；真实原件离线回放 **3 passed／47ms**。
- [x] 最终 `VSLANG=2052` 下 `tools\\build-windows.bat cuda` 构建成功，**35/35 CTest／97.42s**，含云配置、媒体、打包及两个启动测试；普通 CTest 跳过显式真实付费探针和本地真实模型。CUDA 包 **5,101,339,913 bytes，约 4.751GiB**，Torch **3,808,916,676 bytes**、cuBLAS **1,080,248,544 bytes**（重叠统计）；无新增依赖，未构建 CPU 包。暂存按预览流程清理，保留当前构建与证据。

证据：`out/validation/voice-p7-download-20261017/` 的构建／Mock／控制器／离线回放日志、`wav-fix-ctest.txt`、包体和清理记录；两次授权分别在 `live-single/audit.json`、`live-single-raw/audit.json`，后者包含原始 WAV 与仅修正头部的试听副本。失败任务 audit 保持真实失败状态，不回写成远端新请求通过。

**剩余与边界**：本轮修复已经编入应用，完整真实项目候选落库／试听／费用预测校准仍需用修复后路径验收；当前证据是实际合成下载加真实字节离线回放，不冒充第三次在线成功或整阶段完成。旧失败任务无法补下载，不提供虚假的恢复按钮。声音注册／Plus／Next、L2 音质与 L3 准确性仍未完成；真实调用继续遵守逐次明确授权范围。

### 2026-10-17：P7 已下载云音频的完整项目离线闭环通过

**状态：修复后的项目流程通过离线回放，未新增在线付费请求；P7 整体未完成。** 本轮只新增集成测试及进度记录，业务代码、三模块、设置页和外部环境保持原样。

- [x] 新增 `voiceFlashDownloadedWavCompletesProject`：普通回归使用有效 WAV 夹具；显式 `SUBCUE_FLASH_PROJECT_FIXTURE` 可使用已授权下载的真实占位头 WAV，`SUBCUE_FLASH_PROJECT_OUTPUT` 可保留独立测试工程。所有网络接口固定为 RecordingHttpClient，不访问真实云配置／凭据，也不发送真实请求。
- [x] 实际应用控制器完成统一工程保存、云声音版本导入和绑定、费用预览、单句校准确认、头部适配、音频质检与候选落库。新结果仅成为候选，不自动选中；显式应用后，连续试听缓存准备与 WAV 导出通过。再次预览显示复用 1 个候选，再次确认不增加模拟合成次数，历史仍只有 1 条。
- [x] 通过实际 TtsTaskController／ProjectStore 检查 completed、15 输入／37 输出 Token 及价格快照金额 0.0004665；本次响应和用量是模拟回放，不是新账单证据。保存后的 voiceStudio 快照与当前文稿相同，原始 WAV SHA 不变，GPU 队列为空。
- [x] 使用此前第二次真实请求保留的 `download-original.wav`，定向执行 `voiceFlashDownloadedWavCompletesProject voiceFlashCalibrationAndUnknownResultsAreProtected`：**4 passed／2.385s，0 skipped**（含初始化／清理）。生成独立 `cloud-replay.subcue-project` 及资产；导出 **48kHz／24-bit PCM／单声道、156,480 samples／3.26s**，包含默认 300ms 停顿，峰值 0.458788、削波为 0、质检 warnings 为空。
- [x] 最终 `VSLANG=2052` 下 `tools\\build-windows.bat cuda` 成功，**35/35 CTest／98.57s**，包括新用例默认夹具、云配置、媒体、打包和两个启动测试；真实付费探针仍跳过。CUDA 包 **5,101,339,913 bytes／约 4.751GiB**，Torch **3,808,916,676 bytes**、cuBLAS **1,080,248,544 bytes**（重叠统计）；未新增依赖或构建 CPU 包。暂存经预览清理，当前构建与证据保留。

证据：`out/validation/voice-p7-project-replay-20261017/` 内的 `build.txt`、`project-tests.txt`、`build-ctest.txt`、包体与清理日志；`project/replay-audit.json` 明确 networkUsed=false、syntheticUsage=true，保留真实字节来源 SHA，避免将回放工程当成实际云账单或生产声音档案。

**剩余**：未进行第三次付费请求；修复后的在线端到端真实项目与多句价格预测偏差尚未独立验收，人工音质尚未确认。本轮缓存准备／导出是自动化链路验证，不宣称真实硬件连续试听通过。P8 长文音质及 L2／L3／L4 未完成项保持原状态，不靠新增 Mock 测试替代真实质量门槛。

### 2026-10-17：P8 首句音色／第五句异响定位与局部候选对照

**状态：排除成稿阶段显著样本改写，已生成局部对照，等待人工音质反馈。** 本轮不改业务代码、统一界面、原工程或选中 Take，不改默认生成参数／模型／环境；无云端请求。

- [x] 只读原验收工程，按实际任务与 Take 关联找到 12 句原生 24kHz 音频，与成稿 48kHz 对应片段的偶数位采样比较。首句及第五句相关系数分别 **0.9999999999997732／0.9999999999997372**，最大采样差均 **8.940696716308594e-08**；其余句也高度一致。该结果没有显示导出引入音色替换、显著增益变化或额外拼接，问题优先在原生生成结果中定位；不把采样相关性冒充主观音色评估。
- [x] 原首句 0–5.52s；第五句 20.08–28.00s，用户反馈的25–28s位于第五句后段。导出原生／成稿对照及25–28s片段，仅生成验证副本、不覆盖原件。
- [x] 用已授权中文女声参考及转写，维持 Qwen Base、BF16 主模型、FP32 tokenizer、nonStreamingMode=false、参数 seed 留空；单独验收工程只输入两句问题正文，生成新候选，不把它们自动应用到原文稿。随机生成可能漂移，未把同配置重生成宣称为确定性修复。
- [x] 显式设置既有真实入口的 `SUBCUE_P6_REAL_QWEN` 与 `SUBCUE_P8_REAL_TEXT`，运行 `voiceRealQwenWhenRequested`：**3 passed／84.717s**，覆盖两句生成、应用／缓存准备／导出、单句再生成、非目标句保护、人工裁切确认、取消、保存及参考／模型未变化。测试另有既有的第三条再生成与取消分支，不将全部耗时当成两句纯推理时间。
- [ ] 两句新候选已提供试听；等待首句音色是否接近正常句、第五句后半段异响是否消失，以及有无新错读／杂音／音色变化的人工反馈。原结果保持未通过，不能据无削波或任务成功勾选音质。

证据：`out/validation/voice-p8-quality-20261017/` 中 `compare.py`、`comparison.json`、原生／成稿／异响片段、`problem-sentences.txt`、`real-tests.txt`、`retry-audit.json` 和独立 `qwen-retry` 工程。新候选为 `sentence-1-retry.wav`、`sentence-5-retry.wav`。本轮没有源码变更、未重新构建／完整 CTest／打包；不复用前轮全量数字冒充本轮结果。P7 在线验收及其它未完成阶段保持原状态。

### 2026-10-17：局部候选试听反馈与首句起始异响诊断

- [x] 用户确认新首句音色正常、第五句后半段无原异响；但新首句开头有异响。因此只记录上述局部改善，新首句及整篇仍未通过。
- [x] 新首句起始采样约 0.00001073，前 120ms 每 10ms 峰值不超过约 0.000101；原生 24k 与成稿对应 48k 偶数采样最大差 **1.1920928955078125e-07**。没有发现导出造成显著采样跳变的证据；这些指标不能证明无可听异响，也不能确定根因。
- [x] 在验证目录制作 `sentence-1-retry-padded.wav`，仅前后各追加 1 秒静音，不裁切人声、不降噪、不淡入或改变增益；用于区分播放器启动瞬态与随人声出现的生成异常。原件、原工程及选中结果保持。
- [ ] 已询问异响在点击播放时还是约 1 秒后随人声出现，以及爆音／沙沙声／额外人声或气声特征，等待反馈后再定位。没有盲目裁掉句首或宣称修复。

证据新增 `out/validation/voice-p8-quality-20261017/onset-review.json` 和上述静音诊断副本。本轮仅离线采样分析与试听副本，未运行模型、调用云端、修改业务代码或重新构建。

### 2026-10-17：首句异响随音频后移，单句独立重生成

- [x] 用户反馈前置 1 秒静音后，异响位于 1–2 秒。对应原 Take 约 0–1 秒，优先定位原生句首内容；不直接断言具体噪声类型，不裁切“清晨”的发音。第五句已改善结果继续保留。
- [x] 检查当前 GPU 计算进程列表后，仅运行一个本地 Qwen Worker，为首句生成独立候选。复用此前实际请求的输入快照与 inputHash，生成新任务 UUID，模型版本、参考、转写、BF16／FP32、nonStreamingMode=false 和空参数保持一致。原工程、选中 Take、默认配置和模型原件未修改，无云请求。
- [x] 本轮为工具管理的独立 Worker 诊断，不冒充经应用 InferenceManager 执行；进程等待结束并正常返回 0。真实生成 **11.438s**，**24kHz／132,480 samples／5.52s**；有限、峰值 **0.259653**，未削波；与最初失败版本逐样本比较不相同。Torch allocated **3,042,143,744 bytes**、reserved **3,477,078,016 bytes**，并非整卡占用。
- [x] 制作前后各 1 秒静音的试听副本，无裁切、降噪、淡入或增益处理；仅在验证目录保存。客观指标不能证明无异响或音色一致。
- [ ] 等待用户确认新候选在加静音后 1–2 秒是否仍有异响，以及音色是否保持；尚未应用到原工程或标为音质通过。

证据：`out/validation/voice-p8-quality-20261017/first-only-retry/` 下的 `request.json`、`events.jsonl`、`stderr.txt`、`quality.json`、`first.wav`、`first-with-silence.wav`。无业务源码变更，未运行构建／完整回归／打包；P8 和其它未完成阶段保持。

### 2026-10-17：首句再次试听——无异响但音色未通过

- [x] 用户明确新候选前置静音后 1–2 秒没有异响，但音色不对。本候选不采用，`humanAccepted` 继续为 false；原先音色改善但句首有异响的候选也不标为通过。第五句此前改善结论保留。
- [ ] 同配置随机重生成尚不能同时保证音色一致与无异响，不能宣称解决了根因。下一步应检查参考音频充分性及实际支持的生成控制；没有获得证据前，不修改默认参数、不盲目裁切句首或用降噪伪装通过。
- [x] 未修改原工程或选中结果，未运行新的生成、构建或云请求。P8 整篇音质与首句仍未通过，其它阶段状态保持。

### 2026-10-17：P8 参考来源与随机条件复核，准备完整参考审核

- [x] 复核 P3 原始记录：当前 3.000083s 参考来自 `tests/测试音频1/女声/中文/电量不足.mp3` 的前 3 秒及派生处理，原文件 **7.584s／48kHz／双声道**。历史后半段 ASR 草稿为“是你看错了吧”，并不等于完整参考已经人工审核，也不能假定后半段同一说话人。
- [x] 只读本地 generation_config：do_sample=true、temperature=0.9、top_p=1、top_k=50，subtalker 同样采样；此前实际请求 parameters 为空，Worker 只有显式 seed 时才设置 torch.manual_seed。随机生成存在，固定 seed 可用于控制实验条件，但不能据此保证音色、无噪声或完全确定性。
- [x] 制作完整原录音审核副本，仅左右声道平均并转 PCM24 WAV，不裁切、降噪、调增益，不修改原 MP3；源 SHA 前后验证一致。未运行 ASR／TTS、新云请求、模型或构建，未改变档案或选中 Take。
- [ ] 已请求确认完整原音频是否同一说话人，以及“感觉耗电耗得更快了。是你看错了吧。”的完整转写。审核前不用于新克隆，不能将时长增加直接宣称为修复。若审核通过，下一步才以固定实验条件对比短参考与完整参考；默认配置仍保持。

证据：`out/validation/voice-p8-reference-20261017/prepare.py`、`reference-audit.json`、`full-reference-review.wav`。所有新候选及首句音质仍为未通过，P8 整体状态不变。

### 2026-10-17：参考条件固定 seed 对照，等待人工判断

- [x] 用户已确认完整原录音同一说话人，并要求继续。完整转写未单独明确确认，仍标记草稿，不写入正式审核档案；本轮在说明该边界后仅作独立实验。
- [x] 从同一 MP3 制作前 3 秒与完整 7.584 秒两份参考，均只做声道平均与48kHz PCM24转换，没有降噪、增益、裁切人声或采用此前处理版本。两条请求均显式 seed=12345、同一首句／Qwen版本／BF16主模型／FP32 tokenizer／nonStreamingMode=false；短参考使用已确认转写，完整参考使用匹配的完整转写草稿。因此评估的是参考长度及配套转写条件，不能排除草稿误差，也不能据此宣布长度为唯一根因。
- [x] 通过工具管理的单个本地 Worker 批次顺序生成两版，正常退出，非 Mock、无云请求。此诊断没有经过应用GPU队列，不冒充应用任务验收。请求使用独立诊断JSON哈希，不伪称项目规范输入哈希；不写回用户工程或声音库。
- [x] A短参考生成 **24kHz／134,400 samples／5.60s**，耗时 **12.360s**、峰值 **0.226654**；B完整参考生成 **24kHz／138,240 samples／5.76s**，耗时 **11.500s**、峰值 **0.254808**。两版均有限、无削波；这些指标不能证明无异响或音色相似。另各加1秒前后静音，仅供一致位置对照。
- [ ] 已明确展示的是两版新合成而非参考原录音，询问音色相似度及第1–2秒是否有异响，等待反馈；两版均未自动采用。原先第五句改善结果保留，首句与P8整体仍未通过。

证据：`out/validation/voice-p8-reference-20261017/controlled/` 的 `prepare.py`、`conditions.json`、`request.json`、`events.jsonl`、`stderr.txt`、`results.json`、`short-synthesis*.wav` 与 `full-synthesis*.wav`。本轮未修改业务源码／默认参数／模型／环境，未运行构建、完整回归或打包。

### 2026-10-17：参考条件人工结论——完整参考音色更近，清晰度仍不足

- [x] 用户反馈“B版本更接近参考，但是清晰度不如A版本”。记录本次首句对照的完整参考音色优于短参考、短参考清晰度较好；不能外推为所有句子、角色或模型的结论。
- [ ] 用户本次没有明确确认句首异响是否消失，保持未确认；也没有确认B整体音质通过，humanAccepted保持false。完整转写仍为草稿，不补记人工审核。
- [x] B仅作为后续音色一致性对照基线，未改默认参考、声音档案、原工程或选中Take；A继续保留为清晰度对照。不能通过提升清晰度而忽略音色退化，也不能用更长参考直接宣称问题已解决。
- [ ] 后续应围绕完整参考的内容／处理与生成清晰度进行有依据的单项验证，避免继续随机重生成或未经验证地添加滤镜。P8首句及整篇音质仍未完成。

人工反馈保存于 `out/validation/voice-p8-reference-20261017/controlled/human-review.json`。本轮仅登记结论，无新生成、业务代码改动、云请求或构建。

### 2026-10-17：完整参考边缘低电平区单项对照

- [x] 只读原参考信号：两声道相关系数为1、RMS完全相同，排除本次声道平均导致相位抵消的证据。以20ms帧相对能量阈值初步定位明显语音约0.44–6.58s；阈值不能证明低电平区绝无人声，不进行贴边裁切。
- [x] 在B完整参考上仅保留0.24–6.78s，共6.54s，两侧各留约200ms余量，不删除中间内容、不降噪／调增益。与B维持同一全文转写草稿、seed12345、目标首句、模型／精度及nonStreamingMode=false，原参考、声音档案和工程保持不变。
- [x] 工具管理的独立单任务Worker离线生成成功并等待退出，无云调用；未经过应用GPU队列，不冒充完整应用任务验收。新C为 **24kHz／168,960 samples／7.04s**，耗时 **15.015s**、峰值 **0.195767**、有限、无削波。B为5.76s，时长变化表明参考调整也影响节奏，不能仅挑选清晰度改善而忽略其它变化。
- [x] B和C均以相同1秒前后静音方式展示，明确为新合成结果，不对成品做降噪、提亮或增益；未经确认不将裁切规则加入默认路径。
- [ ] 已询问C是否同时保留B音色、改善清晰度，变慢是否可接受，以及句首异响，等待人工结论。完整转写仍是草稿，不升级正式审核状态；P8仍未通过。

证据：`out/validation/voice-p8-reference-20261017/reference-signal-audit.json`、`controlled/prepare-edge.py` 与 `controlled/edge-trim/` 下的 `condition.json`、`request.json`、`events.jsonl`、`stderr.txt`、`quality.json`、`synthesis-padded.wav`。没有业务源码、默认参数或运行依赖变更，未构建／全量回归／打包。

### 2026-10-17：C参考边缘对照未通过——尾段异响

- [x] 用户反馈C在7–8秒有异响；试听副本有1秒前置静音，对应原生生成约6–7秒，仍处于7.04秒音频内部尾段。记录为未通过，不自动将其归为追加静音、播放器问题或参考边缘噪声。
- [x] 本次没有明确肯定音色、清晰度或节奏，不补记这些指标通过。C不采用，参考边缘裁切不加入默认流程，保持原声音档案、工程及选中Take。
- [ ] 同配置重生成、参考长度和边缘裁切试验均未形成同时满足音色、清晰度与无异响的稳定方案；这些结果不支持宣布Qwen首句问题修复。后续应收敛实验，优先定位异常音频本身及模型能力边界，不继续把随机候选试听当作稳定性验收。

反馈保存于 `out/validation/voice-p8-reference-20261017/controlled/edge-trim/human-review.json`。本轮仅记录反馈，无新推理、云请求或业务代码改动；P8音质仍未通过。

### 2026-10-17：P8 C尾段异常客观检查与本地ASR回读

**状态：完成本轮定位，尚无稳定修复；不再用随机候选试听代替质量验收。** 不生成新TTS、不更改声音档案／选中Take／默认参数／统一界面，无云请求。

- [x] 对B、C原生文件检查：两者无非有限采样和削波。C用户所指原生6–7秒尾段RMS **0.0164511**、峰值 **0.124562**，是有效非静音内容，不能直接删除。B末1秒RMS **0.0284767**、峰值 **0.180040**；不同音素和时间位置，不直接据此判断谁的音质更好。
- [x] Welch尾段频谱：B谱质心约718Hz、6kHz以上功率占比0.00816；C约1183Hz、占比0.02591。仅作为线索，频谱受语音内容／节奏影响，不能据此命名噪声类型或自动实施高频衰减。
- [x] 用现有 `.venv-inference` 的Qwen3-ASR本地0.6B模型按批次顺序回读两段完整原生音频，不加载Forced Aligner。网络连接在诊断进程内禁用，模型路径为本地，缓存写验证目录，无安装／下载或模型改动；工具管理进程正常退出并等待释放，不冒充应用GPU队列测试。
- [x] B、C均识别为“清晨，我推开窗户，看见阳光落在对面的屋顶上。”，没有识别出的额外台词。这里只能支持全文内容基本匹配；输出timing=chunk，没有词级时间，不伪造尾段对齐，也不能证明没有非语音异响。保留ASR generation flags等警告，不据回读结果覆盖用户试听结论。
- [ ] C继续未通过，尚无证据支持裁掉句尾、放宽损坏质检或把某种滤镜加入默认流程。用户听到的异响及音色／清晰度取舍仍按真实质量问题保留；不能宣称0.6B的绝对能力上限已测定，更不能擅自升级模型、训练或下载。

证据：`out/validation/voice-p8-tail-diagnosis-20261017/` 的 `signals.json`、`spectral-summary.json`、`asr-request.json`、`run-offline-asr.py`、`asr-result.json`、`asr-stderr.txt`。本轮仅离线分析和本地ASR，不改业务代码、不构建／完整回归／打包。P8音质仍未通过；后续应将该问题保持为有复现材料的已知限制，再推进其它已确认且不依赖该音质通过的未完成项，避免无限扩大随机试听实验。

### 2026-10-17：L2 GPT英日跨语言文本前端离线预检

**状态：独立推进GPT英日准备条件，文本资源缺失阻塞，未启用新语言能力。** 不改变三模块／统一设置、中文零样本路径、外部源码或环境，不下载资源或调用云端。

- [x] 复查现有Worker：GPT目标语言及参考语言目前都固定all_zh，应用只声明中文。即使预检通过也必须先完成独立语言映射与实际验证，不能只扩展能力列表冒充跨语言支持。
- [x] 英文上游处理器导入g2p-en 2.1.0；其代码在旧版NLTK资源ZIP缺失时会自动下载。日文上游pyopenjtalk会在词典缺失时下载，并可能在源码text/ja_userdic生成用户词典。为保护外部仓库，探针复制文本模块到忽略的验证目录（排除G2PWModel权重及pycache），禁网并阻断nltk.download；没有修改外部源码。
- [x] 实际独立环境：NLTK **3.10.3**、g2p-en **2.1.0**、pyopenjtalk **0.4.1**、wordsegment **1.3.1**、NumPy **1.26.4**、Torch **2.7.0+cu126**。本轮仅CPU文本前端预检，未加载主模型／CUDA推理。
- [x] 当前NLTK搜索路径未找到 `taggers/averaged_perceptron_tagger.zip`、`taggers/averaged_perceptron_tagger_eng`、`corpora/cmudict.zip`；分别覆盖g2p-en旧导入检查及当前英语pos_tag数据。缺少的是数据资源，不是已安装的Python包。
- [x] pyopenjtalk默认 `E:/AIModels/TTS/.venv-gpt/Lib/site-packages/pyopenjtalk/open_jtalk_dic_utf_8-1.11` 缺少sys.dic、unk.dic、char.bin、matrix.bin及相关def文件；在实际调用前阻断，未让上游下载。英日均标失败，保留明确错误及Traceback，不用文件存在冒充音素转换通过。
- [x] 较广范围文件名扫描耗时后主动中止，进程已退出；没有据未完成的扫描声称其它目录也不存在。已询问用户是否有其它资源根目录，当前等待路径或未准备答复。
- [ ] 获得资源后：完整性／许可检查、实际clean_text及v2ProPlus音素ID转换，再接参考／目标语言映射和8GB真实英日推理；这些本轮均未执行，原中文能力保持。Qwen首句质量问题仍保留，不被本轮新工作替代为通过。

证据与资源准备清单：`out/validation/voice-l2-gpt-20261017/` 下的 `probe.py`、`text-preflight.json`、`probe-output.txt`、`probe-stderr.txt`、`resources-needed.md`。没有业务代码修改、新运行依赖、构建／全量回归或打包。所需数据由用户提供或另行明确授权，不自动安装下载。

### 2026-10-17：用户补充GPT资源后实际加载复验

**状态：中文与日文前端可执行，英文资源路径仍未就绪，未进入真实语音生成。** 用户要求实际加载与完整错误汇总，本轮不安装下载、不改其它环境，应用仍只声明已验证的中文能力。

- [x] 使用指定 `E:/AIModels/TTS/.venv-gpt/Scripts/python.exe` 重新运行复制模块的离线预检。OpenJTalk 1.11现已存在并实际完成日文g2p、clean_text及v2ProPlus ID转换；此前日语词典缺失条件解除，不要求重复准备。修改用户词典的副作用限制在验证目录复制的text模块内。
- [x] 中文显式使用原有本地G2PW目录及Chinese Roberta，阻断下载函数后实际运行clean_text与ID转换，结果通过、UNK=0；未加载GPT／SoVITS主模型。日文返回有效ID，但原始音素中两处边界符#不在symbols2中，被现有cleaner转成UNK；保留该行为及风险，不擅自增加未训练符号或把文本转换通过当成音质通过。
- [x] 分别实际调用英文 `nltk.pos_tag`、`nltk.corpus.cmudict.dict()/words()`：仍分别报averaged_perceptron_tagger_eng、cmudict未找到。英文clean_text导入g2p-en时尝试下载旧ZIP，已被禁网／nltk.download阻断；未伪装为成功。默认NLTK路径及有限范围的模型资源目录未找到nltk_data，不声称用户尚未下载或全盘不存在；已询问用户提供实际根目录，只在后续验证进程指定路径。
- [x] 汇总所有检查及完整Traceback到 `frontend-verified.json` 与 `frontend-errors.md`，包括英文标注、词典、前端三个错误。`japanese-symbols.json`记录原始音素及两处#，`probe-rerun.txt`保留日文词典加载复验。原资源清单增加“日语已通过、不重复下载”的更新说明。
- [ ] 英文资源定位后重新验证全部英文步骤，再按用户要求进入真实英日合成测试；当前不提前启用多语言、不自动更改Worker语言硬编码或已有工程。中日文本通过不等于真实跨语言验收完成。

本轮仅运行复制的文本模块／本地G2PW与词典，所有诊断进程已退出；不修改外部GPT源码／权重、其它TTS环境、UI或默认参数，无云请求、构建、完整CTest或打包。证据还包括 `verify-frontends.py`、`frontend-verified-stderr.txt`。Qwen首句音质与其它原未完成项保持。

### 2026-10-17：GPT资源路径解除阻塞，三语言前端及真实生成首轮通过

**状态：实际前端与实验生成通过，等待人工音质，尚未启用应用英日能力。** 用户提供 `E:/AIModels/TTS/.venv-gpt/nltk_data`，只在验证进程设置NLTK_DATA。未安装／下载资源，不修改其它环境、模型、外部源码或统一界面。

- [x] 独立GPT环境重新执行 `verify-frontends.py`：英文pos_tag、CMU词典dict/words读取、中文G2PW、英文／日文clean_text与v2ProPlus ID转换全部实际通过。此前英文资源定位阻塞解除；日文两处#按现有cleaner映射UNK的事实保留，不修改词表假装修复。旧资源准备文档已注明不要重复下载。
- [x] 在验证目录复制Worker／Adapter，对实验副本增加目标Chinese→all_zh、English→en、Japanese→all_ja及独立referenceLanguage映射。插入复制text模块路径，保护外部日文用户词典／英文缓存；实验adapterVersion=3和languageProbe计入模型身份，不伪用旧版本。没有更改业务Worker或应用能力声明。
- [x] 使用已批准的3秒中文女声参考及转写、seed12345、现有s1v3／v2ProPlus基础权重，独立单Worker顺序完成中英日三句。进程正常退出，禁网、无并发TTS；这是工具管理的独立诊断，不冒充应用GPU队列／项目闭环测试。
- [x] 实际加载版本v2ProPlus、semanticTokenCount=1025；G2PW实际CPUExecutionProvider与预检一致，主模型使用RTX3060Ti CUDA。最终任务Torch allocated峰值 **3,016,419,328 bytes**、reserved峰值 **3,074,424,832 bytes**，并非整卡显存；短句8GB运行成功不等于所有长文通过。
- [x] 中文“今天的天气很好。” **32kHz／60,160 samples／1.88s／生成17.546s／峰值0.088684**；英文“The weather is nice today.” **32kHz／57,600／1.80s／4.530s／峰值0.126099**；日文“今日はいい天気です。” **32kHz／56,320／1.76s／1.563s／峰值0.111267**。三者单声道、有限、无削波；SoundFile完整读取与独立PyAV/FFmpeg逐包解码通过、corruptPackets=0、样本数一致。生成耗时不包含全部预检和冷加载，首句有预热影响，不直接比较语言速度。
- [x] 原生WAV仅复制为Chinese／English／Japanese可识别文件名后展示，未调音量、降噪或修改原件，真实原生音量偏低保留。
- [ ] 已分别请求中英日读音、漏字／重复、清晰度、杂音和音色相似度人工评价；humanAccepted仍false。前处理及可解码性通过不能代替语言／音质验收；应用英日接入、原环境路径配置及回归测试须在正式实现阶段处理，不能以诊断副本冒充产品支持。

证据：`out/validation/voice-l2-gpt-20261017/` 中的 `frontend-verified.json`（本轮全通过）、`frontend-final-*`、`prepare-inference.py`、`probe-worker/`、`model-check.jsonl`、`generation-request.json`、`generation-events.jsonl`、`generation-stderr.txt`、`real-generation-audit.json`、`ffmpeg-decode.json` 和 `real-infer/Chinese.wav`、`English.wav`、`Japanese.wav`。诊断请求使用独立JSON哈希，不是项目正式输入哈希。没有业务源码变更、构建／完整CTest或打包，未修改已知失败的Qwen首句或其它阶段状态。

### 2026-10-17：GPT三语言人工音质全部未通过——声音有气无力

- [x] 用户明确反馈“三种都不通过，理由是全都是有气无力的声音”。本次中文、英文、日文分别标为音质未通过；此前本轮前端及解码执行通过的事实保留，但不得视为可用音质或L2交付通过。
- [x] 将问题记录为发声状态／表现力不足的人工反馈，不自行归结为低峰值、增益或根因已确定。不能通过提高音量、修改评判标准或上调标签将其伪装为解决；不自动重抽候选继续消耗试听时间。
- [x] 应用GPT英日能力仍不启用；已有中文功能及用户工程不删除、不回写，旧的短句暂接受记录仅保留其当时语料和条件范围，不覆盖本次明确失败结论。
- [ ] 后续若继续GPT音质专项，需要有依据地区分参考本身的发声状态、克隆条件和实际权重能力；未验证前不承诺调参可解决，不自动训练／换模型／下载。P8、L2及其它未完成阶段不因此勾选完成。

人工反馈保存于 `out/validation/voice-l2-gpt-20261017/human-review.json`。本轮仅登记结论，没有新推理、增益处理、业务源码／环境变更或构建。

### 2026-10-17：GPT 可选英日资源检查接入正式离线预检

**状态：资源诊断完善并通过回归；GPT 三语音质未通过的结论不变。** 本轮不重复生成、不修改默认合成参数、统一界面、已有工程或外部模型环境。

- [x] 扩展现有 `tools/tts_engine_preflight.py`，集中报告 NLTK 两个 ZIP 的必要成员、英文 tagger 的三份 JSON、OpenJTalk 1.11 的八项词典文件。搜索进程环境及当前 Python 环境路径；只读检查，不导入可能下载或生成词典缓存的文本模块，不解包或执行 pickle。
- [x] 新增可选 `languageResources` 报告，明确 `filesPresent` 与 `runtimeVerified=false / qualityVerified=false`；缺失英日资源不阻塞现有中文生成就绪状态，不扩大应用的 GPT 语言能力，也不改变模型身份或生成输入哈希。
- [x] 使用 `E:/AIModels/TTS/.venv-gpt/Scripts/python.exe -B tools/tts_engine_preflight.py --engine gpt --repository E:/AIModels/TTS/models/GPT-SoVITS` 实际检查：中文必要文件／包齐备、generationReady=true，英日可选资源均齐备。此处是静态资源检查，实际前端和生成证据仍引用前一阶段，不能据此推翻人工失败评价。
- [x] `E:/AIModels/TTS/.venv-gpt/Scripts/python.exe -B tests/python/test_tts_engine_preflight.py`：8 项通过，包括缺失项汇总、无缓存写入和损坏 ZIP 识别。
- [x] `cmd /c 'set VSLANG=2052&& tools\build-windows.bat cuda'`：CUDA 构建成功，完整 CTest **35/35 通过，105.38 秒**，含打包及两个启动测试；默认测试未执行需单独授权的真实云请求。
- [x] staging 中预检脚本 SHA-256 与源码一致。CUDA 包 **5,101,598,802 bytes（约 4.751 GiB）**，低于 4.8 GiB；Torch **3,808,916,676 bytes**，cuBLAS **1,080,248,544 bytes**（位于 Torch 内容中，不能相加统计）。未构建 CPU 包。按 `tools/clean-workspace.ps1 -Only out/build/windows-release-cuda/package` 预览后 `-Apply` 清除 staging，保留验证证据与当前构建。
- [ ] GPT 声音有气无力、Qwen 长文音色／异响、P7 完整真实云端项目验收及其余原未完成项仍保留，不将资源齐备或自动测试通过标为音质通过。本轮无新模型推理、安装、下载或云端合成。

证据：`out/validation/voice-gpt-resource-preflight-20261017/` 中 `tests.txt`、`installed-preflight.json`、`build-ctest.txt`、`package-size.json`、`cleanup-preview.txt`、`cleanup-result.txt`。本阶段完成的是既有离线预检补强，不代表 L2 或 P8 整体完成。

### 2026-10-17：L3 批量样本情绪建议接入，真实保存链路通过

**状态：补齐样本整理窗口的手动批量识别和待审核保存；情绪准确性仍未通过，L3 不整体勾选完成。** 不重复生成 TTS、不调用云端、不安装或修改模型／独立环境，不改变用户工程、人工标签、默认声音或项目设置。

- [x] 复用 `VoiceLibraryController::recognizeEmotion`、已有单 Worker 和 InferenceManager GPU 队列，增加勾选样本批次。一次接受 1–100 个尚未保存的有效样本，跳过未勾选、错误和已保存项；声音档案不匹配时拒绝执行。
- [x] 整批检查任务数量、顺序、样本 ID、音频 SHA、预测格式和未审核状态后才回写；取消、超时、截断输出或失败不覆盖原有预测。沿用任务所有权、等待退出和 epoch 通知隔离，保留临时素材直到任务退出。
- [x] 保存样本时将预测写入不可变待审核版本，重新打开后仍要求试听审核。手工转写保留，不生成或覆盖人工情绪。实验开关默认关闭；配置及识别按钮留在现有样本整理窗口，小窗口可纵向滚动。
- [x] 新增取消／销毁回归，以及显式 `SUBCUE_L3_BATCH_OUTPUT` 的真实批次验收入口。用已有中英日音频整理样本，中文取消勾选，英日实际识别、保存、重新读取通过；未勾选项完全不变。运行前后模型文件数、总大小和四项关键 SHA 一致，结束时 GPU 队列为空。
- [x] 本轮两段均被模型高分判为 fearful，仅保存为未审核建议，`humanAccuracyAccepted=false`；不将流程成功或高 softmax 分数当成正确情绪。既有 L2／P8 音质失败结论保持。
- [x] 含真实批次和界面的定向测试 **6 passed／0 failed／0 skipped，74.172s**；滚动适配后定向测试 **5 passed／0 failed／0 skipped，21.236s**。批量窗口实际检查 900×560、1366×768、1920×1080，DPR 1.75；最初请求 900×440 时被应用最小高度约束到 560，最终测试已明确改为实际下限，不冒称支持更低窗口。
- [x] CUDA 构建成功。首轮全量 CTest **35/35／102.18s**；最终源码全量 **33/35／97.55s**，失败目标为 EditorIntegration 和 CloudConfig，不能标为最终全绿。显式诊断中 CloudConfig 的 nativeCredentialNamespace 写入失败，集成测试失败集中在建立云配置／凭据阶段；独立随机命名的假凭据写入也返回 **CredWriteW=false、Win32 error=8**，没有读取或删除用户凭据。最终集成诊断 **56 passed／14 failed／8 skipped**，本轮情绪审核与批量取消／销毁用例通过；尚未确认凭据资源失败的更底层原因，不凭此前通过数字覆盖本轮失败。
- [x] 最终打包及两项启动测试通过。CUDA staging **5,101,608,530 bytes／4.75124 GiB**；Torch 目录及其 dist-info 共 **3,810,570,269 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（包含于 Torch，不能相加）。无新增依赖，未构建 CPU 包或覆盖 dist；staging 已按清理脚本先预览再清除。

证据：`out/validation/voice-l3-batch-20261017/` 的 `final-targeted.txt`、`responsive-tests.txt`、`build-ctest.txt`、`final-build-ctest.txt`、`integration-diagnostics.txt`、`cloud-diagnostics.txt`、`credential-probe.txt`、`model-before.json`／`model-after.json`、三尺寸截图、包体及清理日志；`real-batch/voices.sqlite`、资产目录与 `batch-review.json` 保留真实批次及未审核状态。

**后续**：先恢复 Windows 测试凭据可写状态并重跑失败目标，再取得最终完整回归；L3 天然带标签语料／阈值校准、GPU kernel 执行中的取消压力及准确性仍未完成。Qwen／GPT 音质、P7 在线实测、P8 最终发布和 L4 等原未完成项不变，本轮不宣称整个 AI 配音方案完成。

### 2026-10-17：修复测试凭据泄漏，完整回归恢复并补验 CUDA 运算期间取消

**状态：上轮 33/35 的凭据写入阻断已解决；共享 Worker 的真实 CUDA 取消及后续运算通过。** 没有修改业务控制器／界面、用户项目、正式账号或模型；无新 TTS 合成、付费请求、安装、下载或环境设置变更。

- [x] 只读统计发现 **546 条** `SubCue/Test/*` 隔离测试凭据。检查测试生命周期确认：Qt 临时目录删除不会删除 Windows 凭据，集成与控制器测试缺少退出清理。按严格测试目标规则预览后移除这 546 条遗留假凭据，写入测试随即恢复；不将 Win32 error=8 描述为已测定的 Windows 固定配额。
- [x] `EditorIntegrationTests`／`AppControllerTests` 登记本用例创建的 credentials.dat 路径；Qt Test cleanup 在控制器／后台任务退出后，只枚举和删除这些路径对应的隔离云端命名空间，并检查删除结果。断言失败也经过 cleanup，防止反复运行积累资源；生产 CredentialStore 生命周期不变。
- [x] 新增清理范围用例：释放当前临时路径的测试凭据，同时验证另一独立路径的假凭据仍存在；该对照凭据由作用域退出保护释放。原生凭据用例保存失败时带出实际错误说明，不再只给空断言。
- [x] `tools/clean-workspace.ps1 -TestCredentials` 提供预览，追加 `-Apply` 才执行。复用现有凭据清理脚本，但测试模式只匹配 `SubCue/Test/<16位小写哈希>/SubCue/(Cloud|ASR|AIReview|AI|VoiceStudio)/`，拒绝与其它清理目标混用，并在 SubCue 或测试进程运行时停止。未读取／输出密钥内容，正式账号及其它应用命名空间不在匹配范围。
- [x] 原先受阻的 AppController／EditorIntegration／CloudConfig 定向回归 **3/3／69.88s**，结束后测试凭据为 **0**。加入范围保护用例后的 CUDA 构建及完整 CTest **35/35／101.21s**；最终增加 CUDA opt-in 入口后再跑完整 CTest **35/35／107.68s**，结束后仍为 **0**，包括打包、启动与媒体生命周期回归。
- [x] 新增显式 `SUBCUE_CUDA_CANCEL_PYTHON` 的真实 CUDA Worker 验收，普通回归跳过此 GPU 专项。受管理 Python 只创建两个固定 8192×8192 矩阵并按固定批次运算；CUDA Event 报告 `pending=true` 后取消，确认真实 Python 进程句柄已退出、GPU 队列为空，随后新 Worker 的 CUDA 求和返回 4.0。使用已有情绪环境实际执行：**3 passed／0 failed／0 skipped，4.308s**；事件时 Torch allocated **545,390,592 bytes**，不是整卡或模型峰值。
- [x] 该专项证明共享进程组在真实 CUDA 工作期间的取消／等待和后续可用性；没有加载 emotion2vec 或 TTS 权重，不冒充某个模型全部 kernel 阶段／长时间压力、控制器所有销毁路径或情绪准确性通过。
- [x] CUDA staging **5,101,608,530 bytes／4.75124 GiB**；Torch 目录和 dist-info **3,810,570,269 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（包含于 Torch）。未新增依赖、构建 CPU 包或覆盖 dist；package 按预览清理，保留当前 CUDA 构建及证据。

证据：`out/validation/voice-credentials-20261017/` 内的测试凭据清理预览／结果、`after-targeted-preview.txt`、`after-full-preview.txt`、`final-credentials-preview.txt`、构建日志、`targeted-ctest.txt`、`build-ctest.txt`、`cuda-cancel.txt`、`final-ctest.txt`、包体与 package 清理记录。

**剩余门槛**：L2／P8 的 Qwen 音色、清晰度和异响，GPT 发声状态，L3 天然带标签语料及准确率／阈值校准仍未解决；模型具体推理阶段的完整取消压力仍需覆盖。P7 新在线付费批次、L4 上传／留存／费用方案及最终发布继续保留原范围与授权边界，不以本轮基础设施通过替代音质和真实云端验收。

### 2026-10-17：L4 云端音色创建首版接入，保留单次提交与未知结果记录

**状态：已实现用户自备 HTTPS 参考链接的创建入口，真实收费与克隆音质尚未验收，L4 不整体勾选完成。** 复用现有北京 Flash 语音合成用途绑定、账号、Workspace、受管理 HTTP 任务和声音库，不新增 Provider、依赖、自动上传或模型下载。

- [x] 声音库新增创建预览；填写参考链接、音色前缀、人工预计价格／来源／日期，展示账号、型号、Workspace 和参考域名。使用权、服务商读取与留存政策、实际费用三项分别确认后才允许发送一次创建请求。人工价格不是服务商报价、免费承诺或费用硬上限；完整设计见 `docs/CloudVoiceEnrollment.md`。
- [x] 根据官方接口使用 `voice-enrollment/create_voice`，目标固定为已接入的 `qwen-audio-3.1-tts-flash`；中文参考、最多取 10 秒，关闭预处理和音量归一化。服务商仍可读取整个托管文件，应用不下载或质检链接，素材托管、撤下和远端留存政策由用户核对。
- [x] 发出请求前以 QSaveFile 保存结果不明记录，并持有实例间锁；配置修订或前次记录变化则拒绝旧预览。取消、异常、响应丢失及不可信 Voice ID 不自动重试；重启只读取本地记录。人工核对后可重新准备，但不把未知结果标成创建失败、退款或已删除。
- [x] 持久记录排除完整参考 URL、签名参数和 API Key，保留 URL 域名／哈希、操作 UUID、绑定、人工费用快照、确认时间及可信返回信息。URL 哈希不是音频内容指纹。上次有效记录先归档到历史目录；通用存储清理保护当前与历史记录。损坏记录保持阻止创建，需核对远端并恢复有效备份。
- [x] 获得 Voice ID 后须手动查询；沿用现有查询链路，只在目标型号匹配且状态 OK 后关联版本，不自动生成台词或替换工程音频。
- [x] 模拟 HTTP 覆盖参数与三项确认、锁竞争、单次 POST、日志脱敏、重启不重发、DEPLOYING／OK 查询、取消与人工核对、配置变更、控制器销毁等待及无延迟通知、HTTP 401。所有测试使用隔离假凭据，无真实云端请求。
- [x] 界面验证三项确认默认关闭，并填写真实 QML 表单验证预览／确认按钮；仅检查启用状态，不点击付费提交。900×560、1366×768、1920×1080 的原生截图检查通过，DPR 1.75。截图循环恢复原窗口尺寸，修复其对后续情绪滚动断言的状态污染；未放宽旧断言或更改正式界面尺寸。
- [x] 最终离屏定向测试 **4 passed／0 failed／0 skipped，7.542s**；原生定向测试 **4 passed／0 failed／0 skipped，25.160s**。首轮完整构建成功、CTest 34/35 的唯一布局测试失败已定位并修正，最终全量结果另记于下方。
- [x] CUDA 构建成功；窗口测试隔离修正后的最终完整 CTest **35/35 通过，103.81s**，包含打包、两个启动测试与原有媒体生命周期回归。测试退出后按严格测试命名空间预览，凭据残留 **0**。
- [x] CUDA staging **5,101,671,506 bytes／4.75130 GiB**，低于 4.8 GiB；Torch 目录及 dist-info **3,810,570,269 bytes／74.69%**，全部 cuBLAS DLL **1,080,248,544 bytes／21.17%**（与 Torch 统计存在重叠，不能相加）。保留 CUDA 13 的 cuBLAS／cuBLASLt／cudart；未构建 CPU 包或覆盖 dist。staging 按清理脚本先预览再清除，保留当前构建与验证证据。

证据：`out/validation/voice-l4-enrollment-20261017/` 中的构建与回归日志、`ui-diagnostics.txt`、`final-offscreen.txt`、`final-native.txt`、三尺寸截图及 `package-size.json`。诊断审批曾因审核服务额度暂时失败，未执行或绕过；后续按同一审批流程重试成功。

**剩余**：L4 的本地素材自动托管、远端更新／删除、真实账号创建／收费／留存核对和人工克隆音质尚未完成；现有 HTTPS 入口默认不提交。P7 在线项目验收、L2／P8 已知音质问题、L3 准确性与最终发布门槛保持，不用模拟链路通过替代真实质量结论。本轮没有新推理、付费请求、环境或编辑器设置变更。

### 2026-10-17：按用户要求移除云端 Token／金额预测，仅保留实际用量换算

- [x] 删除 `TtsPricing::estimate`、基于历史样本的费用预测、剩余费用预测及相关待发送记录字段。移除无历史样本时的 1～3 句校准流程，手动确认后执行所选批次，不再要求预测或校准数据。
- [x] 云端生成确认窗口仅列出发送文本、Voice ID 与批次／复用数量，移除预计金额、样本可信度和校准数量。生成中显示服务商实际输入／输出 Token、本句金额及本批已知金额；任务历史保留实际用量、价格快照和人民币换算，不删除旧工程账本。
- [x] 保留已有软预算，以实际已知费用达到预算作为后续请求的暂停条件；不推算下一句费用。单次请求可能超出预算，界面已明确说明。实际用量未知仍不按零计算，并暂停后续请求；取消、下载失败及不明结果均不自动重发付费请求。
- [x] 实际计价与付费请求保护定向测试 **3 passed／0 failed／0 skipped，0.048s**；云端用量、下载后提交、连接检查及原生 QML 定向测试 **6 passed／0 failed／0 skipped，29.785s**。测试确认无预测字段、无历史样本可直接执行两句批次、实际 Token／金额落库，以及费用达到软预算后停止下一句。界面检查 900×560 与 1366×768；只打开确认窗口并取消，不发送真实请求。
- [x] 标准 `tools/build-windows.bat cuda` 构建成功，完整 CTest **35/35 通过，107.02s**，含打包和两个启动测试；测试凭据残留 **0**。CUDA staging **5,101,653,586 bytes／4.75129 GiB**，Torch 目录及 dist-info **3,810,570,269 bytes／74.69%**，全部 cuBLAS DLL **1,080,248,544 bytes／21.17%**（统计有重叠，不能相加）。staging 按脚本预览后清除，保留当前构建与证据，未构建 CPU 包或覆盖 dist。

证据：`out/validation/voice-actual-usage-20261017/` 内的构建日志、`pricing-tests.txt`、`targeted.txt` 和两尺寸确认窗口截图；使用隔离假凭据及模拟 HTTP，本轮无真实云端请求、模型推理、下载或设置修改。此前云端音色创建的人工单次价格快照不是 Token 预测，本轮未改动其独立创建流程。原有音质与发布验收门槛保持。

### 2026-10-17：L4 云端自定义音色删除入口接入

**状态：删除功能与模拟链路接入，未执行真实云端删除，L4 尚未整体验收。** 延续统一设置与现有 HTTP 所有者，不恢复 Token／金额预测，不新增依赖或模型下载。

- [x] 声音库“通义云音色”页增加删除入口，仅接受当前北京 Flash 型号的自定义 Voice ID，拒绝系统音色和其它型号。预览只读本地配置；明确展示账号、型号、Workspace 和目标 ID，需再次完整输入 ID 并确认使用权与删除影响。
- [x] 确认后先原子保存操作记录，再查询目标详情。型号必须一致，状态为 OK／DEPLOYING／UNDEPLOYED，才发送一次 `delete_voice`。查询失败或取消记录 `delete_blocked`／`delete_not_sent`，允许重新预览；删除响应丢失或不完整则保持 `unknown`，不得自动重试。
- [x] 仅将符合官方成功结构的响应记为 `deleted`／`delete_acknowledged`。这表示服务商确认删除音色，不代表托管源文件、服务商其它素材副本、训练数据或账单已删除。接口数量仅记录为音色数量，不推算 Token 或金额。
- [x] 与创建共用受保护的当前记录和历史归档，实例间锁、前次记录哈希、配置修订检查均生效；重启只读记录，不恢复删除任务。删除前后保留本地不可变版本、工程快照与已有音频，当前账号目录失效后需重新刷新。删除会影响其它工程／应用使用同一 Voice ID 的后续合成，界面明确要求先停止相关生成。
- [x] 新增模拟回归：缺少确认／ID 不符、非法与系统 ID、锁竞争、型号不符、单次删除、响应完整性、记录脱敏、重启不重发、双实例陈旧预览、凭据修订、查询中取消、删除中取消／销毁、未知结果人工核对、HTTP 401，以及本地声音库／项目快照不变。取消、销毁后请求退出且锁释放，无延迟完成通知。
- [x] 原生定向测试 **5 passed／0 failed／0 skipped，33.460s**，包含新删除、原创建及 QML 整体流程。900×560、1366×768、1920×1080 三尺寸检查通过，DPR 1.75；UI 仅验证按钮启用并关闭预览，未点击真实删除。
- [x] 标准 CUDA 构建成功，完整 CTest **35/35 通过，105.35s**，含打包及两个启动测试；测试凭据残留 **0**。CUDA staging **5,101,699,154 bytes／4.75133 GiB**，低于 4.8 GiB；Torch 目录及 dist-info **3,810,570,269 bytes／74.69%**，全部 cuBLAS DLL **1,080,248,544 bytes／21.17%**（统计有重叠，不能相加）。staging 经脚本预览后清除，保留当前构建与证据，未构建 CPU 包或覆盖 dist。

协议依据及使用边界更新到 `docs/CloudVoiceEnrollment.md`；官方 HTTP API 已复核，成功响应须含请求 ID、空 output 和数量 1。证据位于 `out/validation/voice-l4-delete-20261017/` 的构建、定向测试日志和三尺寸截图。

**剩余**：远端更新涉及同一 Voice ID 内容变化与已有输入／缓存身份保护，尚未实现；自动托管、真实账号权限／收费／删除验收、留存政策核对和克隆音质仍保留。P7 在线项目验收、L2／P8 已知音质问题、L3 准确性与最终发布未因此通过。本轮无真实云请求、推理、安装、下载或设置修改。

### 2026-10-17：L4 远端音色更新与旧候选身份保护接入

**状态：更新功能与本地修订保护已实现，真实云端更新与克隆音质尚未验收。** 不恢复 Token／金额预测，不改已有工程数据、默认参数、编辑器或项目设置，不新增依赖、模型下载或训练。

- [x] 声音库增加更新入口，复用云端音色操作窗口、已绑定北京 Flash 账号与 Workspace。填写自定义 Voice ID、新 HTTPS 参考链接，预览不联网；再次输入目标 ID，分别确认使用权与替换影响、服务商读取／留存政策及本次费用后才执行。完整链接仅随本次请求发送，持久记录排除签名参数和 API Key，保留域名与 URL 哈希（不是音频指纹）。
- [x] 将更新与删除的提交／查询／单次请求／落盘逻辑合并到已有控制器的私有方法，保留删除原行为。更新先取得应用云配置变更门禁，取消并等待本账号云任务，保留其它账号已运行任务；其它实例或已有变更阻止取得门禁时记为未发送，可重新预览。查询型号与状态通过后发送一次 `update_voice`；接口确认不等于审核完成或音质通过。
- [x] 按账号、型号、地域、Workspace、Voice ID 保存本地修订 UUID；发送更新前先持久化并阻止旧身份生成，响应不明时保持阻止。人工控制台核对后才允许重新查询；成功后也必须查询 OK 并显式选用新的不可变声音版本。修订目录 `voice-revisions/` 受通用清理保护，单个记录最多读取 16 KiB，损坏时明确阻止生成，应随配置备份。
- [x] 未改 TTS 输入格式或工程 Schema。新声音版本 ID 进入现有指纹；声音库去重增加修订条件。生成准备、确认旧预览（含全部复用的情况）、冻结任务实际派发都检查当前修订，拒绝用旧版本生成或误复用旧结果。已有声音版本、候选、项目快照及实际 Token／金额记录保持不变；旧音频仍可试听。
- [x] 更新任务由原 HTTP 线程池所有者管理，取消／销毁先等待退出，再释放应用变更门禁；晚到通知按 epoch 丢弃。普通配置修改不能在更新期间切走凭据。更新请求、结果不明和进程重启不自动重发。
- [x] 新模拟回归覆盖三项确认、目标 ID、非法 URL、门禁竞争、同账号取消／其它账号保留、单次更新、记录脱敏、旧生成预览与冻结任务拦截、重新查询后新版本与新输入指纹、旧候选不复用、实际金额保留、更新中取消／销毁、未知结果人工核对、型号不符、响应不完整、修订损坏及跨账号隔离。首轮新测试在重新绑定时使用了不支持的 all 范围，已改为现有 selected 接口并增加实际版本断言，未放宽业务校验。
- [x] 最终原生定向测试 **7 passed／0 failed／0 skipped，40.864s**，包含更新、原创建／删除、实际计费与 QML；900×560、1366×768、1920×1080 更新窗口检查通过，DPR 1.75。UI 仅验证确认启用并撤回预览，测试 HTTP 均为隔离模拟，不发起真实云请求。
- [x] 标准 CUDA 构建成功，完整 CTest **35/35 通过，114.95s**，含打包及两个启动测试；测试凭据残留 **0**。CUDA staging **5,101,730,386 bytes／4.75136 GiB**，低于 4.8 GiB；Torch 目录及 dist-info **3,810,570,269 bytes／74.69%**，全部 cuBLAS DLL **1,080,248,544 bytes／21.17%**（统计有重叠，不能相加）。staging 按脚本预览后清除，保留当前构建与验证证据，未构建 CPU 包或覆盖 dist。

证据：`out/validation/voice-l4-update-20261017/` 中构建日志、首轮及 `final-targeted.txt`、三尺寸更新窗口截图。官方更新协议已复核，使用边界与修订备份说明见 `docs/CloudVoiceEnrollment.md`。

**剩余**：L4 代码缺口继续保留本地素材自动托管；创建／更新／删除的真实权限、收费、审核、留存政策与克隆音质未验收。本地修订只覆盖本机配置记录的更新，不承诺自动检测控制台、其它应用或其它机器的变更。P7 完整在线项目、L2／P8 已知音质问题、L3 准确性、模型具体阶段完整取消压力和最终发布仍未通过。


### 2026-10-17：暂停云端托管，优先完善本地服务——Qwen 异常停止与资源释放

**方向调整**：用户明确“先不用云端托管，先把本地服务做好”。L4 本地素材自动托管与百炼临时空间兼容实验暂停；本轮未上传素材、读取云端凭据或发送云端请求。继续以现有本地工作区、统一设置和共享 GPU 调度为基线。

- [x] 检查三个本地 Worker 的实际适配及已安装 GPT 源码。确认 Qwen 原先在真实推理异常后继续下一句，而 Index／GPT 已停止异常批次；Qwen 的模型加载和逐句异常路径也缺少统一 finally 清理。
- [x] Qwen 真实异常后停止本批次，保留已经发布的句级结果与完整 Traceback；Mock 故障后继续的既有测试行为保留。逐句 finally 解除 writer、输入参考及输出波形引用，批次 finally 清空克隆条件缓存并释放模型，再执行 GC 与 CUDA 缓存释放；加载异常同样进入清理。
- [x] 在真实 Qwen 适配分支注入轻量依赖，覆盖加载、参考处理、推理 OOM、输出写入失败及正常完成。用弱引用检查清理 CUDA 缓存前模型／参考条件／波形已无强引用，核对后续句未启动、已发布文件内容保留且无 .part 残留。此项是故障与资源生命周期测试，不是实际模型或音质验收。
- [x] 独立 TTS Python 下 `-B -m unittest discover -s tests/python -v`：25/25 通过，2.269s；其中 Worker 11 项通过。首次沙箱运行因 Windows 临时目录权限失败，随后以正常进程权限重跑通过，没有修改环境、依赖或模型。

**音质边界**：只读检查未发现足以解释 GPT“有气无力”的确定参数错误；未修改默认采样、增益、滤镜、参考转写或语言能力，也未随机重生成。Qwen 清晰度／异响与日文、Index 清晰度／尾噪、GPT 发声状态、L3 天然语料准确率及 P8 真实长文人工验收仍未通过。本轮不以基础设施测试替代这些质量门槛。

- [x] `VSLANG=2052` 下 `tools\build-windows.bat cuda` 构建通过；完整 CTest **35/35，112.70s**，包含打包及两个启动测试。构建附带 Worker 与源码 SHA-256 一致。没有运行新一轮真实模型生成或模型阶段取消专项，不沿用历史结果冒充本轮执行。
- [x] CUDA 包 **5,101,730,972 bytes，4.751357GiB**，Torch 及其 dist-info **3,810,623,753 bytes（74.69%）**，cuBLAS **1,080,248,544 bytes（21.17%，与 Torch 重叠）**，未新增依赖或构建 CPU 包。完整测试退出后测试凭据残留 **0**；打包暂存经预览后按清理脚本移除，保留当前 CUDA 构建。

证据：`out/validation/voice-local-reliability-20261017/` 中 `python-worker.txt`、`python-all.txt`、`build-ctest.txt`、`worker-hashes.json`、`package-size.json`、`cleanup-preview.txt`、`cleanup-applied.txt` 和 `test-credentials-preview.txt`。


### 2026-10-17：移出云端存储计划，修复 GPT 隐式加速并验收真实阶段取消

**范围**：按用户最新要求，云端存储、本地素材自动托管及百炼临时空间实验暂时移出当前执行计划及交付门槛；已在本文件与计划书顶部、云端音色说明中明确覆盖历史托管待办。既有云端合成、音色 CRUD、实际 Token／金额记录保留，云端在线验收未自动开启。

- [x] 新增显式运行的 `inferenceCancellationStopsRealTtsStagesWhenRequested`：受 InferenceManager 进程组与共享 GPU 门禁管理，在实际 GPT CNHuBERT／语义嵌入模块完成首次前向后通知取消，检查进程句柄已退出、没有发布结果和 .part／WAV 残留、队列释放；随后使用未注入钩子的生产 Worker 在同一门禁重新加载并生成参考原文。普通 CTest 不自动运行此 GPU 专项。
- [x] 首轮参考编码取消通过，但语义钩子未触发且音频意外完成，专项失败 **2 passed／1 failed，122.494s**。检查实际安装的 `TTS.py` 与 `tools/acceleration.py`，确认此前预检 profile 声明 acceleration=false，实际却省略 use_cuda_graph/use_flash_attention，上游默认 true，并在历史真实日志和本轮日志中实际启用 torch_static_cuda_graph。不是以放宽断言隐藏问题，也不据此认定音质根因。
- [x] 生产 GPT 请求显式传入 `use_cuda_graph=false` 与 `use_flash_attention=false`；若运行后仍持有 t2s_accel 实例则在发布文件前报错。结果诊断增加实际 accelerationActive，GPT 适配版本升为 3，关闭参数进入模型身份，避免旧预检／旧候选在修复后误复用。Index 仍保留 adapterVersion=2，未改变其推理设置。原工程、候选和模型原件不修改。
- [x] Python 回归 **26/26，2.006s**，新增显式关闭参数、预检 profile、意外启用加速拒绝提交的测试。
- [x] 修复后同一真实专项 **3 passed／0 failed／0 skipped，52.514s**。CNHuBERT 触发时 CUDA 已分配 **1,345,574,400 bytes**，语义 TokenEmbedding **1,350,293,504 bytes**；两次通知均观察到 pending CUDA event=true，进程分别退出，无候选提交，队列恢复。此项覆盖上述两个 GPT 模块阶段，不宣称其它引擎、每个阶段或控制器真实销毁均已专项验收。
- [x] 两次取消后通过同一门禁实际完成中文原文复述“感觉耗电耗得更快了”：32kHz／70,400 样本／2.2s，seed=12345，非 Mock、可完整解码且非静音；实际 v2ProPlus、semanticTokenCount=1025、accelerationActive=false，生成耗时 5,750ms，Torch 峰值 allocated=1,471,217,152／reserved=1,881,145,344 bytes，不作为整机显存或与旧样本等条件的性能对比。
- [x] GPT 模型根目录运行前后 **342 个文件／2,254,044,377 bytes**，逐文件 SHA-256／大小／清单全部一致；TTS 模型不在 ASR 的 models-manifest.json 覆盖范围，故独立记录本地全量清单。无模型／依赖下载、训练、外部环境及编辑器／项目设置变更，无云端请求。

**音质状态**：新复述音频已提供参考原声对照，用户反馈“有所改善，仍需调整”，随后明确“发音过快了”，人工仍未通过，humanAccepted=false；旧三语言“有气无力”失败记录保留。此次同时修复了隐式加速路径，不把它当作严格仅换文本的音质因果对照，不承诺已解决发声状态。Qwen／Index 已知清晰度、异响与跨语言，L3 天然语料准确率及 P8 真实长文最终验收仍未通过；云端存储不再计入剩余内容。

证据：`out/validation/voice-local-gpt-service-20261017/`。隐式加速修复后的 CUDA 构建与完整 CTest 35/35／110.54s 通过；此后用户补充语速反馈，下节语速新增内容需另行最终构建和全量回归，不能沿用本条结果。


### 2026-10-17：按“发音过快”反馈接入 GPT 每句原生语速

- [x] 用户在“有所改善，仍需调整”之后明确“发音过快了”。保留该结论为未通过，不调整全局默认值或使用增益／后期拉伸掩盖问题。
- [x] 对照已安装 GPT `TTS.run` 与 v2ProPlus decode 的 speed_factor 实现，接入每句 `speedFactor`，默认沿用 1，范围 0.5～2，小于 1 更慢。仅 GPT 声明支持；其它本地引擎和云端继续拒绝该参数，不静默忽略。
- [x] 复用参数 JSON 和现有指纹，不改 Schema；语速参与不可变输入及候选失效。控制器支持恢复原速时移除字段，QML 朗读检查器新增语速输入；保留当前布局，支持非法值原子拒绝、输入焦点保护、草稿备份／恢复、撤销／重做及批量参数现有能力校验。编辑值需重新生成才生效，旧音频保持。
- [x] Python **27/27／1.745s**；文稿定向 **3 passed／2ms**，覆盖范围、类型、序列化与其它句隔离、输入指纹变化。原生 QML 首轮 **3 passed／39.258s**，包括非法语速不提交、0.85 提交、草稿恢复、撤销／重做。首轮 900 宽截图因 resize 与开抽屉时序导致抽屉收起，已仅修正测试等待，第二轮最终原生 QML 3 passed／38.788s，900 与 1366 两尺寸截图已检查，语速控件与说明可见；未改变产品窗口尺寸或布局规则。
- [x] 通过现有应用声音档案／文稿／TtsTaskController 实际生成同参考、同原文、同 seed=12345 的 0.85 候选：**32kHz／83,200 样本／2.60s**，原速对照 **70,400 样本／2.20s**。单次原生生成 5,047ms；同一 v2ProPlus 和适配版本、参考 SHA、中文输入，accelerationActive=false。原件字节复制为 `gpt-speed-085.wav` 供试听，不做后期处理。
- [x] 实际应用专项 `voiceRealExternalEngineWhenRequested` **3 passed／48.886s**：候选入库、选择、手工编排、错误引擎拒绝、再生成取消、历史和快照保留、临时目录清理及工程保存均通过；验证工程独立于用户工程。之后再次核对 GPT **342 文件／2,254,044,377 bytes** 全量 SHA 清单不变。

**人工状态**：用户明确确认 0.85 对照“语速合适”，本样本节奏验收通过；音色、清晰度及整体音质仍保留“有所改善，仍需调整”的结论，不把语速通过扩大为整体通过。原有 Qwen／Index／L3 和最终发布未完成项继续保留；云端存储仍在当前计划之外。

证据仍在 `out/validation/voice-local-gpt-service-20261017/`，新增 `python-speed.txt`、`speed-document.txt`、`real-speed.txt`、`slower-project/audit.json`、`speed-review.json`、`gpt-speed-085.wav`、`prepare-after-speed.txt`；最终原生界面 3 passed／38.788s，900 和 1366 窗口截图已检查并归档为 gpt-speed-ui-900.png／gpt-speed-ui-1366.png。


- [x] 语速功能最终 `VSLANG=2052`／`tools\build-windows.bat cuda` 构建成功；完整 CTest **35/35，112.66s**，包含打包、两个启动测试及原有媒体生命周期／取消／恢复回归。生产目录中两个 TTS Worker 与源码 SHA-256 一致；测试凭据残留 **0**。
- [x] 最终 CUDA 包 **5,101,736,431 bytes／4.751362GiB**；Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠）。无新增依赖、CPU 包或模型内容；按脚本预览后清理 package 暂存，保留当前构建和证据。
- [x] 语速人工结论保存为 `speed-review.json` 的 tempoAccepted=true、overallQualityAccepted=false；原速改善但未通过的结论保存为 `human-review.json`，不覆盖历史三语言失败记录。最终日志为 `final-build-ctest.txt`、`final-speed-ui.txt`、`final-worker-hashes.json`、`final-package-size.json`、`test-credentials.txt` 和清理日志。

**该轮后续记录，已由核心流程稳定化范围覆盖**：本样本 0.85 语速不推广为所有音色／文本的全局默认值。Index／GPT 高级音质与多语言、L3 精度优化现已暂停；云端存储已移出当前计划。当前执行下节四项核心验收。

### 2026-10-17：停止功能扩张，进入核心流程稳定化

用户明确收紧阶段目标；本节覆盖此前 L1～L4／P8 历史章节中的扩展优先级。既有实现及未通过结论保留，暂停项不计入当前稳定化交付门槛，也不因此标记完成。

**当前四项核心工作**

2026-10-18 当前状态：四项均在固定参考、文稿与参数下验收完成。Qwen 与云端 12 句整轨均获用户“整体可用”确认，完整工程、实际用量与基本稳定性验证通过。具体证据和历史失败保留在后续各节；暂停项继续暂停，不视为已完成。

- [x] **Qwen 中文音质**：沿用已审核参考、固定代表性文稿和已记录问题句，优先定位首句音色／清晰度与首尾异响；每次有依据地只改变一个条件，保留原生音频、请求及人工结论。既有首句与第五句材料位于 `out/validation/voice-p8-quality-20261017/`，完整参考对照及尾段诊断分别在 `voice-p8-reference-20261017/`、`voice-p8-tail-diagnosis-20261017/`。原生输出与导出差异已检查，不能靠随机重生成、增益／滤镜或 Mock 测试宣布音质通过。
- [x] **真实云端合成验收**：围绕已接入的云端合成路径，准备明确的文稿、Voice ID、发送范围和账号条件，经现有批次确认后验证实际请求、音频下载／解码、候选入库、试听及实际 Token／人民币换算。保留实际用量未知与结果不明的记录、停止后续请求及不自动重发规则；不恢复发送前 Token／金额预测，不扩展音色 CRUD 或素材托管。本轮范围调整本身不发起付费请求。
- [x] **一份完整配音项目**：使用固定的适量代表性文稿与参考，逐项走通导入／编辑 → 分配声音 → 生成／复用 → 候选试听／应用 → 修改一句并重生成 → 连续试听 → 保存／重开 → WAV 导出。核对未修改句和人工裁切保留、选中候选与导出一致、音频与工程可重开；分别记录真实 Qwen 和云端结果，不混用 Mock 结论。优先复用既有真实验收工程和失败材料，避免另建复杂框架或不断扩大语料。
- [x] **基本稳定性**：只修复核心链路中实际发现的问题；保留取消／重试、退出／切工程、保存／恢复、普通失败、候选和实际费用记录不丢失、临时文件及 Worker 退出等必要回归。运行与改动匹配的定向验证和项目规定的完整回归，不新增极限压力矩阵或复杂性能专项。

**暂停清单**：L3 自动情绪识别精度／阈值优化；L4 高级音色管理和完整自动托管；IndexTTS／GPT-SoVITS 高级音质及多语言优化；极限资源压力和复杂性能优化；专属音色微调训练集成。已有功能只在影响上述核心链路时修复必要缺陷；未经用户重新要求，不恢复扩展。

**本轮执行**：核对最新 Qwen 失败结论、原生／导出对照、任务提交／质检／取消和统一项目行为，更新本文件、计划书及统一体验验收范围。本轮仅收紧计划和核对证据，未修改业务代码、模型、设置或依赖，未运行新推理、付费调用、构建或测试；此前 35/35 不冒充本轮执行结果。下一步从 Qwen 已有失败样本及固定完整项目验收开始。


### 2026-10-18：核心流程稳定化——修复失效音频误复用、补验真实工程重开及 Qwen 中文模式对照

**状态：核心复用漏洞已修复，真实资产工程重开／导出通过；Qwen 新对照人工失败，默认不变。** 延续功能冻结，不扩展 L3／L4、Index／GPT、多语言、训练或压力矩阵。

- [x] 审查发现“补齐缺失／失效”对已应用音频仅比较输入指纹，未验证音频文件；云端历史 completed 也可能在音频已丢失时仍视为可复用。新增常规回归先复现损坏文件仍被跳过：**2 passed／1 failed，549ms**，needed=-1，预期 1。首轮新增真实工程测试曾错误向 generateQwen 传 4 个参数导致编译失败，改用已有 generateLocal 的 review 参数后编译通过，未扩大业务 API。
- [x] 在已有 hasCurrentCandidate 中补充已选 Take 的项目资产 ID／类型／SHA、实际解码 SHA、采样率及样本数检查；本地去除只看输入指纹的捷径。云端的失效范围与历史复用同样要求真实可用音频。有效历史候选仍可复用，损坏／丢失时进入待生成预览；云端仍需显式确认，不自动重发，不改历史实际 Token／金额。
- [x] 修复定向 **5 passed／0 failed／2.991s**，覆盖已选音频损坏、临时移走、恢复后继续复用；云端 all／invalid 范围正确列出缺失句、模拟 HTTP 发送次数不变及实际用量记录不变，并保留原用量未知保护。故障注入仅作用于测试拥有的临时资产，恢复原字节后再断言；不是对用户工程的清理或删除。
- [x] 对 `voice-p8-real-20261016/qwen/acceptance.subcue-project` 制作独立便携副本，使用 **12 句、13 个历史 Take 的真实 Qwen 音频**，连续预览缓存准备 → 保存 → 关闭／新工程 → 重开 → 再导出 → Qwen 补缺复用通过，**3 passed／12.824s**。新合成请求为 **0**，前后任务／历史／文稿相同，48kHz WAV **3,011,520 samples／62.74s**，两次导出 SHA-256 同为 `8b22354530c7f60fc56261b7cb282095a8da7ba7a1a04fa3acb6e0f56e2c3a49`；原工程 SHA 不变。此项验证重开／编排与真实音频资产，不冒充本轮重新生成、硬件连续试听或音质通过。
- [x] 核对实际 Qwen SDK 后进行一次中文 nonStreamingMode=false／true 单项对照：相同已确认短参考、首句、seed=12345、BF16 主模型／FP32 tokenizer，只有文本／codec 上下文组织模式不同；诊断副本的 profile 及模型身份包含 true，生产 Worker 保持 false。两个独立 Worker 依次经过 InferenceManager GPU 门禁，正常退出、队列释放；专项 **3 passed／135.629s**。无默认配置、环境或模型改动。
- [x] A 当前模式 **24kHz／134,400 samples／5.60s**，B 完整文本模式 **24kHz／130,560 samples／5.44s**；均有限、无削波，仅作为有效音频检查。A 与历史短参考对照时长相同，最大采样差 **1.189655449707061e-7**，不是文件或样本位级相等，未承诺固定 seed 永久完全确定。
- [x] 用户明确反馈 **“B 没改善或更差”**。B 标为未通过，不采用完整文本模式，不覆盖已选 Take；Qwen 音色／清晰度／异响仍未解决。本轮停止该方向追加生成，不通过增益、滤镜、删尾或随机候选伪装通过。
- [x] Qwen 模型目录前后 **29 文件／2,516,109,553 bytes**，逐文件 SHA-256、大小和清单完全一致。没有下载、训练、安装依赖或修改外部环境；本轮云端仅模拟回归，无真实 API 或新付费请求。

- [x] 最终 `VSLANG=2052`／`tools\build-windows.bat cuda` 构建成功，完整 CTest **35/35，113.68s**，包含打包、两个启动测试与既有生命周期回归。真实 GPU 对照和真实工程专项为上述独立显式执行项，普通 CTest 不重复触发推理。
- [x] CUDA 包 **5,101,737,455 bytes／4.751363GiB**；Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠），低于 4.8GiB。未新增依赖、CPU 包或模型内容。完整测试退出后测试凭据残留 **0**；按脚本预览后仅移除 package 暂存，保留当前 CUDA 构建及验证证据。

证据：`out/validation/voice-core-stability-20261018/` 的 `repro.txt`、`fixed-targeted.txt`、`real-project.txt`、`real-project/audit.json`、`real-mode.txt`、`comparison.json`、`sample-comparison.json`、`human-review.json`、前后模型清单及原生 A／B WAV；最终构建与收尾为 `build-ctest.txt`、`package-size.json`、`test-credentials.txt`、`cleanup-preview.txt`、`cleanup-applied.txt`。

**剩余核心门槛**：Qwen 稳定可接受音质；经现有手动批次确认的真实云端项目、下载与实际费用；完整项目的人工连续试听与最终质量确认，以及后续真实发现的基本故障。当前对照失败作为已知限制保留，不恢复暂停的扩展项。

### 2026-10-18：核心流程稳定化——试听缓存不能掩盖原始音频损坏

**状态：原始 Take 变化后仍播放旧缓存的问题已复现并修复，定向、真实工程及完整 CUDA 回归通过。** 不扩展功能、模型参数或计划范围。

- [x] 检查连续试听发现：缓存命中只校验缓存 WAV 与元数据，未核对原始 Take。源文件内容改变时仍提示“已复用编排缓存”，而整轨导出会拒绝，导致试听与导出不一致；准备失败后还可能保留旧试听行及定位入口。新增回归先复现 **2 passed／1 failed，316ms**，在 rows.isEmpty 断言失败，未以人工试听代替可重复检查。
- [x] 缓存命中后逐句核对原始 Take 实际 SHA-256、采样率、样本数并传播取消；文件丢失、损坏或规格不匹配时明确失败。准备失败时停止播放并释放旧编排、波形、缓存路径与编辑键，避免继续定位播放旧结果；导出失败保留原有可用试听状态。缓存文件不因此删除，恢复原始资产后仍可正常复用，不自动生成或修改已选候选。
- [x] 定向 **7 passed／0 failed，3.191s**：覆盖原始 WAV 内容改变、临时移走、恢复后复用、失败后无旧行／波形及不可开始播放，核对失败导出不发布文件、文稿／候选不变；同时通过已有冻结导出、取消、销毁等待、生成复用完整性及云端下载模拟流程。故障注入只改测试拥有的项目资产，断言前恢复原内容／文件名，源测试媒体 SHA 不变。
- [x] 重新复制已有真实 Qwen 工程，**12 句／13 个历史 Take** 的试听准备、保存重开、再导出及补缺复用专项 **3 passed／11.309s**。导出前后仍为 48kHz／3,011,520 samples／62.74s，SHA-256 `8b22354530c7f60fc56261b7cb282095a8da7ba7a1a04fa3acb6e0f56e2c3a49`；原工程、历史与文稿不变，新合成请求 **0**。未运行新模型推理或真实云端请求，也不以流程测试宣称音质通过。

- [x] 最终 `VSLANG=2052`／`tools\build-windows.bat cuda` 构建成功，完整 CTest **35/35，115.65s**，包含打包、两个启动测试及既有生命周期回归。CUDA 包 **5,101,739,503 bytes／4.751365GiB**；Torch 与 dist-info **3,810,623,753 bytes／74.69%**、cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠），低于 4.8GiB。完整测试后测试凭据残留 **0**，按预览仅清理 package 暂存，保留当前构建和验证证据。

证据：`out/validation/voice-cache-integrity-20261018/` 中 `repro.txt`、`targeted.txt`、`real-project.txt`、`real-project/audit.json`、`build-ctest.txt`、`package-size.json`、`test-credentials.txt`、`cleanup-preview.txt`、`cleanup-applied.txt`。Qwen 上轮 B 对照的否定结论与默认模式保持，真实云端批次及完整项目人工质量验收仍未通过。

### 2026-10-18：核心验收——真实云端两句工程闭环与 Qwen 主声码采样对照

**状态：已授权的两句真实云端工程技术链路通过，用户随后表示现有云端音色列表差不多；参考复刻不继续扩展，历史不一致反馈保留。Qwen B 首句获确认，但 12 句推广验证在第二句长时间未完成，已恢复原正式默认。** 本轮没有扩展产品功能、接口、设置、模型或依赖。

- [x] 新增显式运行的云端工程验收入口。先读取当前北京合成配置，制作独立两句工程、声音绑定和 `ready.json`，准备阶段 **3 passed／349ms**，不联网。清单冻结账号／模型／Workspace、正文、Voice ID、软预算、配置指纹及文稿指纹；确认阶段须逐字节匹配清单 SHA，发送前写 NewOnly 的 attempted 标记，结果不明或失败后不能复跑同一目录。普通 CTest 不启用真实入口。首轮新增测试因同一 auto 声明混用 QJsonObject／QJsonArray 编译失败，分开声明后编译通过，未改业务 API。
- [x] 用户明确授权当前北京账号 `baiqinglan_v3.1` 的 **最多两次合成、软预算 ¥0.10**，正文为“清晨，我推开窗户，看见阳光落在对面的屋顶上。”及“请检查手机电量，再出发。”。授权绑定清单 SHA-256 `d3a1f7b438b07c546a91b0546c9d87d050fb7df16eea4bd1e83a55f9405ef9e6`；此前历史单句授权未复用。
- [x] 真实 `VoiceStudioController → TtsTaskController → CloudTtsProvider` 两句执行及完整工程专项 **3 passed／4.131s**。两条任务均 completed，各获得不同服务请求 ID，完成真实下载、头部适配、解码质检和候选落库；未自动选中，显式应用后准备连续试听、导出、保存关闭／重开，文稿与费用记录一致，再次预览复用两句，未确认新增调用。没有第三次合成或结果不明后的重发。
- [x] 第一句实际输入 **22**／输出 **68 Token**，按工程保存的价格快照换算 **¥0.000849**；第二句输入 **12**／输出 **30 Token**，**¥0.000378**。本批合计输入 **34**／输出 **98 Token**、**¥0.001227**，不是发送前预测或服务商账单核销。实际用量、服务请求 ID 与价格快照随任务保存；保留既有用量未知保护。
- [x] 云端整轨导出 **48kHz／24-bit PCM／单声道／405,120 samples／8.44s**，其中包含句间编排停顿；原始合成结果仍保留在独立工程。此项技术链路通过，不自动扩大为长文、人工音质或账单一致性通过。
- [x] 核对已安装 Qwen SDK，当前主声码默认 do_sample=true。沿用已确认短参考、首句正文、seed=12345、原 nonStreamingMode=false，仅在诊断副本中设 do_sample=false，子声码采样不变；profile／模型身份显式区分诊断值，不修改生产 Worker 或原有候选。受 InferenceManager GPU 门禁管理，真实专项 **3 passed／30.330s**，正常退出、队列释放。
- [x] Qwen A 复用上次相同模型／输入的已存原生基线 **24kHz／134,400 samples／5.60s**，B 新生成 **24kHz／188,160 samples／7.84s**。B 有限、非静音、无削波，不把这些检查当作音质通过。已提供 A／B 和云端整轨播放器请求人工意见；不追加随机候选或后期增益／滤镜。
- [x] Qwen 模型目录运行前后 **29 文件／2,516,109,553 bytes**，全量逐文件 SHA／大小／清单不变，并与上次对照后的清单相符。未下载、训练、安装依赖、调整模型配置或改动用户原工程。

证据：`out/validation/voice-core-acceptance-20261018/` 中云端 `cloud/ready.json`、`cloud/attempted`、`cloud/audit.json`、独立工程及 WAV，`cloud-prepare.txt`、`cloud-live.txt`；Qwen `qwen/request.json`、`qwen/preflight.json`、`qwen/audit.json`、`qwen/events.json`、`qwen/stderr.txt`、前后模型清单和原生 A／B；汇总为 `summary.json`。

**人工结论与后续**：用户确认 Qwen B“明显改善且可用”，只验收本次首句，未扩大为 12 句整体通过。云端反馈“音色和语气完全不同，不通过”，进一步明确“与参考原声不一致”；当前声音库只发现 baiqinglan_v3.1，没有与本地参考对应的指纹记录，已请求提供对应的已复刻 Voice ID，只核对绑定、不自动注册／上传或付费。本批两句授权已消费，后续付费句子仍需新批次确认。

- [x] 曾试将已通过的 do_sample=false 仅接入正式中文路径，进入 profile／模型身份；英文／日文保留原设置，Python **27/27，1.690s**。固定 12 句、同一已审核短参考及 seed=12345 的真实工程中，首句与已接受 B 同为 24kHz／188,160 samples，解码后最大采样差 **0**；第二句超过四分钟未完成，其余十句未启动。Qt Test 在函数 **300,008ms** 超时退出，专项 **1 passed／1 failed**，不记作完整工程通过。
- [x] 因出现上述未解决问题，撤回正式中文贪心设置并恢复原 Worker／原模型身份，不新增 UI 参数或按具体文本特判。首句 B 原件与“明显改善且可用”的人工结论保留；不能推广为全部中文通过。超时后未发现测试或该路径 Worker 进程残留，模型清单不变；停止预览时测试已自行退出，没有执行外部进程终止。Worker 临时目录经脚本预览后清理，已提交首句资产和工程保留。显式专项补充 90 秒无新候选时由任务控制器取消的保护，避免后续同类验证直接耗尽 Qt Test 函数期限；不改变产品生成策略或普通测试行为。

**当前剩余**：Qwen 单句改善条件的多句稳定性与完整项目最终连续试听。云端先按当前已有音色列表使用，不恢复高级声音管理、自动托管或参考复刻扩展；真实合成／下载／候选／费用／保存重开／导出已获本轮新证据，历史“与参考不一致”结论保留，不改写为复刻音质通过。

### 2026-10-18：按用户新要求尝试一次 Flash 本地参考复刻

**状态：真实创建、审核及一条试听成功；人工反馈“差异仍明显，不通过”。** 用户随后明确提出“云端的 qwen-audio-3.1-tts-flash 可以尝试复刻一下本地参考”，仅恢复该参考的一次验证，不恢复 L4 高级音色管理或完整自动托管。

- [x] 重新核对官方声音复刻说明和 HTTP API：该型号使用 voice-enrollment/create_voice，需要音频 URL，不能混用 Qwen3-TTS 的 Base64 接口。官方说明该系列创建音色免费，合成按目标模型计费；WAV 需 16-bit，MP3 可用，参考至少包含 5 秒清晰朗读。原 3 秒短参考不足，故采用用户此前已确认同一说话人的完整原始 `tests/测试音频1/女声/中文/电量不足.mp3`，**7.584s／192,009 bytes**，SHA-256 `f7ba24a9ea820067417e6244ec92c3c3ee3866a19e624c3bdcf14cd2ebb1a442`，不拼接、不加滤镜、不改原件。
- [x] 百炼临时上传凭证查询 model=voice-enrollment 返回 HTTP 200；私有 ACL、禁止覆写和北京 OSS 主机均核对通过。一次上传返回 200；使用 oss:// 内部临时资源地址及 X-DashScope-OssResourceResolve=enable 调用一次创建，实际返回 HTTP 200 与可信新 Voice ID。整个过程复用现有账号，凭据只在进程内读取，不写入文件／命令参数／日志；禁重定向、限响应大小、限时并用独占 attempted 标记阻止重跑。
- [x] 新 Voice ID：`qwen-audio-3.1-tts-flash-sc261018a-402ef84cf48348cf8a648fd3b7f26527`。首次查询 DEPLOYING，随后一次查询为 **OK**，返回 target_model 与 qwen-audio-3.1-tts-flash 完全一致。兼容性仅证明本次临时空间＋复刻组合真实可用，未将实验接入正式托管或增加应用依赖。临时参考按官方规则有效 48 小时；该期限不代表创建音色或服务商全部内部数据会同步删除。
- [x] 用户另行明确授权 **1 句试听**“你好，这是 SubCue 云端配音测试。”，软预算 ¥0.10；没有复用已消费的前两句授权。既有真实 Provider 专项 **3 passed／2.487s**，新 Voice ID 实际合成与下载解码通过，**24kHz／78,720 samples／3.28s**、非静音、无削波，原始下载及仅头部兼容后的文件均保留。
- [x] 该试听服务返回输入 **15**／输出 **41 Token**，按保存价格快照换算 **¥0.0005145**。加此前系统音色两句，本轮真实合成总计 **3 次、输入 49／输出 139 Token，¥0.0017415**；这是已执行用量换算，不是预测或账单核销。音色创建按本轮官方说明免费，实际创建数量为 1；没有额外合成、更新或删除调用。
- [x] 用户确认复刻试听 **“差异仍明显，不通过”**。保存人工否定结论，不采用为当前声音、不覆盖工程候选，不追加随机参考、自动付费重试或删除远端音色。结果只证明该参考的本次复刻相似度未通过，不据此认定所有参考或该型号均不可用。

证据：`out/validation/voice-core-acceptance-20261018/cloud-clone/` 中 `policy-audit.json`、`audit.json`、`attempted`、`query.txt`、`synthesis-ready.json`、`audition/audit.json`、`audition/attempted`、`audition-test.txt`、`clone-preview.wav` 和 `human-review.json`。官方依据：[声音复刻说明](https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide)、[HTTP API](https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api)、[临时文件上传](https://help.aliyun.com/zh/model-studio/get-temporary-file-url/)；核对日期 2026-10-18。

**剩余门槛**：本地 Qwen 多句稳定音质和完整项目最终人工验收；该本地参考的云端复刻相似度。技术链路与人工质量分别记录，不以创建成功、Token 可记录或普通回归通过代替声音相似度。

- [x] 对本轮超时工程制作独立便携副本，经真实 ProjectSession 打开恢复：**1 条 completed 与真实首句候选保留、11 条未完成任务标记 interrupted**，文稿 12 句不丢失；不启动任何生成，因缺少选中音频正确拒绝不完整导出，源工程 SHA 不变。专项 **3 passed／212ms**，日志 `qwen/recovery.txt`。90 秒无新候选的专项取消保护已编译；本轮未再次触发一次真实 90 秒超时，不宣称该路径已专项通过。
- [x] 恢复 Qwen 原默认后的 Python **27/27，1.767s**；最终 `VSLANG=2052`／`tools\build-windows.bat cuda` 构建成功，完整 CTest **35/35，118.83s**，包含打包和两个启动测试。随后本地参考上传实验与单句真实试听只新增忽略目录内脚本、记录及媒体，没有再改业务代码或编译输入；该实际试听另有 **3 passed／2.487s** 证据。普通完整回归不加载本轮真实专项或重复付费调用。
- [x] 最终 CUDA 包 **5,101,739,503 bytes／4.751365GiB**；Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠），未新增应用依赖、模型或运行时文件。源码与构建内 Worker SHA 一致、原 Qwen 默认已恢复；完整专项退出后测试凭据残留 **0**，package 暂存按预览清理，当前构建、真实创建 ID 及失败证据保留。最终收尾日志为 `final-build-ctest.txt`、`final-worker-hashes.json`、`final-package-size.json`、`test-credentials.txt` 和清理日志。

### 2026-10-18：Qwen 重复停滞定位与 12 句诊断工程通过

**状态：固定 12 句诊断工程技术闭环通过，整轨人工试听待确认；正式默认未改。** 继续核心稳定化，不恢复暂停功能或新增云端请求。

- [x] 核对已安装 SDK 与模型：max_new_tokens=8192；SDK 达到上限仍可能返回声码并解码，不能以返回 WAV 认定自然结束。独立 Worker 副本记录真实结束标记并限制 256 Token；未产生 EOS 时抛错，禁止发布截断音频。诊断由 InferenceManager 管理，单句进程限时 180 秒。
- [x] 固定此前停滞的第二句、已审核短参考、seed=12345，保持 do_sample=false／重复惩罚 1.05：实际 256 Token 未产生 EOS，末段一个声码连续重复数十次；没有发布 WAV。原成功型专项如实记录 **2 passed／1 failed，98.005s**，不是正式代码回归失败，也未观察完整 8192 Token。
- [x] 相同限长诊断中只将重复惩罚改为 **1.15**：第二句 **64 Token、EOS=true**，**24kHz／120,960 samples／5.04s**，无削波；专项 **3 passed／19.623s**。诊断 profile 包含 talkerDoSample=false、repetitionPenalty=1.15 与 diagnosticMaxTokens=256，与正式模型身份区分，不推广为全局或多语言默认。
- [x] 将现有集成测试程序复制到隔离临时运行目录，利用既有 bundled Worker 定位加载诊断副本，正式源文件与当前构建均不替换。固定 **12 句＋修改首句后重生成 1 句** 全部自然结束，Token 数 **54～72**，没有触及上限；真实工程专项 **3 passed／168.589s**。覆盖导入、声音分配、候选生成／应用、连续试听准备、修改一句、其它句不变、人工裁切冲突保护、导出、取消再生成、保存及队列释放。测试函数总限时通过 QTEST_FUNCTION_TIMEOUT 设为 900000ms，仅该进程生效；原 90 秒无候选取消保护保持。依据：https://doc.qt.io/qt-6/qtest-overview.html#increasing-test-function-timeout 。
- [x] 独立便携副本重开、保存再重开、再导出与缺失补齐预览专项 **3 passed／64.695s**，没有新合成。**12 句／13 个历史候选**保留，前后导出均 **48kHz／3,011,520 samples／62.74s**，SHA-256 同为 `1b9d1393186ac6c4e390db41a7ed61d87389b2b51212af104dd95f6eeb6ab2da`；源工程 SHA 不变。
- [x] 模型运行前后 **29 文件／2,516,109,553 bytes**，全量 SHA／大小／清单一致。正式 Worker SHA 仍为 `EE3808E6B1E4E11AB807F58D3AE55E03D3419E64FAFA8796D17AB7DC69B13A75`。测试凭据残留 0，诊断 Worker 无进程残留；临时运行目录经脚本预览后移除，保留脚本副本、原始音频、工程和审计。未改生产代码、编译输入、依赖、模型或设置，因此未重跑完整构建／CTest，不将上轮 35/35 冒充本轮结果。

证据：`out/validation/voice-qwen-stop-20261018/` 的 `probe.txt`、`events.json`、`stderr.txt`、`penalty-115/`、`project-decode.jsonl`、`project-test.txt`、`project/audit.json`、`project/first-batch.wav`、`roundtrip-test.txt`、`roundtrip/audit.json`、模型前后清单及 `project-worker.py`。首轮整轨已提供试听，等待音色、清晰度、语速与首尾的人工判断；不能将自然结束或工程闭环通过扩大为音质通过。云端复刻仍保留“差异明显，不通过”，本轮云端调用 0。

**随后人工确认与正式落地**：用户明确反馈 **“整体可用”**，上述待确认状态已更新为本轮固定参考／12 句整轨通过。

- [x] 正式 Worker 仅对 Chinese 显式传入 do_sample=false、repetition_penalty=1.15；英文／日文不传入覆盖值，保持原模型设置。两项中文条件进入 preflight profile／模型身份，旧候选不会被当作新参数结果复用。诊断的 256 Token 上限及内部方法包裹未接入产品，也未改模型文件／默认 UI 参数。
- [x] Python 回归 **27/27，1.633s**，覆盖中文参数与英文／日文不覆盖、异常停止及资源释放。首轮新增测试记录整个 kwargs，意外持有参考条件导致 3 项生命周期断言失败；改为仅记录不含参考对象的参数后通过，没有放宽释放断言或改生产清理逻辑。
- [x] 正式 CUDA 构建与完整 CTest **35/35，116.34s**。正式 Worker 再跑同一 12 句、改单句及取消流程 **3 passed／186.230s**，13 个 completed、1 个 canceled；`first-batch.wav` 与 `regenerated.wav` 分别与人工验收诊断工程的对应 WAV **逐字节一致**，见 `production-verification.json`。正式源码与构建内 Worker SHA-256 同为 `127716ed338bb06088aa3aa9a395071a714764a119ce3ad5272e4f6807de22e8`；模型最终全量清单仍不变。
- [x] CUDA 包 **5,101,739,978 bytes／4.751366GiB**，Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（重叠统计）。打包暂存已预览后清理，测试凭据残留 0；无新增依赖、CPU 包、模型、编辑器或项目设置变更。

**当前边界**：本轮固定参考与代表性文稿的 Qwen 中文音质、完整工程技术流程及人工整轨试听已通过；不是所有声音／文本或多语言普遍保证。云端现有音色合成技术链路沿用此前通过记录；本地参考的云端复刻相似度仍不通过，未追加付费调用。暂停扩展项与云端托管仍不恢复。正式验收工程为 `out/validation/voice-qwen-stop-20261018/production-project/acceptance.subcue-project`。

### 2026-10-18：核心稳定化收尾——真实 Qwen 主声码阶段取消与恢复

**状态：补齐 Qwen 主声码首次前向后的真实取消证据；当前固定参考／文稿的本地核心验收已完成，云端复刻相似度仍未通过。** 不恢复已暂停的扩展，不追加云端请求。

- [x] 复用现有显式 `inferenceCancellationStopsRealTtsStagesWhenRequested`，仅增加 qwen-local 的 talker 分支与动态标签；原 GPT reference／semantic 两阶段保持。普通 CTest 仍跳过真实推理，生产代码／参数未变。
- [x] 独立诊断 Worker 副本只在真实 Qwen3TTSTalkerForConditionalGeneration 完成首次前向后通知取消；观察到 CUDA allocated **2,600,294,912 bytes**、pending event=true。原生专项 **3 passed／29.249s**：取消返回时已持有的 Worker 进程句柄确认退出，没有 result、WAV 或 .part，InferenceManager 队列清空；取消后重新经过同一门禁运行无注入的正式 Worker，成功完成固定首句。没有外部终止其它进程，没有采用假音频或空模型替代。
- [x] 恢复首句 **24kHz／134,400 samples／5.60s**，与已验收正式首句解码后的全部采样值一致，最大采样差 **0**。文件 SHA 不同，仅 WAV 的 PEAK 块偏移 60、61 两个字节不同，不能声称整个文件字节一致。初版补充审计的整文件哈希相等断言因此失败，后续检查改为如实记录文件差异和逐样本比较；原生取消／恢复测试本身通过。
- [x] 模型前后 **29 文件／2,516,109,553 bytes**，逐文件 SHA、大小与清单一致；正式 Worker SHA 仍为 `127716ed338bb06088aa3aa9a395071a714764a119ce3ad5272e4f6807de22e8`。未训练、下载、安装或改变模型／编辑器／项目设置。此项只覆盖主声码上述时点，不扩大为每个推理阶段、真实控制器销毁或极限压力验收。

证据：`out/validation/voice-qwen-cancel-20261018/` 中 `request.json`、`cancel-worker.py`、`real-cancel.txt`、`talker-stderr.txt`、`recovery-stderr.txt`、`stages-and-recovery.json`、`recovery-verification.json` 与模型前后全量清单。同步更新本文件顶部、计划书及统一体验验证的当前状态，保留历史未通过和暂停项目；真实云端创建／合成调用 **0**，费用 **0**。

- [x] 本轮最终 CUDA 构建及完整 CTest **35/35，111.25s**，含打包与两个启动测试；未因阶段取消专项改变普通回归的自动跳过规则。CUDA 包 **5,101,739,978 bytes／4.751366GiB**，Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠）。按脚本预览后移除 package 暂存，保留当前构建与证据；测试凭据残留 **0**，仅核对本次诊断路径的 Python 进程，未发现残留。最终日志：`build-ctest.txt`、`package-size.json`、`cleanup-preview.txt`、`cleanup-applied.txt`、`test-credentials.txt`。本轮只改显式测试入口与文档，未改生产行为；Python 上轮 27/27 保留为上轮证据，本轮未重复执行。

### 2026-10-18：云端复刻失败排查与原句复述对照准备

**状态：离线排查及单句发送准备完成，新的付费调用等待用户确认。** 不覆盖已通过的本地 Qwen 条件，不重复创建、更新或上传参考。

- [x] 核对原创建与试听审计：均为 `qwen-audio-3.1-tts-flash-sc261018a-402ef84cf48348cf8a648fd3b7f26527`，目标型号一致；未发现误用系统音色或其它 Voice ID。原始下载与兼容后的试听 WAV 的 PCM 数据逐字节相同，不能将相似度问题归因于本地容器头适配。
- [x] 只读解码本地中文目录 9 份 MP3 并逐文件核对 SHA 不变；此前参考左右声道完全相同，首声道与本地单声道平均值也相同，排除本文件的声道选择差异。文件总长 **7.584s**；20ms RMS 窗口在 -45／-40／-35dBFS 阈值以上的累计时长为 **4.30／3.90／3.28s**，这是能量统计，不能冒充连续清晰人声时长、说话人识别或已确定的失败原因。目录内没有达到 10 秒的单个素材。
- [x] 重新核对官方指南：推荐 10～20 秒，要求至少 5 秒连续清晰朗读，双声道仅处理首声道。不能用文件总长证明符合人声内容要求，也不通过补静音、重复原句或未经确认的说话人拼接凑时长。现有创建的语种、关闭预处理及音量归一化等参数符合文档类型，未发现可以直接修复的参数错误。
- [x] 下一步仅复用现有复刻音色，合成已审核的参考原句 **“感觉耗电耗得更快了。”**，账号仍为 Qwen 华北2（北京），最多 **1 次**、软预算 **¥0.10**；只改变合成文本，不重建／更新音色，不上传新素材，不自动重发。清单 `ready.json` SHA-256 为 `38ea8b387ea1fef2b9f62b74806e3f25709a670e49b3b3cc87544e1ada585086`。
- [x] 复用现有显式单句验收入口，增加可选冻结清单校验，发送前要求清单哈希、完整账号／型号／Workspace 配置、Voice ID、最多 1 次和 ¥0.10 范围匹配；不传清单时原有已授权测试正文保持。错误哈希拒绝检查如预期记录 **2 passed／1 failed，8ms**，在 Provider 创建与 HTTP 发送前终止，属于故意的本地拒绝验证，没有合成用量。普通 CTest 不启用真实付费入口。

证据位于 `out/validation/voice-cloud-reference-audit-20261018/` 的 `audit.py`、`audit.json`、`ready.json`、`build-target.txt` 和 `rejected-manifest.txt`；官方依据为 https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide 与 https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api ，核对日期 2026-10-18。已请求此次单句授权；此前单句授权已执行完，不能沿用。上述排查没有模型推理、素材上传或新的云端创建／合成请求。

- [x] 最终 CUDA 构建成功，完整 CTest **35/35，111.54s**，包括打包和两个启动测试；普通测试未发送真实合成。CUDA 包 **5,101,739,978 bytes／4.751366GiB**，Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（重叠）；暂存按预览清理，保留当前构建与证据，测试凭据残留 **0**。本轮未改变生产业务、引擎参数、依赖或项目设置。证据为 `build-ctest.txt`、`package-size.json`、清理日志与 `test-credentials.txt`。

**原句对照已获授权并执行**：用户明确同意“这 1 句原句对照”。保持原 `ready.json` 字节和 SHA 不变，按冻结清单使用当前北京账号与既有复刻 Voice ID，实际发送 **1 次**“感觉耗电耗得更快了。”；没有新建／更新／上传素材或自动重发。

- [x] 真实 Provider 单句专项 **3 passed／2.015s**，清单、账号配置、Voice ID 及预算校验通过，下载解码有效；输出 **24kHz／46,080 samples／1.92s**，无削波。原始下载与试听 WAV 的 PCM 数据完全相同，音色绑定与清单一致。
- [x] 实际输入 **10**／输出 **24 Token**，合计 **34 Token**；按保存的价格快照换算 **¥0.000303**，非发送前预测或账单核销。服务请求 ID `5674ad6b-8097-98d8-a60e-b14534cfd6a9`，用量随 `audition/audit.json` 保留。此前三次已授权合成加本句，总计 **4 次、输入 59／输出 163 Token、¥0.0020445**。
- [x] 已提供已审核短参考与云端原句播放器，请用户判断音色、语气和语速。`live-review.json` 单独记录此次授权及已消费范围，不改写原清单，也不把生成成功当作人工通过。当前人工结论待回复，旧“差异仍明显，不通过”样本继续保留。

本次只执行已编译的显式单句专项，未改代码／构建输入，因此未重复完整构建／CTest；上一轮 **35/35／111.54s** 保持为上一轮记录。本次授权已执行完，结果不明或后续试听不通过都不自动追加付费调用。

**原句对照人工结论：未通过。** 用户明确反馈 **“音色差异仍明显，不通过”**，已将 `live-review.json` 更新为 humanAccepted=false／rejected／productionAdopted=false。与此前不同文本试听的否定结论分别保留；在本次相同参考原句下仍未通过，当前证据不支持仅靠改变合成正文即可解决相似度问题。尚不能确定根因是参考内容、服务商特征提取或模型能力，不将其扩大为所有参考或该型号均不可用。

该实验音色不采用为当前声音，保留 Voice ID、原始下载、费用与失败样本，不删除远端音色、不自动重建／更新／付费重试。本地 Qwen 已通过的固定工程与参数继续保留。本次只记录人工反馈，未改业务代码、发起网络请求或运行新测试。若后续继续复刻，应先确定新的参考条件与具体单次验证范围。

### 2026-10-18：梅比乌斯长素材截取与新云端复刻试听

**状态：已按用户新素材测试指令完成一次新建音色与一次原句合成，人工质量待确认。** 原失败音色／样本保留，不覆盖本地已通过方案，不恢复托管产品功能。

- [x] 用户新增同一说话人的 `tests/测试音频1/女声/中文/梅比乌斯音频1.flac`，明确允许自主截取用于继续测试。源文件 **182.70s／48kHz／双声道 PCM16／7,150,201 bytes**，SHA-256 `545b31e4f26ac1e93bd23301a3ed4d87a906a565243f455fe9337f32e49e929a`；前后 SHA 一致，不改原件。
- [x] 用 20ms RMS 低电平区间辅助寻找截取边界，选择原文件 **78.94～96.80s，17.86s** 的一个连续片段，输出 **48kHz／单声道 PCM16／857,280 samples／1,714,604 bytes**。明确取首声道，符合服务端的首声道规则；没有降噪、增益、拉伸、补静音、重复、拼接或重采样。参考 SHA `cef855a1de320748f410963bc0c139d7908bc8f1e345cd982f70c183a72a9748`。能量选段不等于语音识别、句界、背景噪声或音质验收，最终提供参考播放器供人工判断。
- [x] 复用一次性实验脚本和当前北京账号，新前缀 `sc261018b`，WAV 后缀／MIME／文件名同步，max_prompt_audio_length 显式 **17.86**，避免旧 10 秒上限截短新参考。已只读复核参考与脚本 SHA、旧前缀清除、上传前 NewOnly attempted 和结果不明不重发保护；未改生产代码或新增托管依赖。官方创建接口及 10～20 秒推荐重新核对，创建仍免费。
- [x] 私有临时空间只上传上述片段一次，创建一次新音色；初查 DEPLOYING，随后一次查询 **OK**，target_model 严格匹配 `qwen-audio-3.1-tts-flash`。新 Voice ID：`qwen-audio-3.1-tts-flash-sc261018b-923423da8776404c9756f076c09650f4`，创建请求 ID `67ead564-c284-9384-96c5-732558d37899`。未更新或删除任何旧音色，凭据与上传策略／签名未落盘。
- [x] 按本次“使用新素材测试并继续剩余任务”的新指令，将实际合成范围限定为 **1 次**“感觉耗电耗得更快了。”，沿用软预算 **¥0.10**，与上次使用相同文本，未复用已消费的旧批次授权。发送前冻结 `synthesis-ready.json`，SHA-256 `87398ea072949d38e6320ed5b2ed1fbb04604c86f7e86a88fed52c3e8b7559de`，现有原生入口核对完整账号／型号／Workspace、Voice ID、文本与最多 1 次范围。
- [x] 实际单句专项 **3 passed／2.068s**，音频 **24kHz／67,200 samples／2.80s**，无削波，原始下载与兼容后试听 PCM 数据一致。服务返回输入 **10**／输出 **35 Token**，合计 **45 Token**，按保存价格快照折算 **¥0.000435**，非预测或账单核销；合成请求 ID `27f9b3cf-a23d-9bad-87fc-fb8f25e64904`。本轮创建 **1**、合成 **1**，没有自动重发；此前四次加本句累计合成 **5 次、输入 69／输出 198 Token，¥0.0024795**。

证据：`out/validation/voice-cloud-mobius-20261018/` 的 `reference-audit.json`、`reference.wav`、`preparation.json`、`attempted`、`audit.json`、`clone.txt`、`query.txt`、`synthesis-ready.json`、`audition/`、`audition-test.txt` 和 `human-review.json`。已提供参考及新合成播放器并请求人工判断；未通过前不采用为当前声音，不自动扩大为长文或其它音色通过。官方依据：https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide 、https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api 。本轮未改业务代码、编译输入、模型、依赖或设置，没有本地模型推理；只执行现有已编译的显式专项，未重复完整构建／CTest。

### 2026-10-18：按新复刻音色“速度和语气偏快”接入云端原生语速

**状态：原速反馈为有所改善但未通过；0.85 原生语速单句已生成，人工节奏及整体质量待确认。** 不调整全局默认、音调、情绪指令或参考素材，不重建／更新音色。

- [x] 用户反馈“有些改善，但是速度过快，语气过快”，旧 `voice-cloud-mobius-20261018/human-review.json` 更新为未通过并保留此反馈。核对官方 HTTP API：原生参数是 input.rate，范围 **0.5～2**，默认 1，与 SDK 的 speech_rate 名称不同。
- [x] 复用既有每句 speedFactor、严格参数校验、输入指纹、冻结任务及候选失效链路；CloudTtsProvider 声明该能力并仅在显式参数存在时发送 input.rate。默认请求不新增字段；UI 恢复 1 时移除覆盖值，保留原默认身份。复用现有语速控件与草稿／撤销机制，将 GPT 专用标题和错误说明泛化，不改 UI 结构、项目 Schema 或设置。
- [x] 模拟回归先复现当前云端拒绝 speedFactor，**2 passed／1 failed，42ms**；修复后 **3 passed／69ms**，覆盖默认不发 rate，0.5／0.85／1／2 映射正确，非法类型与越界不增加请求数量，并保留原实际费用与不重发规则。原生 QML 定向 **3 passed／38.215s**，验证云端控件可见、参数提交、撤销／重做及恢复原速，截图已人工检查。
- [x] 按新素材测试及本次偏快反馈的调整方向，冻结一条 **0.85** 请求，继续相同 Voice ID、参考、账号与“感觉耗电耗得更快了。”；只有 input.rate 新增为 0.85，没有本地拉伸或变调。`ready.json` SHA `f439d3a019e8047fa4457e7e755dd2377f278444a8c4c17157c2ad0162aea156`，最多 1 次、软预算 ¥0.10，结果不明不重发。
- [x] 实际单句专项 **3 passed／3.239s**，输出 **24kHz／81,318 samples／3.38825s**（原速 **67,200 samples／2.80s**），无削波、原始下载与试听 PCM 数据一致。实际输入 **10**／输出 **36 Token**，合计 **46 Token**，按保存价格快照折算 **¥0.000447**，请求 ID `dbd35b17-0e26-9d65-a5c6-1c3b9e441b07`。本轮创建／更新／上传 **0**，合成 **1**；不按时长计算费用。新参考两次试听合计输入 **20**／输出 **71 Token**，**¥0.000882**；所有本阶段已执行六次合成累计 **¥0.0029265**，不含账单核销含义。

证据位于 `out/validation/voice-cloud-mobius-speed-20261018/`：`repro.txt`、`transport.txt`、`ui.txt`、`cloud-speed-ui.png`、`ready.json`、`audition/`、`live-test.txt`、`human-review.json`。原 FLAC SHA 未变，生产本地 Worker／模型／依赖未改；0.85 只作为本次听审参数，不自动采用为所有音色默认。官方依据：https://help.aliyun.com/zh/model-studio/qwen-audio-tts-http-api 。

- [x] 首轮完整 CUDA 回归 **35/35，103.28s**；随后将共用语速控件的两处 GPT 专用错误提示泛化，最终源码对应的 CUDA 构建与完整 CTest **35/35，98.45s**（`final-build-ctest.txt`），包括打包及两个启动测试。生产包与当前构建 EXE SHA 一致。CUDA 包 **5,101,739,978 bytes／4.751366GiB**，Torch 与 dist-info **3,810,623,753 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（重叠）；暂存按脚本预览后清理，测试凭据残留 **0**。不新增依赖／运行时文件，不更改编辑器或项目设置。

**当前待确认**：已向用户提供原速与 0.85 倍播放器并询问节奏及整体是否可用。人工未回复前仍保留原速“有些改善、节奏过快”的结论，不将原生语速接口成功或普通回归通过扩大为声音质量通过；不自动追加合成或改动全局默认。

**0.85 倍单句人工验收通过**：用户明确确认 **“节奏合适，整体可用”**。`voice-cloud-mobius-speed-20261018/human-review.json` 已更新为 humanAccepted=true／accepted，固定这份新参考、sc261018b 音色及 speedFactor=0.85 的单句使用条件。旧失败音色与新音色原速的未通过记录保留，不改全局默认，不扩大为其它文本、长文或多语言音质通过。本次反馈记录本身没有新合成或代码测试。

### 2026-10-18：已验收云端音色的完整 12 句工程准备

用户随后明确要求“继续执行”，下一项为新云端音色在完整项目中的验证；新增发送范围先准备、冻结并单独确认，尚未发送真实合成。

- [x] 复用现有显式 `voiceCloudProjectAcceptanceWhenRequested`，保留旧两句模式，新增可选固定 12 句计划。采用已验收的 sc261018b 音色，每句 speedFactor=0.85；冻结原文、参数、Voice ID、账号／型号／Workspace、项目输入指纹，以及修改首句的原文／新文。最多 **13 次**，整批软预算 **¥0.10**，不预测 Token 或金额。
- [x] 先用已有模拟 HTTP／本地音频夹具跑通。首轮夹具漏填 finish_reason=stop，正确被 Provider 拒绝，**2 passed／1 failed，347ms**，未发送真实请求；补齐夹具后使用全新离线目录，准备 **3 passed／372ms**，完整离线闭环 **3 passed／2.676s**。离线清单明确包含 offlineFixture=true，不能当作真实批次授权，模拟用量不能当作实际费用。
- [x] 离线闭环确认：首批 12 句完成；第二句保留测试用的 10 样本首部裁切和 150ms 停顿，增益为 1；只改首句后其余 11 句复用；生成 1 个新候选时旧已选 Take 保持，显式应用新候选；未改 11 句的文稿、候选、裁切和参数不变。保存重开后任务／历史／文稿一致，再导出 SHA／规格一致，预览全部复用、不新增请求。
- [x] 两次确认共用 **¥0.10** 软预算：首批全部成功且实际费用已知后，改单句用剩余预算执行 selected 范围；另行免费预览 all 验证 11 句复用后丢弃，避免重复扣减已复用费用。首批失败／用量未知／结果不明或已知费用达到预算均停止，不自动重发。13 条任务和独立请求 ID、实际 Token／金额在最终审计汇总；单次费用可能越过软预算，不宣称硬限额。
- [x] 独立真实工程只完成离线准备，**3 passed／374ms**，没有联网。冻结清单 `project/ready.json` SHA-256 为 `dd1bef2498b9da5f522c2a03697b946380d6f0f9e39eb52ffc78eb04014faa55`。可读发送清单为 `send-list.md`，包含原 12 句及首句改为“清晨，我推开窗户，看见阳光铺满了对面的屋顶。”。已请求这批最多 13 次的新批次确认；此前单句授权不沿用。

证据目录：`out/validation/voice-cloud-full-project-20261018/`，包含计划、可读发送清单、两轮离线日志及工程、真实准备工程和清单。当前生产业务代码、设置、模型和依赖没有新变化；本轮只扩展显式测试入口，未恢复 L3／L4 或托管产品扩展。完整云端整轨的技术闭环与人工质量仍待本批实际运行及听审，不以单句通过或模拟通过代替。

### 2026-10-18：真实云端 12 句完整工程技术验收通过

**状态：用户明确授权“同意这批最多13次”，本批已全部执行成功，随后整轨获人工确认“整体可用”。** 沿用冻结清单、北京账号、`qwen-audio-3.1-tts-flash`、sc261018b 音色与每句 speedFactor=0.85，没有新增／更新音色或素材上传。

- [x] 实际专项 **3 passed／29.995s**，首批 12 句与修改首句 1 次均 completed；共 **13 个唯一请求 ID**，offlineFixture=false、所有结果 mock=false，无自动重发。授权清单 SHA 与原准备值一致，未变更已批准原文、参数或请求上限。
- [x] 逐项实际用量合计 **输入 273／输出 843 Token，共 1,116 Token**；按各任务保存的价格快照折算 **¥0.0105255**，低于整批 ¥0.10 软预算。金额来自实际用量，不是预测或账单核销。`project/audit.json` 保留每条任务、价格与用量。
- [x] 真实验证完整生成／下载／应用候选后，只改首句；其它 **11 句复用且内容、候选、裁切和参数不变**。第二句的 10 样本首部裁切与 150ms 停顿保留；未做本地变速或变调。保存重开后保留 12 句／13 个历史候选，再次预览全部复用，不产生请求。
- [x] 最终 `cloud-revised.wav` 与重开再导出的 `cloud-reopened.wav` **SHA-256 完全一致**：`0ef11a199c4b6af46e953cf2946349847abe41db32585137af84ec3ee2f44931`，均为 **48kHz／单声道／3,675,790 samples／76.57896s**。自动音频检查无削波；此结果不代替人耳质量判断。已提供最终整轨播放器并询问音色、语气、语速与句间衔接。
- [x] 完整 CUDA 构建与 CTest **35/35，104.05s**，含打包及两个启动测试。包 **5,101,739,978 bytes／4.751366GiB**；本次精确统计 `torch/` 加 `torch-*.dist-info/` 为 **3,810,570,269 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（与 Torch 重叠）。包与当前构建 EXE SHA 相同；按脚本预览清理 package 暂存，测试凭据残留 **0**。

正式工程：`out/validation/voice-cloud-full-project-20261018/project/cloud-acceptance.subcue-project`。证据另含 `live-project.txt`、`project/audit.json`、`human-review.json`、`build-ctest.txt`、`package-size.json`、清理与凭据检查日志。当前批次授权已消费完毕，不追加付费调用；本轮未改变生产行为、全局默认、声音绑定、模型、依赖或项目设置。

**整轨人工验收与阶段收尾**：用户对本批最终 12 句整轨明确反馈 **“整体可用”**，`human-review.json` 已记录 accepted／humanAccepted=true、原文反馈及 2026-10-18 日期。该结论覆盖新参考、sc261018b 音色、每句 0.85 语速与本次最终文稿，包含修改首句；不改写旧失败样本，不推广为所有音色／文本。`project/audit.json` 保留技术执行结束时的原始快照，最终人工结论以独立 `human-review.json` 为准。

本阶段 Qwen 中文音质、真实云端验收、完整项目、基本稳定性 **四项全部完成**，本次仅更新验收记录与文档，没有新请求或代码改动，不重复构建／测试。最近完整回归仍为上述 **35/35，104.05s**。L3／L4 扩展、多语言与高级音质、复杂性能、训练及云端托管继续暂停，当前阶段无剩余执行项。

**剩余任务复核**：后续“继续执行”时再次核对当前清单，四项均已完成；早期未勾选项属于历史阶段记录或已暂停范围，不自动恢复。已同步交互重构文档的最新验收摘要与历史边界、标注早期计划状态，并补充 README 的现有逐句语速和实际费用说明。本次只整理文档，未新增功能、运行测试或发起云端请求；恢复暂停范围需先明确下一项工作。

### 2026-10-18：保持功能冻结，修复请求超时与未提交候选误计

用户明确选择 **“保持功能冻结，只修复已有问题”**，据此对现有生成、实际费用、候选和取消流程做有界审查，不恢复历史扩展待办。

- [x] 云端 `generate` 的 `timeoutMs` 曾被忽略，调用方指定 120 秒仍发送默认 360 秒请求。模拟 HTTP 红测 **2 passed／1 failed，112ms**，实际 transferTimeoutMs=360000、预期 120000；最小修复将正值用于合成 POST 与下载，负值保持原默认，连接超时、软预算及不自动重发行为不变。定向 **3 passed／102ms**，覆盖 120000 与 -1 两种输入。
- [x] Provider 返回音频后，候选可能因取消或存储失败未提交，原最终状态却仅按 Provider 的 result 列表计算完成数。使用测试临时工程的资产目录写入故障确定复现：持久任务 failed、历史为空、候选信号 0，但界面状态为 candidate，红测 **2 passed／1 failed，409ms**。改为仅记录本批真正提交成功并定位到的 job ID，再据此设置候选状态和完成数；不改变持久任务、实际费用、Schema 或已提交候选的取消保护。
- [x] 修复定向 **7 passed／4.200s**，包括写入失败不计完成、恢复后重新生成成功、静音拒绝、取消／切工程、云端实际用量及下载音频闭环。新增确定性测试覆盖存储失败分支，未单独注入质检中取消窗口；取消同根路径由实际提交集合修正，并保留既有取消回归，不夸大专项证据。

证据：`out/validation/voice-core-maintenance-20261018/` 中红测、修复定向及构建日志。本次没有真实云端请求、模型推理、下载或新增依赖，不改全局默认与用户工程。

- [x] CUDA 全目标构建成功。首轮完整 CTest 为 **34/35，103.79s**，编辑器集成 target 在 65.48s 失败，但该轮日志未捕获具体 Qt 失败详情；不能认定原因已查明。相同二进制离屏独立复验 **76 passed／0 failed／11 skipped，70.388s**；随后按原 CTest 条件完整复验 **35/35，112.71s**，没有放宽断言或调整超时。首轮失败日志、独立明细和整套复验分别保留为 `ctest-first-lasttest.log`、`editor-offscreen.txt`、`ctest-recheck.txt`。首轮原因尚未定位，若再次出现应优先记录具体失败用例后调查。
- [x] 最终 CUDA 包 **5,101,739,466 bytes／4.751365GiB**，低于 4.8GiB；`torch/` 与 `torch-*.dist-info/` **3,810,570,269 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（重叠）。包与当前构建 EXE SHA 一致。完整测试退出后测试凭据残留 **0**，package 暂存按预览清理，保留当前构建及全部验收证据。日志另见 `package-size.json`、`test-credentials.txt` 和清理记录。

本轮维护修复与验证完成。后续继续保持功能冻结，只处理能够确认的现有缺陷；不把历史扩展重新列为当前执行项。

### 2026-10-18：AI 配音模块现有功能整体检查

**结论：核心流程功能可用，修复一处云端种子输入范围错误；真实质量维持已验收条件，暂停功能不恢复。** 检查文稿／编辑、声音库／分配、引擎参数、生成／候选、试听／编排、保存／恢复、导出、云端用量及取消／退出九组流程，完整覆盖说明见 `docs/UnifiedExperienceValidation.md` 的“AI 配音现有功能整体复核”。

- [x] 本轮独立详细回归：应用控制器 **39 passed／0 failed／1 skipped，3.789s**；编辑器集成 **76 passed／0 failed／11 skipped，68.569s**。原生 Windows 界面专项先通过 **3 passed／38.654s**，检查多尺寸、高 DPI 与实际截图，不把 Mock 音频当真实模型音质。
- [x] 截图发现云端 seed 提示／输入允许 2147483647，但 Flash Provider 实际上限为 65535。增加回归先复现 **2 passed／1 failed，9.882s**，仅改 QML 的种子上限、提示及错误说明，云端用 0～65535，本地原范围保持。新增 -1／65536 拒绝且文稿不变、0／65535 接受、空值移除及本地 2147483647 兼容验证；未更改后端安全校验、默认参数、格式或功能范围。
- [x] 最终 CUDA 构建及完整 CTest **35/35，110.24s**；修复后原生界面专项 **3 passed／38.267s**，最终两份截图已查看。最终包 **5,101,739,978 bytes／4.751366GiB**，Torch 与 dist-info **3,810,570,269 bytes／74.69%**，cuBLAS **1,080,248,544 bytes／21.17%**（重叠）；构建与包 EXE SHA 一致，测试凭据残留 **0**，按脚本预览清理 package 暂存。
- [x] 首轮全量 **33/35，103.84s**，应用控制器和编辑器集成两个 target 失败，均未在详细日志独立复验中再现；后续修复前全量 **35/35，102.78s**，修复后最终全量如上通过。未定位首轮失败根因，没有为通过调整断言、放宽超时或改变测试设置；继续保留偶发回归失败的跟踪项。

证据目录：`out/validation/voice-module-check-20261018/`，含 `build-ctest.txt`、`app-controller.txt`、`editor-offscreen.txt`、`ctest-final.txt`、`seed-repro.txt`、`final-build-ctest.txt`、`native-ui-final.txt`、最终界面截图、包体积和清理记录。Qwen 中文／云端固定工程的真实听审继续沿用已接受记录，本轮没有追加付费合成或真实模型推理；Index／GPT 高级质量、多语言与 L3 精度等仍不能宣称全面可用。未改变编辑器或项目设置。
