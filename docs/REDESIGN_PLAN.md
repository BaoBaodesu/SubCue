# SubCue 配音与声音库重构方案

日期：2026-11-01。本文只做诊断和方案，不改应用代码。

结论先说：

- 「创建音色」没有坏在模型调用上。这个按钮**根本不会去调用克隆模型**。它只打算把已通过人工确认的参考 WAV 写进 `voices.sqlite`。
- 截图里按钮下方那句「请逐项试听并确认勾选的素材及转写；未确认的素材不能创建音色。」就是 `VoiceLibraryController::createFromSamples` 的拒绝文案。点击已经进了函数，然后整批被退回，页面停在第 3 步，所以看起来像没反应。
- 当前引擎是「一句参考音频 + 转写 → 合成时现算」的零样本克隆，不是 fish.audio 那种多片段注册声纹。界面却按「每段都要听完、勾选、裁切、对稿」来设计，和引擎能力、和用户想要的流程都不匹配。
- 建议：**字幕 / 打轴 / 播放 / 粗剪保持现状并继续修；配音子系统按新的音色模型和引擎适配层重建。** 第一阶段先让「选一批音频 → 创建音色 → 用它合成一句」跑通，不必等 GPT-SoVITS v3。

本环境是 Linux，无法编译运行 Windows 版 Qt/CTest。按钮结论来自调用链阅读、现有测试对这条闸门的断言，以及截图中的状态文案与源码字符串完全一致。

---

## 1. 当前架构

SubCue 是 C++20 / Qt 6 桌面程序。QML 只做界面，领域逻辑在 `src/core`，Windows 平台能力在 `src/platform`。进程入口 `src/app/main.cpp` 加载 QML 模块 `AppRouter`（不是体积更大、且已不再作为运行时入口的 `Main.qml`）。

三个工作区由 `WorkspaceRouter` 切换：

| 工作区 | 界面 | 控制器 | 做什么 |
| --- | --- | --- | --- |
| 字幕 | `SubtitleWorkspace.qml`（约 1500 行） | `AppController` | 媒体、时间轴、字幕、ASR、强制对齐 |
| 粗剪 | `RoughCutWorkspace.qml` | `RoughCutController` | 采样区间与 Premiere XML |
| AI 配音 | `VoiceWorkspace.qml`（约 1070 行） | `VoiceStudioController` + `VoiceDocument` | 文稿、音色、句级合成 |

本地推理不进 `SubCue.exe`。`InferenceManager` 持有一把 GPU 租约，拉起独立 Python 进程（`tools/asr_python_worker.py`、`tools/tts_python_worker.py`）。模型权重在仓库外，由 `SUBCUE_MODELS_ROOT` 或设置项 `modelsRoot` 指定。

### 配音数据流（现状）

```text
声音库向导 VoiceLibraryDialog
  → VoiceLibraryController::prepareSamples / transcribeSamples / createFromSamples
  → VoiceLibrary（voices.sqlite + .assets 里的 WAV）
  → 分配进工程时 VoiceLibrary::copyVersionToProject
  → ProjectStore::importVoiceVersion（工程 sqlite + 工程 .assets）

配音工作区「生成」
  → VoiceStudioController::generateLocal / prepareFlash / generateMock
  → 为每句组装 TtsRequest（文本、语言、音色版本、参考指纹、参考转写、参数）
  → TtsTaskController 写入工程 jobs 表，再调用 Provider
  → 本地：一个 Python 进程吃完整批，批内加载一次权重，进程退出即卸载
  → 云端：通义 Flash 逐句 HTTP，使用账号里的 Voice ID，不上传本地参考
  → AudioQuality 质检 → 写成 Take → 界面候选列表
```

音色在库里是「档案 profile + 多个不可变版本 version」。**一个版本 = 一条参考 WAV（或一条云端 Voice ID）加一份 JSON 元数据。** 没有说话人嵌入、没有 SoVITS prompt 缓存、没有可复用的微调权重。合成时 `generateLocal` 只取**一个**已审核版本的参考文件。

引擎能力写在 `LocalTtsProvider::capabilities` 和 `CloudTtsProvider::capabilities`：

| 界面名称 | provider | 参考音频 | 必须有参考转写 | 语言 |
| --- | --- | --- | --- | --- |
| Qwen Base · 本地 | `qwen-local` | 要 | 要 | 中 / 英 / 日 |
| Mock · 测试音 | `mock` | 不要 | 不要 | 仅中文 |
| 通义 Flash · 云端 | `qwen-cloud` | 不要（用云端 Voice ID） | 不要 | 仅中文 |
| IndexTTS 2.5 · 本地 | `index-local` | 要 | 不要 | 中 / 英 / 日 |
| GPT-SoVITS · 本地 | `gpt-local` | 要 | 要 | **仅中文** |

Qwen 在 Worker 里调用 `create_voice_clone_prompt(ref_audio, ref_text)` 再 `generate_voice_clone(text, language)`。GPT-SoVITS 走 `tools/tts_external_engines.py`，权重写死为 v2ProPlus 的 `s1v3.ckpt` + `s2Gv2ProPlus.pth`，`prompt_lang` 和 `text_lang` 都写死 `all_zh`。Index 只用参考音频路径，不把转写发给模型。

---

## 2. 「创建音色」为什么点了没反应

### 调用链

第 3 步蓝色按钮就是这一处（`src/app/qml/VoiceLibraryDialog.qml`）：

