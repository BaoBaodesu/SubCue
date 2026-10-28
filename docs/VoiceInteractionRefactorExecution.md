# AI 配音交互重构 v1.1 执行检查

日期：2026-10-17。依据用户提供的《SubCue AI 配音前端交互重构 v1.1.md》，在已有未提交实现上增量落地。

## 实施结论与边界

P0、P1、P2 的代码已接入现有业务；下文操作与自动验证为 2026-10-17 当轮记录，当时未完成真实付费合成和本地模型音质验收。

**2026-10-18 最新验收**：核心流程稳定化四项已在固定条件下完成。Qwen 中文固定参考与 12 句整轨获用户“整体可用”确认，完整项目及取消后恢复通过；北京 `qwen-audio-3.1-tts-flash` 使用新梅比乌斯 sc261018b 音色、每句 0.85 语速，12 句加修改首句共 13 次真实合成、11 句复用、裁切保留、保存重开与导出一致性通过，用户亦确认整轨“整体可用”。实际输入 273／输出 843 Token，折算 ¥0.0105255，最近完整回归为 35/35（104.05 秒）。该结论限于已验收参考、参数和文稿；账号全目录、供应商限流、其它型号／地域／语言不因此全部通过。详情见 [progress.md](../progress.md) 末尾与 `out/validation/voice-cloud-full-project-20261018/human-review.json`。已暂停扩展继续暂停。

实际技术栈为 C++20 / Qt Quick QML，入口是 AppRouter.qml → VoiceWorkspace.qml。ProjectSession 统一协调字幕、粗剪、配音的保存和恢复；VoiceDocument 保存稳定句子 ID、声音版本、已应用候选及非破坏性裁切。继续复用 VoiceLibraryController 的样本分析、ASR 草稿、审核及不可变版本，复用 TtsTaskController 的生成批次与取消，复用 VoiceAudioController / AudioPreviewController 的编排、连续试听和导出。

当前云端 TTS 真实适配范围仍是北京 qwen-audio-3.1-tts-flash、中文与当前 Provider 参数。未把其他地域、语言、模型、克隆或免转写模式伪装为已适配。

## P0：文稿与逐句工作流

- 卡片直接播放、生成/重新生成、展开候选；候选在句内试听、分页和应用。只有显式应用改变正式音频，切换长度时继续保护人工裁切。
- 沿用三栏与底部编排，移除重复正文标题，修复 1366×768 下句内动作裁切。生成范围与导出语义统一。
- 稳定 UUID 多选、复选与范围选择；右栏显示共同属性、混合值、可编辑数及锁定数，可批量语言、参数、增益、停顿、音色分配与生成。
- 独立锁定状态冻结有效音色。控制器保护编辑、删除、候选应用与批量替换；批量属性、生成及默认音色变化跳过锁定句。
- 区分未选音色、待生成、生成中、候选待应用、已应用、失效与失败。批次显示总数、完成数和失败数，取消保留已持久化候选。
- 从现有任务记录恢复未选中句的候选/失败状态；流式查询仅保留当前句状态，不复制完整历史或音频。输入改变不会沿用旧失败状态。
- 源输入变化仅使对应结果失效；增益、停顿和本地裁切保持非生成语义。撤销/重做沿用原命令栈。
- 空文稿、缺声音、缺账号、环境未预检和跨账号/型号/引擎不兼容有就地说明及可操作入口，旧音频保留。
- 空格、Enter、Delete 和撤销/重做沿用应用快捷键，并避开文本输入、输入法组合与模态编辑。

## P1：账号、模型与音色