```qml
PrimaryButton {
    objectName: "createReferenceVoice"
    text: creationStep === 2 ? qsTr("创建音色") : qsTr("下一步")
    enabled: controller && !controller.busy
             && creationName.text.trim().length > 0
             && creationSamples.count > 0
    onClicked: {
        if (creationStep < 2) creationStep++
        else controller.createFromSamples(creationName.text, dialog.transcriptRequired)
    }
}
```

按钮是否可点，只看「不忙、名称非空、列表非空」。**不看有没有确认。** 截图里名称是「梅比乌斯v1」，列表里有片段，按钮是蓝色的，所以点击会进入 `createFromSamples`。

`VoiceLibraryController::createFromSamples`（`src/app/voice_library_controller.cpp`）的实际逻辑：

1. `creating_` 为假或 `busy_` 为真：直接 `return`，**不改状态文字**。这是另一条真正的「完全没反馈」路径。
2. 跳过没勾选的样本。`ReferenceSamples::prepare` 给每个切出来的片段默认 `"selected": true`（`src/core/tts/reference_samples.cpp`）。
3. 只要有一条选中样本满足下面任一条件，**整批放弃**，并写入截图里的那句话：
   - `confirmed` 不是 true
   - 样本带 `error` 字段
   - `transcriptRequired` 为真且转写去空白后为空
4. 通过后才 `VoiceLibrary::createWithVersions`：建 profile，把 WAV 复制进 `.assets`，写元数据。没有 Python，没有 GPU，没有声纹注册。

`transcriptRequired` 不是向导自己的设置。`AppRouter.qml` 把它绑到**当前配音引擎**：

```qml
transcriptRequired: voiceWorkspace.capabilities.referenceRequired !== true
    || voiceWorkspace.capabilities.transcriptRequired !== false
```

默认引擎是 Qwen Base，转写必填。切到 IndexTTS 才放宽「空转写」，但 **`confirmed` 仍然必填**。

### 和截图对得上的原因

提供的第 3 步截图底部已经是这句拒绝文案，不是步骤说明。步骤说明是另一句：「核对名称和参考后创建音色。首个已确认样本作为默认参考……」。所以这次点击已经执行，并停在闸门上。

同一段录音在第 2 步和第 3 步截图里出现了不同的时长和不同的句子（「总觉得最近总是很困呢」对「清晨的阳光真是刺眼」等），文件名都是 `梅比乌斯音频1.flac`。这符合「勾选多段整理后，按静音把一条长录音切成很多 3～12 秒片段」的行为。切分结果**每一段默认选中**。自动转写结束时会把 `confirmed` 明确写成 false（`transcribeSamples`），界面也写了「自动识别不会替你确认」。

因此：屏幕上这几条即使已经勾了「已试听、核对转写并接受质量提示」，列表里只要还有一条选中但未确认、或转写为空的片段（经常在滚动区域外面），整次创建就失败。失败信息：

- 用 `Theme.secondaryText` 画在试听条上面，和普通提示一个颜色
- 不指出是第几条、文件名、缺的是确认还是转写
- 不弹窗，不跳转，按钮保持可点

用户看到的就是：按钮按下去，人还在这一页。

现有测试把这个闸门当成正确行为。`EditorIntegrationTests::voiceCreationDraftIsReviewedAtomicAndCancellable` 先 `editSample` 写好转写，再 `createFromSamples`，断言档案仍是空的；只有再 `confirmCreationSample` 之后才会真正建档。QML 回归也是手动 `setProperty("checked", true)` 并调用 `toggled` 之后，第二次点击才创建成功。测试证明的是「未确认就拒绝」，不是「用户勾了就一定写进模型」。

### 为什么勾选框看起来已经勾上，模型里仍可能是未确认

这几条会让界面和 `samples_` 不一致。它们不是截图里那句文案的替代解释，而是「我明明全勾了」仍然会撞上同一句文案的原因。

1. **确认框是单向绑定加回写，失败时不说话。**  
   `checked: !!modelData.confirmed && creationText.text === (modelData.transcript || "")`，`onToggled` 再调用 `confirmCreationSample`。用户点击会打断这个绑定。`confirmCreationSample` 在这些情况下直接 `return`，不 `emit changed()`，也不写 `statusText`：
   - 向导已结束或正在忙
   - 下标越界
   - 语言不是 `Chinese` / `English` / `Japanese`
   - 样本含 `error` 键  
   语言来自 `["Chinese","English","Japanese"][creationLanguage.currentIndex]`。组合框**没有**从 `modelData.referenceLanguage` 回填；`currentIndex` 为 -1 时语言是空字符串，函数直接返回。勾选框可以保持勾上，模型里的 `confirmed` 仍是 false。

2. **改字会悄悄取消确认，而且不通知界面。**  
   `editSample` 发现转写字符串变了就把 `confirmed` 设为 false，然后**不** `emit changed()`。转写框的 `onTextChanged`、以及文件名旁边那个「选中」勾选框的 `onToggled`，都会走 `editSample`。选中框的回调把 `creationText.text` 写回模型。委托重建时，如果这个信号在转写框还是空字符串的时候先触发，会把已经识别好的转写清掉，并取消确认，界面却还显示旧句子。

3. **语言下拉会主动取消确认。**  
   `creationLanguage.onActivated` 调用 `confirmCreationSample(index, false, language)`。用户只是点开「中文」确认一下，这条样本的 `confirmed` 就被清掉，必须再勾一次。

4. **裁切会清空转写和确认。**  
   `cropCreationSample` 成功后把 `transcript` 设为空、`confirmed` 设为 false。第 3 步仍把这条算作选中样本。