- 沿用统一账号、受保护凭据与用途绑定，同账号各模型无需重复录入 Key。Workspace 新增账号级保存和官方配置引导；旧模型级覆盖兼容保留。
- “连接并选择模型”只读取元数据，直接进入可读名称列表；适配器自动映射，手工模型 ID 收入高级选项。
- 百炼改用原生 GET /api/v1/models，按地域/Workspace 分页并筛选已适配型号。保留其他厂商原有文本模型发现能力。
- 缺 Key/Workspace、离线、鉴权/权限、限流、错误响应及空列表分别说明。官方模型退路标记来源、版本、日期和权限未验证，不伪装联网成功。
- 自定义音色使用 voice-enrollment 的 list_voice 分页、query_voice 逐项详情校验。只有精确型号匹配且状态 OK 可选；处理中或不匹配项默认隐藏，可查看原因。高级外部 ID 同样校验。
- 官方中文目录 v2 共 49 个当前适配音色，带名称、声线、语言、日期与现成试听资源；保持 ID 大小写，不称为账号实时扫描。
- 选择顺序为项目已使用、收藏、账号兼容自定义、系统目录；支持搜索与不兼容筛选。按账号、地域、Workspace、型号和 Voice ID 去重，保留项目快照和别名，刷新不改变句子绑定。
- 目录缓存按连接身份及凭据版本隔离，保留刷新时间；恢复标记过期，刷新后才可选用账号音色。单个目录缓存上限 2 MiB。
- 系统音色播放官方现成样本，不调用 TTS；自定义 resource_link 标明参考样本、非合成效果，播放前重新校验。无资源时说明须选用后确认费用生成。
- 试听下载不携带 Bearer、不跟随重定向；只保留一个最大 32 MiB 缓存，重复播放复用，切换/退出释放。签名 URL 不进入普通目录、工程或日志。
- 选用与制作分离，继续复用全局档案、不可变版本与项目副本。本地快速列表明确禁用云端音色并解释；切换引擎不冒用声音。
- ASR 草稿、人工校对和质量审核沿用原实现，不覆盖确认文本。Index 可以审核无转写参考；Qwen/GPT 必填转写规则仍由真实 Provider 校验。

官方依据：

- 模型接口：https://help.aliyun.com/zh/model-studio/list-models
- 系统音色及试听：https://help.aliyun.com/zh/model-studio/qwen-audio-tts-voice-list
- 自定义分页、详情与状态：https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api
- 首次配置：https://help.aliyun.com/zh/model-studio/first-api-call-to-qwen

## P2：辅助与编排

- 整篇按标点分句，确认后固定 ID，可撤销；同默认声音的新句继承有效引擎、共同参数和语言，不继承旧音频、锁定或裁切。
- 中英日检测仅作当前能力内的建议，导入预览要求确认；混写或不明确时保留人工选择，不强拆语言段。
- 底部片段、句子、播放器与右栏继续联动，复用连续试听、停顿、增益、拖动裁切与精确数值输入。
- 编排可折叠、记忆布局，桌面尺寸提供有界片段缩放。未生成句和缺波形数据显式占位，不能伪装可播放音频或平直波形。
- 收藏、面板宽度/展开状态和最后引擎恢复于本地状态；现有项目会话保存继续可用。缓存失效、过期导出确认与迁移保护保留。

## 文件与兼容性

核心改动：

- src/app/qml/VoiceWorkspace.qml、VoiceLibraryDialog.qml、CloudSettingsPanel.qml、AppRouter.qml
- src/app/voice_studio_controller.{h,cpp}、voice_library_controller.{h,cpp}、tts_task_controller.{h,cpp}、cloud_settings_controller.cpp
- src/core/tts/voice_document.cpp、src/core/project/project_store.{h,cpp}、src/core/settings/cloud_config_store.cpp
- tests/cpp/test_editor_integration.cpp

未变更文稿版本、SQLite Schema 或 Public Provider 协议；locked 与会话字段是旧文稿可缺省扩展。目录/收藏/状态投影没有成为第二套工程真相。旧句子 ID、历史候选、声音版本快照、裁切、停顿和增益继续使用原格式。

已有其他未提交修改未撤回或覆盖，没有修改编辑器设置，没有下载或改动模型权重，没有提交或发布发行包。

## 当轮实际操作记录（2026-10-17）

单句链路使用真实工程存储、独立测试 Worker 和播放器：新建统一工程 → 两句分段 → 关联声音版本 → 生成测试音候选 → 试听/应用 → 修改与切换候选 → 裁切保护 → 连续音频导出 → 保存/重开与资产迁移。测试音明确为 Mock，不算真实模型音质验收。

多句链路：三句 → 范围多选 → 批量增益/停顿 → 首句锁定 → 编辑/删除保护 → 选中句生成只建立两个未锁定任务 → 状态恢复后未选中句显示候选 → 修改末句仅末句失效。