5. **第 3 步只是把 QML 的 `creationStep` 加一。**  
   步骤号存在对话框里，不在控制器里。点「下一步」不检查确认，也不把文本框里尚未写回模型的内容提交进去。`createFromSamples` 也不像 `saveSamples` 那样先 `inputMethod()->commit()`。

6. **失败样式让人以为没执行。**  
   拒绝之后只 `emit changed()`。向导不关闭（关闭条件是 `operationFinished(success)` 且 `creating` 已变 false，而闸门在 `submit` 之前就返回了）。

### 闸门通过之后才会碰到的下一层

通过确认后，`storeVersion` 要求元数据里的 `quality.sha256` 和复制后的文件哈希一致，否则报「参考与审核指纹不一致」。现有测试说明正常 WAV 可以走通。这不是这次「点了没反应」的原因，但第一阶段改闸门时要保留这个校验，避免把未检查的文件标成 `approved`。

还有一条产品层的落差：就算按钮修好了，创建出来的也只是「一组参考文件」。fish.audio 的「创建」会得到一个可复用的音色模型；这里要到后面每一句「生成」时，才用其中**一个**版本的 WAV 现算。用户会觉得创建成功了，配音却还是每次都在吃原始参考，而且换工程要再复制一份资产。

---

## 3. 分领域问题

优先级按「用户现在就撞上」排序。P0 不修，配音功能不可用；P1 让功能难用或结果不可复用；P2 是结构和维护成本。

### 3.1 前端

| 级别 | 问题 | 位置 |
| --- | --- | --- |
| P0 | 创建按钮的可点条件和真正的成功条件不一致；错误用次要文字颜色，不指向具体片段 | `VoiceLibraryDialog.qml` 创建页页脚；`statusText` 标签同样用 `Theme.secondaryText` |
| P0 | 三步向导把「听、改字、裁切、语言、确认」堆在同一个 ListView 委托里。委托是 `ColumnLayout`，没有按「一行一条素材」排，截图里勾选框、裁切框和文件名的视觉顺序已经和操作对象对不齐 | `VoiceLibraryDialog.qml` 中 `creationSamples` 的 delegate |
| P0 | 配音主按钮「生成」的 `enabled` 不看 `generationReadiness`。环境没预检、没音色时按钮仍是亮的，点下去才在 `generate()` 里写 `draftError`，或在 C++ 里直接 return | `VoiceWorkspace.qml` `voiceGenerateButton`；`VoiceStudioController::generateLocal` 在忙或范围无效时无状态返回 |
| P1 | 界面状态散落在 QML：向导步骤、引擎下拉的 `acceptedIndex`、文稿草稿、`draftDirty`、收藏音色、左右栏比例。控制器不知道用户停在第几步 | `VoiceLibraryDialog.qml` 的 `creationStep`；`VoiceWorkspace.qml` 前半的属性 |
| P1 | 声音库有两套 UI：对话框里一套档案/版本/云音色，配音左栏又一套。规则（转写是否必填、能否跨引擎）只在其中一处体现 | `VoiceLibraryDialog.qml` 与 `VoiceWorkspace.qml` 音色分配区 |
| P1 | `VoiceLibraryPage` 是铺满父级的假对话框。返回、试听条、页脚按钮和内容区叠在同一个 Page 里，`DropArea` 再盖满整页 | `VoiceLibraryPage.qml` |
| P1 | 主工具条同时放五个引擎，其中包括「Mock · 测试音」。用户要在生成前先找到「配置…」，再到设置窗口填 Python 和模型目录 | `VoiceWorkspace.qml` `localEngine` |
| P2 | `SubtitleWorkspace.qml`、`VoiceWorkspace.qml`、`SettingsWindow.qml`、`RoughCutWorkspace.qml` 都是单文件巨型界面。`Main.qml` 仍保留一套平行的打轴/预检 UI，测试还在加载它，和运行时 `AppRouter` 会漂移 | `src/app/qml/` |
| P2 | 「训练专属模型」「导入已有模型」按钮能点进去，里面的操作全部 `enabled: false`，文案是「后端尚未接入」 | `VoiceLibraryDialog.qml` `unavailablePage` |

### 3.2 后端（领域模型与 Provider）

| 级别 | 问题 | 位置 |
| --- | --- | --- |
| P0 | 音色版本只能表达「一条参考」。多片段素材被收成多个版本，合成只用 `effectiveVoiceVersion` 的那一条。没有「一个音色、一组参考、一份当前 prompt」 | `VoiceLibrary`、`VoiceDocument::effectiveVoiceVersion`、`generateLocal` 里 `store.voiceVersion(...)` |
| P0 | GPT-SoVITS 被接成「只能中文、只能 v2ProPlus 预训练、语言写死 all_zh」。这和用户已经在 v3 网页上跑通的「日语参考 → 中文输出」不是同一条能力 | `LocalTtsProvider::capabilities`；`tools/tts_external_engines.py` 的 `load` 与 `batch` |
| P1 | 创建、审核、云端开通、批量整理、派生裁切全挤在 `VoiceLibraryController`（约 1300 行）。库写入、ASR、情绪识别、云 HTTP 共用一个 `busy_` / `epoch_` | `voice_library_controller.cpp` |
| P1 | 云端 Flash 与本地克隆被放进同一个引擎下拉，但数据模型互斥：本地版本不能用于云端，云端版本不能用于本地。读iness 用长句子说明，用户需要自己换引擎 | `VoiceStudioController::generationReadiness` |
| P1 | 工程导入音色时按同一个 version UUID 复制 WAV。库是全局的，工程却再存一份，两边没有「库更新了，工程要不要跟上」的关系，只有「不能覆盖」 | `VoiceLibrary::copyVersionToProject`、`ProjectStore::importVoiceVersion` |
| P2 | `VoiceDocument::inputKey` 不含参考内容。换了参考转写或参考语言，句级「是否失效」可能仍认为旧候选有效；真正的指纹在 `TtsRequest::inputHash` | `voice_document.cpp` |
| P2 | Worker 预检失败或取消时，Index / GPT 也会显示「Qwen 预检已取消。」 | `VoiceStudioController` 预检完成回调 |

### 3.3 运行时逻辑

| 级别 | 问题 | 位置 |
| --- | --- | --- |
| P0 | 每一批 TTS、每一次 ASR，都新起 Python 进程并重新加载权重。声音库转写是一个 GPU 租约里对每个源文件再 `transcribe` 一次，Qwen 路径还会为跨切口的片段再识别一遍短音频。建一个音色就要把 0.6B 模型反复装进显存 | `LocalTtsProvider::generate`、`LocalPythonAsrService::transcribe`、`VoiceLibraryController::transcribeSamples`、`tools/asr_python_worker.py`（先卸 ASR 再加载 Forced Aligner） |
| P1 | GPU 是全局一把锁。TTS 预检也走 `InferenceManager::run` 且默认占 GPU。配音预检、样本转写、字幕 ASR 互相排队 | `inference_manager.cpp` |
| P1 | 本地批次里一条失败就 `break`，后面的句子不再合成。控制器最后把没跑的 job 标失败，但界面主要看到汇总句，不容易知道停在哪一句 | `tts_python_worker.py`、`tts_external_engines.py`、`TtsTaskController` |
| P1 | 一批里只要有失败，`finished(success=false)`，即使前面几句已经写出候选。用户分不清「全失败」和「部分可用」 | `TtsBatchReport::success`、`TtsTaskController::start` |
| P1 | 工作区切换只看工程会话是否 `busy`，不看编辑器、TTS、声音库。生成或打轴过程中可以切走 | `WorkspaceRouter::canSwitch` |
| P2 | `createFromSamples`、`submit`、`generateLocal`、`approveDerived`、`startAlignmentRun`、`confirmCreationSample` 都有「条件不满足就 return、不告诉 UI」的分支 | 上列函数 |
| P2 | Qwen / Index / GPT 输出的是 float WAV。导出文案如果按 48 kHz 24-bit PCM 理解，会和文件实际格式不一致 | Worker 里 `sf.write(..., subtype='FLOAT')` |

播放侧本身有上限：包队列、帧队列、音频缓冲和 128 MiB 帧缓存是写明的。问题集中在「每次推理都冷启动」和「失败不落到具体句子」，不是播放线程泄漏。

### 3.4 操作逻辑

当前建音色的操作顺序是：

1. 添加音色 → 参考音频创建（训练和导入是空页）。
2. 选文件。长于 30 秒必须勾「多段整理」，否则 `prepare` 直接失败。整理按静音切成最多 12 秒一段，上限 2000 段或 512 MiB。
3. 若本地 ASR 可用，自动转写，但**不**替用户确认。
4. 用户对每一段：试听、改转写、选语言、可选裁切、勾选「已试听、核对转写并接受质量提示」。
5. 下一步。这一步不校验。
6. 创建。任何一条选中样本未确认，整批失败。

这和 fish.audio 即时克隆的差别不是皮肤，是任务定义不同：

| | fish.audio（截图） | SubCue 现在 |
| --- | --- | --- |
| 素材 | 一次上传或录很多短片段 | 一次可选多文件，但长文件要先决定是否切段 |
| 进度 | 总时长一条条：10 秒最少、30 秒够用、90 秒更好 | 没有总时长；单段参考硬限制 30 秒，切段目标是 ≤12 秒 |
| 检查 | 一个「分析音频」，每条显示分析中 / 通过 / 语言 | 每条手动试听、改字、裁切、两个勾选框 |
| 失败 | 单条状态，不挡住其它条 | 一条不合格，整批不能创建，且不指出是哪条 |
| 创建 | 单独一页：名称、封面、描述、标签、公开范围 | 名称在列表顶上，创建只是页脚按钮 |
| 产物 | 一个可复用音色 | 多条参考版本，合成时只用一条 |

配音工作区的操作也重：先在设置里准备 Python 和模型目录，回到配音页选引擎，点「检查当前环境」，再在左栏建音色、分配到句，然后才能生成。GPT-SoVITS 网页是「参考、目标文本、语言、一个合成按钮」，参数在同一页但有默认值。SubCue 把等价信息拆进设置窗口、引擎条、左栏、检查区和候选抽屉。

其它操作缺口：

- 打轴在 `busy_` 时直接 return，没有「正在进行」的说明（`AppController::startAlignmentRun`）。本地模型目录、Forced Aligner、Python 是否存在，预检并不查，失败发生在跑起来之后。
- 保存按钮的可用条件不算 TTS 和声音库是否在忙。
- 离开工程时 `commitAllDrafts()` 失败会直接 return，用户不一定看到原因。

### 3.5 缓存与存储