云端模拟 HTTP 覆盖：原生模型接口、缺 Workspace 退路、账号级保存、跨页去重、状态/型号过滤、过期缓存、Key 更换隔离、现成试听零合成请求/零凭据下载、缓存上限、重复播放不重下载、取消后请求及临时目录清理。未使用真实 Key 发起付费操作或参考上传。

真实 Qt 窗口覆盖 900×440、911×512、960×540、1092×614、1100×700、1280×720、1366×768、1440×900、1920×1080；运行环境为 Windows 原生高 DPI（桌面 DPR 1.75）。两轮截图检查修复句内动作裁切与云端左栏混入本地管理内容。

前后变化：原候选依赖全局侧栏切换，系统音色依赖内部 ID，多选/锁定和发现不完整；现在由卡片直接完成逐句操作，按名称快速选声音，参考制作留在管理窗口，云端生成继续显式确认费用。

截图保存在 C:/Users/baobao/.codex/visualizations/2026/10/17/01a14864-43cb-7013-9c8a-e72ef7f458b0/ 下的 voice-1366.png、voice-1920.png、voice-cloud.png。

## 当轮自动验证（2026-10-17）

定向命令：SubCueEditorIntegrationTests.exe voiceCapabilityDrivenReferenceAndLanguage voiceMultiSelectionLockingAndRecovery voiceCloudDiscoveryScopesAndCancellation voiceNativeModelDiscoveryAndWorkspace voiceEditorCapabilitiesAndBuiltinCatalog qmlSharedProjectWindow voiceLibraryOwnerMaintainsProjectCopies voiceBatchMissingAsrPreservesManualPathAndBindings voiceUnsavedWorkspaceGeneratesExportsAndMigratesAssets。

含初始化和清理共 11 通过、0 失败、0 跳过（19.338 秒）。

字幕回归：SubCueAppControllerTests.exe，38 通过、0 失败、1 跳过（4.212 秒）。本轮修复百炼型号过滤误伤其他厂商文本发现的问题。

最终全量命令：tools\build-windows.bat cuda，主程序与全部目标构建成功，CTest 35/35 通过（105.57 秒），包含字幕、粗剪、媒体生命周期、源代码约束、打包限制与两项启动。真实推理/付费等显式 opt-in 用例仍按测试条件跳过。最后补齐本页音色筛选和直接参考试听后，重新构建 SubCue，打包及两项启动 3/3 通过（15.02 秒）。

高 DPI 独立补验：QT_SCREEN_SCALE_FACTORS=1.75;1.75;1.75;1.75，窗口固定主屏，qmlSharedProjectWindow 含初始化/清理 3/3 通过（17.833 秒）；日志逐项确认 900×440 至 1920×1080 窗口实际 DPR 均为 1.75。第一次全局缩放与主屏缩放叠加导致窗口超出屏幕、拖动断言失败，已修正测试定位/缩放条件并复验通过；没有修改用户系统/编辑器设置。

CUDA 包 5,101,593,865 字节（4.7512 GiB），低于 4.8 GiB；Torch 3.5473 GiB（74.66%），cuBLAS 1.0061 GiB（21.17%）。 保留现有 dist，不覆盖发行版。

## 当轮真实环境验收边界（2026-10-17）

当时真实账号目录可见性、Workspace、权限、自定义音色和供应商限流仍需在用户实际账号验收。模拟 HTTP 不能证明生产服务调用成功。2026-10-17 本轮没有真实付费合成、克隆、设计、样本上传或真实本地大模型音质验收；后续已完成的固定条件验收见页首最新记录，其余边界保留。

未扩展后端尚未支持的型号、语言和其他地域，目录列出不等于新增后端支持。因此代码落地和自动回归结果与真实云端业务验收分开记录，不声称 v1.1 全部真实业务验收通过。

清理结果：已按 tools/clean-workspace.ps1 先预览再执行，仅删除本轮辅助脚本/临时日志与 package staging，释放约 4.75 GiB；当前 CUDA 构建、既有 dist、原有测试资料和模型目录保留。验证日志与截图已复制到上述交付目录。