| 数据 | 放在哪 | 问题 |
| --- | --- | --- |
| 全局音色 | 设置目录下 `voices.sqlite` 和旁边的 `.assets` | 只存 WAV 和 JSON，没有引擎产物缓存。清理工具默认不扫它 |
| 工程音色 | `<工程>.sqlite` 的 `voice_versions`，WAV 再复制进 `<工程>.assets` | 每个工程一份。换机器靠工程包，不靠音色库 |
| TTS 任务 | 工程 `jobs` 表，输出在工程资产；临时目录 `.subcue-tts-*` | 批与批之间不复用已加载的模型 |
| Worker 缓存 | 输出目录里的 `.worker-cache`（HF、CUDA、Numba） | 跟着任务目录走，不是稳定的模型缓存；任务结束策略不统一 |
| ASR 分析缓存 | 系统缓存目录 `analysis/`，键是整文件 SHA-256 | 没有容量上限，只能靠 `StorageCleaner` 整类删 |
| 试听缓存 | 应用数据目录 `voice-preview-cache` | 控制器报告的 `previewCacheBytes` 只反映当前云端试听文件，不是目录总量 |
| 样本草稿 | `QTemporaryDir` | 有 512 MiB / 2000 段上限，这点是对的；取消向导会删草稿 |

播放和波形没有把整段 PCM 按时长堆在内存里，这块约束是守住的。配音侧缺的是「按内容哈希复用推理结果」。

### 3.6 模型复用

- Qwen 的 `voice_clone_prompt` 只活在一个进程里，最多留 4 个，进程退出就没了。下一批同一音色要重新从 WAV 编码。
- GPT-SoVITS 每次 `model.run` 都读 `ref_audio_path`。没有把参考音频的语义 token / 风格嵌入落到磁盘。
- 微调权重没有入口。预检只认固定文件名 `s1v3.ckpt` 和 `v2Pro/s2Gv2ProPlus.pth`。用户自己的 v3 GPT 权重、SoVITS 权重无法选。
- Index 同样每次从参考 WAV 现推，虽然它不需要转写。
- 通义自定义音色是另一套云端 enrollment（`confirmCloudClone`），和本地参考库互不通用。
- 跨工程复用只做到「同一个 version UUID 再复制一遍 WAV」。做不到「这个说话人在所有工程里是同一个音色，引擎缓存也只有一份」。

---

## 4. 目标架构

不重写播放器、时间轴和打轴。配音改成下面四块。字幕工程继续通过「当前句文本 + 时间范围」把稿子交给配音，这一边界保持。

```text
VoiceLibrary          全局音色：身份、封面、标签、一组素材、一条当前 prompt
        │
        ▼
PromptCache           按 (引擎, 模型版本, prompt 内容哈希) 存可复用产物
        │
        ▼
EngineAdapter         统一的 synthesize(text, language, prompt, params)
        │
        ├── QwenBaseAdapter          现有 Worker，保留
        ├── GptSovitsAdapter         本地 API 优先，内嵌 Worker 其次
        ├── IndexAdapter             可选，情绪和时长
        └── CloudPresetAdapter       通义成品音色，不走克隆
        │
        ▼
SpeechJobQueue        句级任务、进度、取消、错误落到句子上
        │
        ▼
ProjectPin            工程只记录 voiceId + prompt 哈希 + 参数快照
```

### 4.1 音色与参考怎么存

全局库仍放在设置目录，避免拷进仓库和安装包。建议目录：

```text
voices/
  library.sqlite
  assets/<sha256>.wav          内容寻址，只存一份
  voices/<voiceId>/voice.json  展示信息和素材清单
  covers/<voiceId>.<ext>
  cache/<engine>/<modelVersion>/<promptHash>.bin
```

`voice.json` 最小字段：

- `id`，`name`，`description`，`tags`，`cover`
- `clips[]`：`assetSha256`，`durationMs`，`language`，`transcript`，`transcriptOrigin`（人工 / ASR），`qc`（通过 / 警告 / 失败 + 原因）
- `prompt`：真正拿去合成的那一段。`assetSha256`，`startMs`，`endMs`，`transcript`，`language`。由质检自动选出，用户可以改
- `createdAt`

不要把 API key、云账号密钥放进这个 JSON。云端成品音色只存 provider、Voice ID 和 revision，沿用现在的「凭据只留引用」。

合成参数（seed、语速、top_k、top_p、temperature、怎么切句）属于**这一次生成**或工程里的句，不属于音色。音色是说话人，不是某一次网页上的滑块位置。

工程侧：

- 默认只 pin `voiceId` 和当时的 `promptHash`。打开工程时向全局库解析。
- 提供「嵌入到工程」给需要拷走的项目，相当于今天的 `importVoiceVersion`，但是整份 prompt 素材一次拷贝，而不是每个版本各拷一次。
- 库里的音色改了 prompt 之后，旧工程保持旧哈希，直到用户点「更新到音色库最新」。

### 4.2 引擎适配

每个适配器只回答四件事：`capabilities()`、`check()`、`preparePrompt(clips) -> cache`、`synthesize(jobs, prompt)`。

能力里把「参考语言」和「目标语言」分成两个列表。今天 `TtsCapabilities::languages` 同时用来卡两者，GPT 被卡成只能中文，Qwen 反而已经在 Worker 里分开传了。

适配器背后可以是现在的 stdin/stdout Worker，也可以是本机 HTTP。界面不关心是哪一种。

### 4.3 缓存

- **模型进程**：每个引擎一个长期存活的 Worker。空闲超时（例如几分钟无任务）再退出并 `empty_cache`。取消时发取消标记，超时再杀进程。禁止为了省事在每次「生成当前句」时重新 `from_pretrained`。
- **Prompt 缓存**：Qwen 的 clone prompt、GPT-SoVITS 的参考语义 token、Index 的说话人条件，键都是 `(engine, modelVersion, prompt 音频哈希, prompt 转写, prompt 语言)`。写入 `voices/cache`。模型版本变了就失效。
- **不要**为了缓存把整段长音频的 PCM 留在内存里。缓存的是模型条件，体积应有上限和可查询的字节数，超限按最近使用淘汰。
- ASR 分析缓存保留「整文件哈希 → 结果」，但要加上容量上限，避免 `analysis/` 无限涨。

### 4.4 任务队列和错误

一句一个 job：`id`，`segmentId`，`text`，`language`，`voiceId`，`promptHash`，`parameters`，`state`，`error`。

- 状态：排队、运行、完成、失败、已取消。
- 失败写在**那一句**上：原因、引擎、是否可以只重试这一句。
- 一条失败不丢弃已经完成的句子，汇总明确写成「完成 12，失败 1」。
- 取消传递到 HTTP、Python 和质检，和现在的 `std::atomic<bool>` 一样，但是 UI 要显示「正在取消」并在结束后恢复按钮。
- C++ 侧禁止再出现「校验失败就 return」而不带 `status` 或结构化错误。QML 用警告色或句子旁的错误，不用次要文字颜色报告失败。

GPU 锁继续用 `InferenceManager`，但租约的粒度改成「占用这个引擎的 Worker」，而不是「为了跑 20 句就把进程创建销毁一次」。ASR 和 TTS 仍互斥，避免两套模型同时进显存；排队时界面说明是在等谁。

---

## 5. 声音克隆流程（按 fish.audio 的操作，不按它的服务）

本地引擎吃的是一条干净参考，不是 90 秒一次性注册。流程上学习 fish 的「收集 → 一次分析 → 一页资料 → 创建」；实现上把通过质检的素材存下来，并**自动挑或拼出 6～12 秒的 prompt**。90 秒是素材库的舒适区，不是单次推理的输入长度。现在的 30 秒硬限制和 12 秒一切，对「收集素材」太严，对「一条 prompt」又没有自动选择。

### 屏幕 A：收集

- 标题就是音色草稿，不出现「1 / 3」。
- 两个主按钮：上传音频、录制音频。一次可以加很多条。
- 右侧或下方是片段列表：文件名、时长、大小、删除。支持拖放。
- 底部一条总时长：已上传时长；10 秒起可分析，30 秒标记为够用，90 秒标记为更好。不足 10 秒时主按钮不可用，并写明还差几秒。
- 不在这一屏放转写框、裁切框、确认勾选。

### 屏幕 B：一次分析

- 一个按钮：「分析音频」。
- 每条独立状态：排队、分析中、通过、警告、失败。通过时显示检测语言（中 / 英 / 日，其它标成未知）。
- 分析内容：解码、静音、削波、过短（少于约 3 秒标警告）、语言和转写。转写用现有本地 ASR（Qwen3 / FunASR），失败的条目标失败原因，**不调用云端**。
- 某一条失败不影响其它条。结束后主按钮是「继续」，只要至少有一条通过且总的可用时长达到下限。
- 转写结果先存着，默认不展开。需要改某条时再点开。不要用勾选框代表「我听过了」。

### 屏幕 C：音色资料

对应 fish 的详情页，也是创建前的唯一一页：

- 名称（必填）
- 封面（可选）
- 描述
- 标签：性别、年龄感、场景、风格。用一组可选标签，不要再做第三步向导
- 语言：由分析得到，可改。这里的语言是**参考语言**，不是配音目标语言
- 当前 prompt：默认用质检最好的一条，或把相邻的干净片段拼到约 10 秒。旁边可以试听，可以改成另一条通过的素材
- 若引擎需要参考转写（Qwen、GPT-SoVITS），只编辑 **prompt 这一条** 的转写，并标明「这是参考音频里的原话，日语参考就保持日语」。不要要求用户把列表里 30 段全部勾完
- 主按钮：「创建音色」。不可用时按钮旁用警告色写明原因（没有名称、没有通过的片段、prompt 转写为空）

创建成功后回到音色库，这条音色可以立刻分配给工程。取消不留半成品档案；这一点保持现在 `createWithVersions` 的事务行为。

录制可以后做，但上传路径必须先通。录制沿用现有试听设备，片段同样进屏幕 A 的列表。

---

## 6. 配音界面（按 GPT-SoVITS 网页的清晰度）

网页版所以好用，是因为一屏就能回答：用哪个权重、参考是什么、参考是什么语言、要合成什么、合成成什么语言、一个按钮。参数多，但有默认值，不挡主路径。SubCue 要的是同一件事，并且参考来自音色库，而不是每次重新选文件。

### 屏幕 1：写稿和生成

左到右三列，默认都在：

1. **文稿。** 句子列表。当前句正文、一句的生成状态（未生成 / 排队 / 完成 / 失败）。失败原因写在该句上。
2. **当前句。** 大文本框就是要念的字。上面一行：音色名、目标语言、试听。下面一个主按钮「合成当前句」，旁边「合成未完成的句子」。
3. **音色。** 只显示音色库里的音色（名称、封面、参考语言、时长）。没有「版本 1 / 版本 2 / 待审核」这种第二层，除非用户展开「更换 prompt」。

引擎不放在五个并列选项里。音色已经决定了「这是克隆音色还是云端成品音色」。克隆音色用本地适配器；成品音色走通义。Mock 只留在测试里，从界面去掉。

主按钮的可用条件等于真正能提交：有文本、有音色、prompt 就绪、引擎已预检。缺什么，按钮禁用，旁边一行警告说明缺什么，并给一个动作（去分析、去预检、去填转写）。

### 屏幕 2：合成参数（折叠）

默认收起。展开后对齐用户熟悉的那一页，并给出已经验证过的默认值：

- 目标语言、参考语言（参考语言来自音色，一般只读，需要时再改）
- 怎么切句（默认按标点，相当于网页的「凑四句一切」这一档；不要把切句方式藏进另一套设置）
- 语速，句间停顿
- 采样：top_k、top_p、temperature。无参考文本的引擎不用这些，就不要显示
- seed
- 「与上一次保持同一套采样」可选，用来减少同一角色前后句音色飘

Index 的情绪和时长倍率只在选了 Index 适配器时出现。备注仍然只保存，不发给模型，避免用户以为写了表演指令就会生效。Qwen Base 今天就不吃自由表演指令，这点要继续讲清楚。

### 屏幕 3：环境（一次配好）

设置里每个适配器一张卡片：

- Qwen：模型目录、预检、状态
- GPT-SoVITS：优先填本地 API 地址（用户已经在用的 v3 网页服务）；没有 API 时再填仓库目录和权重点名（GPT 权重、SoVITS 权重），不要写死 `s1v3.ckpt`
- Index：仓库目录
- 通义：现有云账号，标成「成品音色」，不要和克隆混在同一张卡片

预检结果持久保存，并带上 `modelVersion`。目录没变就不要每次生成前都让用户再点一次检查。目录变了，生成按钮回到「需要重新预检」。

### 和今天界面的关系

`VoiceWorkspace.qml` 里的文稿列表、候选试听、导出 WAV、撤销，可以留。要拆掉或藏起来的是：引擎条上的 Mock、把 Flash 和克隆并列、左栏里第二套声音库、必须先打开设置才能知道为什么不能生成。

---

## 7. 跨语言：日语参考，中文输出

用户的经验是对的：GPT-SoVITS 用日语参考音频和**日语**参考文本，目标文本和目标语言用中文，可以做到没有明显口音。当前 SubCue 的 GPT 路径做不到这件事，原因是代码写死的，不是模型不行。

| 路径 | 现在 | 应改成 |
| --- | --- | --- |
| GPT-SoVITS | 能力表只有中文；Worker 里 `prompt_lang` 和 `text_lang` 都是 `all_zh`；权重固定 v2ProPlus | `prompt_lang=ja`（或分析得到的语言），`text_lang=zh`。参考转写必须是日语原文，不能把日语 ASR 结果翻译成中文再送进去。允许选择 v3 的 GPT / SoVITS 权重，或直接调用用户已经跑起来的网页 API |
| Qwen Base | Worker 已经按 `referenceLanguage` 做 prompt、按目标 `language` 合成，能力表包含日语 | 保留，作为不依赖 GPT-SoVITS 环境的零样本路径。创建音色时把检测语言写入 prompt，不要默认成中文 |
| IndexTTS 2.5 | 参考只传音频，目标语言可 zh/en/ja，不需要转写 | 保留为可选。跨语言效果单独耳测，不把它当成「已等价于 GPT-SoVITS」 |
| 通义 Flash | 不接收本地参考，只有中文成品音色 | 不用于这个需求 |

实现时注意：

- 参考文本语言和目标文本语言是两个字段。`TtsRequest` 里已有 `referenceLanguage` 和 `language`，但 GPT 适配器没有使用它们。
- 日语 prompt 不要走中文 G2P。现在的 GPT 预检把 G2PW、中文 BERT 当成中文生成的必要资源，这只对 `all_zh` 成立。日语参考要走该版本模型自己的日语音素前端；若本地 API 已经做完这件事，就不要在 SubCue 里再做一遍。
- prompt 音频用 6～12 秒清晰日语，比把 90 秒整段塞进一次推理更符合这些模型。90 秒用来挑选这 6～12 秒。
- 同一音色、同一模型版本的 prompt 条件缓存之后，后面的中文句只跑解码，不再读参考 WAV。这是「音色可复用」的实际含义。
- 第一阶段不必为了跨语言先换引擎。先用已经能分开传语言的 Qwen 把「日语参考 + 中文句子」跑通，同时把 GPT 的语言写死列为必须改的缺陷。用户长期要用的质量，以他们自己的 GPT-SoVITS v3 为准，所以 API 适配器要在第二阶段接上，而不是永远停在 v2ProPlus。

不建议把 fish.audio 的云端克隆当成默认后端。用户要的是那套操作，素材和成品都留在本机。若以后要「多段音频训练一个嵌入」，再单开一个可选的云端或微调适配器，不要塞回参考音频向导。

### 引擎取舍

- **留下并修好：Qwen3-TTS Base。** 已经在仓库里，协议完整，适合作为默认零样本引擎。
- **留下并重做适配：GPT-SoVITS。** 优先本地 HTTP API（v3 网页同款参数：文本、文本语言、参考、参考语言、参考文本、语速、top_k、top_p、temperature、切分）。API 不可用时，再用内嵌 Worker，且必须能选权重、能设 `prompt_lang` / `text_lang`。
- **留下为可选：IndexTTS 2.5。** 情绪向量和时长倍率是它独有的，不作为默认跨语言引擎。
- **从克隆流程拿开：通义 Flash。** 继续放在「云端成品音色」。
- **从产品界面拿开：Mock。** 测试继续用。
- **不在这一阶段做：专属微调训练。** 界面上的「训练专属模型」现在是空壳，应先去掉或标成以后的独立功能，避免再出现能点进去但什么都不做的按钮。导入已有 SoVITS/GPT 权重属于 GPT 适配器的「选择权重」，不是另一个向导。

---

## 8. 决策和阶段

### 决策：重建配音子系统，不重写整个应用

整包重写不划算。字幕编辑、打轴、播放、粗剪已经有自己的模型和测试（队列上限、波形、50 次开关媒体、工程格式）。配音是后加的一层，问题集中在音色模型、向导和引擎协议。

只做小修补也不够。现有「一个 version = 一条必须人工确认的参考」和鱼式的「一个音色、一批素材、一条 prompt」不是同一个对象。继续在 `VoiceLibraryDialog.qml` 上加步骤，会把第 4 步、第 5 步叠上去。引擎语言写死、进程每批冷启动、工程和库双份拷贝，也不是改文案能解决的。

所以：播放和字幕不动；`VoiceLibrary` / 向导 / 配音主界面 / TTS 适配按上面的结构换掉。第一阶段允许仍写入今天的 `voices.sqlite`，好让「能创建、能合成」先出现，同时新的 `voice.json` 开始作为音色的真正定义。

### 阶段 1：创建音色到合成一句（先打通）

目标：用户上传若干短音频，点一次创建，得到音色，分配到一句中文（或日语参考 + 中文句子）上，用已配置的 Qwen 合成出 WAV。不引入新引擎。

改动集中在：

1. `createFromSamples` 不再要求每条 `confirmed`。规则改成：选中的、质检没有硬失败的片段都可以入库；转写为空的片段可以入库，但**不能**被选为 prompt。若引擎要转写，只检查 prompt 那一条。
2. 没有任何可用 prompt 时，不要静默返回。用警告色列出：第几条、文件名、原因（未分析、全静音、转写为空、解码失败）。通过的条目照样保留。
3. `confirmCreationSample`、`editSample`、语言下拉、选中勾选框的回写，在新界面落地前先修成：写模型一定 `emit changed()`；失败一定写 `statusText`；勾选框只显示模型里的值，不用「点击后打断绑定」。
4. 第 3 步按钮在模型不满足时直接禁用，禁用原因写在按钮上，而不是点下去才冒出一行灰字。
5. 创建成功后，现有 `generateLocal` 能选中这个默认版本并合成。日语参考要把 `referenceLanguage` 写成 `Japanese`，目标语言写成 `Chinese`，走已经实现的 Qwen 分支。
6. 测试：改 `voiceCreationDraftIsReviewedAtomicAndCancellable`，覆盖「未确认但质检通过可以创建」「空转写不能当 prompt」「失败条目有明确原因」。QML 测试改为走新的启用条件和错误文案，而不是手动 `invokeMethod("toggled")`。

这一阶段结束时，截图里的按钮要么创建成功并离开向导，要么明确告诉用户哪一条不行。

### 阶段 2：换成两屏收集 + 一屏资料

用第 5 节的屏幕 A/B/C 换掉「参考音频创建 · n / 3」。旧向导删除，避免两套入口。批量分析复用 `AudioQuality` 和本地 ASR，一条失败不影响其它条。总时长条按 10 / 30 / 90 秒。创建仍走阶段 1 的库写入。

同时删掉「训练专属模型」「导入已有模型」这两个空页面。权重选择放到阶段 3 的 GPT 卡片里。

### 阶段 3：配音主界面和 GPT-SoVITS

- 按第 6 节收束 `VoiceWorkspace`：主路径只有文稿、当前句、音色、一个合成按钮。
- 增加 GPT-SoVITS 本地 API 适配器，参数与 v3 网页对齐，`prompt_lang` / `text_lang` 分开。
- 内嵌 Worker 去掉 `all_zh` 和写死的 ckpt 路径；预检改为用户选中的权重文件。
- 通义 Flash 移出克隆引擎列表。Mock 移出界面。
- 用日语参考、中文台词做一条耳测样例，和用户现有的网页结果对比。不通过就不把 GPT 标成默认跨语言引擎。

### 阶段 4：长期 Worker 和 prompt 缓存

- 每个引擎一个常驻进程，空闲再退出。
- prompt 条件按第 4.3 节落盘，第二次合成同一音色不再读 WAV、不再重新编码。
- 句级任务状态和「部分成功」写进界面。
- 工程改为 pin `voiceId + promptHash`，嵌入工程做成显式动作。
- ASR 分析缓存加容量上限。
- 取消和销毁测试：退出后没有残留 Worker、临时目录和迟到回调。缓存字节数可查询。

### 不做的事

- 不把模型权重拷进仓库、构建目录或安装包。
- 不在应用里自动下载模型。GPT-SoVITS 的环境仍由用户按现有网页方式准备，SubCue 只连接。
- 不用整段 90 秒当单次 prompt。
- 不把 fish.audio 的服务端协议搬进来。学的是操作顺序和「一条不合格不拖垮整批」。

---

## 9. 验收

阶段 1 就可以用下面几条判断有没有做对：

1. 打开向导，一次加入多段 `梅比乌斯音频1.flac` 这类短素材，填写名称，点「创建音色」。要么音色出现在库里，要么屏幕上列出具体哪一条被拒绝。
2. 不再出现「按钮可点、点完仍停在原页、只有一行灰色说明」。
3. 把该音色分配给一句中文，Qwen 环境已预检时，「合成当前句」产出可试听的 WAV。
4. 参考语言为日语时，发给 Qwen 的 `referenceLanguage` 是 `Japanese`，目标 `language` 是 `Chinese`，参考文本仍是日语。
5. 创建过程没有启动 TTS 模型。分析过程如果用了 ASR，取消后 Worker 和临时目录要清掉。
