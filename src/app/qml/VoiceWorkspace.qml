import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: workspace


    objectName: "voiceWorkspace"
    property var controller: typeof voiceStudio === "undefined" ? null : voiceStudio
    property var voices: typeof voiceLibrary === "undefined" ? null : voiceLibrary
    property var previewPlayer: typeof audioPreview === "undefined" ? null : audioPreview
    property var audio: typeof voiceAudio === "undefined" ? null : voiceAudio
    signal requestVoiceLibrary()
    signal requestVoiceCreation()
    property bool externalModalEditing: false
    readonly property bool generating: typeof ttsTasks !== "undefined" && ttsTasks !== null && ttsTasks.busy
    readonly property bool storageBusy: typeof projectSession !== "undefined" && projectSession && (projectSession.busy || projectSession.savePending)
    readonly property bool flashEditing: flashDialog.visible || flashBudget.activeFocus
    readonly property bool modalEditing: externalModalEditing || textDialog.visible || changesDialog.visible || importDialog.visible || exportDialog.visible || cropDialog.visible || defaultVoiceDialog.visible || generationRangeDialog.visible || historyDialog.visible || relinkDialog.visible || wavOptions.visible || sentenceFolderDialog.visible || wavDialog.visible || staleWavDialog.visible || flashDialog.visible || fileMenu.visible || documentMenu.visible || sentenceMenu.visible || viewMenu.visible
    readonly property bool textEditing: (workspace.Window.window && workspace.Window.window.activeFocusItem && typeof workspace.Window.window.activeFocusItem.inputMethodComposing !== "undefined") || bodyEditor.activeFocus || spokenEditor.activeFocus || notesEditor.activeFocus || targetLanguage.activeFocus || pastedText.activeFocus || seedField.activeFocus || speedField.activeFocus || durationField.activeFocus || emotionStrength.activeFocus || emotionChoice.activeFocus || gainField.activeFocus || pauseField.activeFocus || trimStart.activeFocus || trimEnd.activeFocus || pythonPath.activeFocus || qwenModelPath.activeFocus || localEngine.activeFocus || generationScope.activeFocus || voiceScope.activeFocus || flashEditing
    readonly property var commandActions: [voicePlayAction, voicePreviousAction, voiceNextAction, voiceEditAction, voiceGenerateAction, voiceUndoAction, voiceRedoAction]
    property string draftId: ""
    property string draftError: ""
    property bool loadingDraft: false
    property bool draftDirty: false
    property bool indexParametersDirty: false
    property bool gptParametersDirty: false
    property bool targetLanguageDirty: false
    property string textOperation: "append"
    property bool targetedImport: false
    property string cropCandidate: ""
    property string selectedCloudVoiceKey: ""
    property var favoriteVoiceKeys: []
    property string requestedVoiceScope: "selected"
    property bool voicesCollapsed: false
    property bool candidatesExpanded: false
    property bool sequenceCollapsed: false
    property bool inspectorCollapsed: false
    property bool engineExpanded: false
    property bool cancelRequested: false
    property string auditionSource: qsTr("当前句 · 已应用音频")
    function voiceRank(version) {
        const metadata = version.metadata || {}
        const compatible = localEngine.currentIndex === 2 ? !!metadata.providerVoiceId : !metadata.providerVoiceId
        const reviewed = !!metadata.providerVoiceId || metadata.qualityState === "approved"
        const language = metadata.referenceLanguage || (metadata.preset ? metadata.preset.language : "")
        return (compatible ? 0 : 8) + (reviewed ? 0 : 4) + (language === targetLanguage.values[targetLanguage.currentIndex] ? 0 : 1)
    }
    function cloudConnectionKey() {
        if (typeof editor === "undefined" || !editor.cloudSettings) return ""
        const cloud = editor.cloudSettings
        const binding = cloud.bindings.find(function(row) { return row.taskId === "speechSynthesis" }) || {}
        const model = cloud.models.find(function(row) { return row.id === binding.target }) || {}
        const account = cloud.accounts.find(function(row) { return row.id === model.accountId }) || {}
        return [account.id || "", account.region || "", model.workspace || account.workspace || "", model.remoteId || ""].join("/")
    }
    function cloudVoiceRank(row) {
        if (!row.compatible) return 4
        if (row.project) return 0
        if (favoriteVoiceKeys.indexOf((row.scope || cloudConnectionKey()) + "/" + row.id) >= 0) return 1
        return row.builtin ? 3 : 2
    }
    readonly property var cloudVoiceChoices: {
        if (!voices) return []
        const key = cloudConnectionKey()
        const project = controller ? controller.projectVoices.map(function(row) {
            const scope = [row.cloudAccountId, row.region, row.businessSpace, row.model].join("/")
            const builtin = voices.builtinVoices().find(function(item) { return item.id === row.providerVoiceId }) || {}
            return {id: row.providerVoiceId, versionId: row.versionId, displayName: row.name, name: row.name + qsTr(" · 项目已使用"), scope: scope, project: true, builtin: !!builtin.id, compatible: scope === key, reason: qsTr("项目快照与当前账号或型号不匹配"), hasPreview: !!builtin.previewUrl, description: builtin.description || ""}
        }) : []
        const builtin = voices.builtinVoices().map(function(row) { return {id: row.id, name: row.name + qsTr(" · 系统"), builtin: true, compatible: true, hasPreview: !!row.previewUrl, description: row.description} })
        const custom = (voices.voiceCatalog.rows || []).map(function(row) { return Object.assign({}, row, {name: row.name + " · " + row.source + (row.compatible ? "" : " · " + row.reason)}) })
        return project.concat(custom, builtin).filter(function(row, index, rows) {
            return (showUnavailableVoices.checked || row.compatible) && row.name.toLowerCase().indexOf(cloudVoiceSearch.text.toLowerCase()) >= 0 && rows.findIndex(function(other) { return other.id === row.id && (other.scope || key) === (row.scope || key) }) === index
        }).sort(function(a, b) { return cloudVoiceRank(a) - cloudVoiceRank(b) })
    }
    readonly property var selectedCloudVoice: builtinVoice.currentIndex >= 0 && builtinVoice.currentIndex < cloudVoiceChoices.length ? cloudVoiceChoices[builtinVoice.currentIndex] : ({})
    function localVoiceUnavailableReason(row) {
        const metadata = row.metadata || {}
        if (metadata.providerVoiceId) return qsTr("属于通义云端")
        if (metadata.qualityState !== "approved") return qsTr("参考素材尚未确认")
        if (capabilities.transcriptRequired && !(metadata.referenceText || "").trim()) return qsTr("参考文字尚未确认")
        if (metadata.referenceLanguage && capabilities.languages.indexOf(metadata.referenceLanguage) < 0) return qsTr("当前引擎不支持参考语言")
        return ""
    }
    readonly property var suggestedVersions: voices ? voices.versions.filter(function(row) { return showUnavailableLocal.checked || !localVoiceUnavailableReason(row) }).sort(function(a, b) { return voiceRank(a) - voiceRank(b) }) : []
    readonly property var capabilities: controller ? controller.engineCapabilities(localEngine.currentIndex === 1 ? "mock" : localEngine.currentIndex === 2 ? "cloud" : localEngine.engineKey) : ({languages: ["Chinese"], parameters: []})
    readonly property var readiness: {
        if (!controller) return ({text: "", action: ""})
        if (typeof editor !== "undefined" && editor.cloudSettings) { editor.cloudSettings.accounts; editor.cloudSettings.bindings }
        controller.statusText; controller.selectedId; controller.defaultVoiceVersionId; generating; storageBusy
        return controller.generationReadiness(localEngine.currentIndex === 1 ? "mock" : localEngine.currentIndex === 2 ? "cloud" : localEngine.engineKey, pythonPath.text, qwenModelPath.text)
    }
    property real voiceRatio: 0.3
    property real inspectorRatio: 0.3
    onHeightChanged: if (height > 0 && height < 650 && voiceSequencePanel) voiceSequencePanel.SplitView.preferredHeight = 96
    onWidthChanged: {
        if (width >= 900) inspectorDrawer.close()
    }
    onInspectorCollapsedChanged: {
        if (viewSaveDelay) viewSaveDelay.restart()
        if (width < 900) { if (!inspectorCollapsed) inspectorDrawer.open(); else inspectorDrawer.close() }
    }
    Drawer {
        id: inspectorDrawer; objectName: "voiceInspectorDrawer"
        edge: Qt.RightEdge; width: Math.min(350, workspace.width - 48); height: workspace.height
        parent: workspace; modal: false
        background: Rectangle { color: Theme.panel }
        onClosed: if (workspace.width < 1000) inspectorCollapsed = true
    }
    onGeneratingChanged: if (!generating) cancelRequested = false
    Component.onCompleted: { restoreSession(); loadDraft(); loadEngineSettings(); if (audio) audio.setActive(visible); if (voices) voices.refresh(voices.selectedProfile) }
    function loadEngineSettings() {
        if (typeof editor === "undefined" || !pythonPath || !qwenModelPath) return
        const config = (editor.defaultSetting("ttsEngines") || {})[localEngine.currentIndex === 1 ? "mock" : localEngine.engineKey] || {}
        pythonPath.text = config.python || ""
        qwenModelPath.text = config.directory || ""
    }
    Connections { target: typeof editor === "undefined" ? null : editor; function onSettingsChanged() { loadEngineSettings() } }
    onVisibleChanged: { if (audio) audio.setActive(visible); if (!visible && previewPlayer) previewPlayer.clear(); if (visible && controller && controller.preview.length > 0) changesDialog.open() }
    function viewState() { return {layoutVersion: 2, engine: localEngine.currentIndex, voicesCollapsed: voicesCollapsed, inspectorCollapsed: inspectorCollapsed, sequenceCollapsed: sequenceCollapsed, voicesWidth: voicePickerPanel.width, inspectorWidth: voiceInspectorSlot.width, voiceRatio: voiceRatio, inspectorRatio: inspectorRatio, selectedId: controller ? controller.selectedId : "", favorites: favoriteVoiceKeys} }
    Timer { id: viewSaveDelay; interval: 400; onTriggered: if (controller) controller.persistSessionState(viewState()) }
    onVoicesCollapsedChanged: if (viewSaveDelay) viewSaveDelay.restart()
    onSequenceCollapsedChanged: if (viewSaveDelay) viewSaveDelay.restart()
    function captureSession() {
        if (controller) controller.setSessionState(viewState())
    }
    function restoreSession() {
        if (!controller) return
        const state = controller.sessionState || {}
        favoriteVoiceKeys = Array.isArray(state.favorites) ? state.favorites.slice(0, 500) : []
        if (state.engine !== undefined) { localEngine.currentIndex = Math.max(0, Math.min(4, state.engine)); localEngine.acceptedIndex = localEngine.currentIndex; loadEngineSettings() }
        if (state.voicesCollapsed !== undefined) voicesCollapsed = state.voicesCollapsed
        if (state.inspectorCollapsed !== undefined) inspectorCollapsed = state.inspectorCollapsed
        if (state.sequenceCollapsed !== undefined) sequenceCollapsed = state.sequenceCollapsed
        if (state.layoutVersion === 2 && state.voiceRatio > 0) voiceRatio = Math.max(0.2, Math.min(0.4, state.voiceRatio))
        if (state.layoutVersion === 2 && state.inspectorRatio > 0) inspectorRatio = Math.max(0.2, Math.min(0.4, state.inspectorRatio))
        if (state.selectedId && controller.segmentIndex(state.selectedId) >= 0) controller.select(state.selectedId)
    }
    function recoveryFields() {
        if (!draftDirty || !draftId) return ({})
        if (bodyEditor.inputMethodComposing || spokenEditor.inputMethodComposing || notesEditor.inputMethodComposing) return null
        return { id: draftId, text: bodyEditor.text, spoken: spokenEditor.text, manual: manualSpoken.checked, notes: notesEditor.text,
            language: targetLanguage.currentIndex, seed: seedField.text, duration: durationField.text, speed: speedField.text, emotion: emotionChoice.currentIndex,
            strength: emotionStrength.text, gain: gainField.text, pause: pauseField.text, start: trimStart.text, end: trimEnd.text,
            indexDirty: indexParametersDirty, gptDirty: gptParametersDirty, languageDirty: targetLanguageDirty }
    }
    function restoreRecoveryFields(fields) {
        if (!fields || !fields.id || !controller || controller.segmentIndex(fields.id) < 0) return
        controller.select(fields.id); loadDraft(); loadingDraft = true
        bodyEditor.text = fields.text; spokenEditor.text = fields.spoken; manualSpoken.checked = fields.manual; notesEditor.text = fields.notes
        targetLanguage.currentIndex = fields.language; seedField.text = fields.seed; durationField.text = fields.duration
        speedField.text = fields.speed === undefined ? "1" : fields.speed
        emotionChoice.currentIndex = fields.emotion; emotionStrength.text = fields.strength; gainField.text = fields.gain
        pauseField.text = fields.pause; trimStart.text = fields.start; trimEnd.text = fields.end
        loadingDraft = false; draftDirty = true; indexParametersDirty = fields.indexDirty; gptParametersDirty = !!fields.gptDirty; targetLanguageDirty = fields.languageDirty
    }
    function cycleFocus(direction) {
        if (direction > 0 && bodyEditor.activeFocus) { inspectorCollapsed = false; inspectorTabs.currentIndex = 0; targetLanguage.forceActiveFocus() }
        else if (manuscript.activeFocus) bodyEditor.forceActiveFocus()
        else manuscript.forceActiveFocus()
    }
    function openMediaDialog() { openScriptDialog() }
    function openScriptDialog() { if (commitPending()) { targetedImport = false; importDialog.open() } }
    function selectedVoiceName() {
        if (voices) for (const profile of voices.profiles) if (profile.id === voices.selectedProfile) return profile.name
        return qsTr("项目音色")
    }
    function assignVoice(versionId, scope, name) {
        if (scope === "default" && versionId !== controller.defaultVoiceVersionId && controller.inheritedVoiceCount() > 0) {
            defaultVoiceDialog.versionId = versionId; defaultVoiceDialog.voiceName = name
            defaultVoiceDialog.affected = controller.inheritedVoiceCount(); defaultVoiceDialog.open()
        } else controller.bindVoiceVersion(versionId, scope, name)
    }
    function requestExport() { if (!storageBusy && commitPending()) { wavOptions.summary = controller.exportSummary(); wavOptions.open() } }
    function appendText() { if (commitPending()) { textOperation = "append"; pastedText.text = ""; textDialog.open() } }
    function navigate(direction, pending) {
        if (!controller || !commitPending()) return
        const id = controller.adjacentFilteredId(direction, pending || false)
        if (id !== "") choose(id)
    }
    function generate(scope) {
        if (!controller || controller.modifying || storageBusy || generating || !draftId || !commitPending()) return
        if (localEngine.currentIndex !== 1 && !controller.selected.effectiveVoiceVersionId) {
            voicesCollapsed = false
            draftError = qsTr("先选择音色并分配给当前句；旁白可选“全部未绑定句子”一次分配。没有音色时，点击音色分配区的“管理…”创建并审核参考音频。")
            if (voices) voices.refresh(voices.selectedProfile)
            return
        }
        candidatesExpanded = true
        if (localEngine.currentIndex === 0 || localEngine.currentIndex >= 3) controller.generateLocal(localEngine.engineKey, pythonPath.text, qwenModelPath.text, scope, scope !== "selected")
        else if (localEngine.currentIndex === 2) controller.prepareFlash(scope, Number(flashBudget.text), flashRegenerate.checked)
        else controller.generateMock(pythonPath.text, scope, scope !== "selected")
        if (!controller.modifying && !generating) { draftError = controller.statusText; engineExpanded = true; inspectorTabs.currentIndex = 0 }
    }
    function toggleAudition() {
        if (!previewPlayer || !audio) return
        if (previewPlayer.loading) { audio.stop(); return }
        if (previewPlayer.playing) { previewPlayer.pause(); return }
        if (previewPlayer.paused) { previewPlayer.resume(); return }
        if (!controller || audio.busy || storageBusy || !commitPending()) return
        if (controller.selected.selectedTake && controller.selected.selectedTake.id) { auditionSource = qsTr("当前句 · 已应用音频"); audio.prepare(true); return }
        draftError = qsTr("当前句还没有音频。先生成候选，试听后应用，再导出。")
        candidatesExpanded = true
    }
    function loadDraft() {
        if (!controller) return
        const item = controller.selected
        loadingDraft = true
        draftId = controller.selectedId
        bodyEditor.text = item.text || ""
        spokenEditor.text = item.manualSpoken ? item.spoken || "" : item.effectiveSpoken || ""
        manualSpoken.checked = !!item.manualSpoken
        notesEditor.text = item.notes || ""
        targetLanguage.currentIndex = targetLanguage.values.indexOf(item.language || "Chinese")
        seedField.text = item.parameters && item.parameters.seed !== undefined ? String(item.parameters.seed) : ""
        durationField.text = item.parameters && item.parameters.durationFactor !== undefined ? String(item.parameters.durationFactor) : "1"
        speedField.text = item.parameters && item.parameters.speedFactor !== undefined ? String(item.parameters.speedFactor) : "1"
        emotionChoice.currentIndex = Math.max(0, emotionChoice.values.indexOf(item.parameters ? item.parameters.emotion || "" : ""))
        emotionStrength.text = item.parameters && item.parameters.emotionStrength !== undefined ? String(item.parameters.emotionStrength) : "0.3"
        gainField.text = String(item.gain === undefined ? 1 : item.gain)
        pauseField.text = String(item.pauseMs || 0)
        trimStart.text = item.selectedTake ? item.selectedTake.trimStart || "0" : "0"
        trimEnd.text = item.selectedTake ? item.selectedTake.trimEnd || "" : ""
        loadingDraft = false; draftDirty = false; indexParametersDirty = false; gptParametersDirty = false; targetLanguageDirty = false; draftError = ""
    }
    function commitPending() {
        if (!controller) return true
        Qt.inputMethod.commit()
        if (bodyEditor.inputMethodComposing || spokenEditor.inputMethodComposing || notesEditor.inputMethodComposing) { draftError = qsTr("请先确认输入法候选，再应用编辑。"); return false }
        if (!draftDirty || !draftId) return true
        if (!gainField.acceptableInput || !pauseField.acceptableInput) { draftError = qsTr("增益须为 0～4，句后停顿须为 0～60000 毫秒。"); inspectorCollapsed = false; inspectorTabs.currentIndex = 1; return false }
        if (seedField.text.trim() !== "" && !seedField.acceptableInput) { draftError = qsTr("随机种子须为 0～%1 的整数，或留空。").arg(localEngine.currentIndex === 2 ? 65535 : 2147483647); inspectorCollapsed = false; inspectorTabs.currentIndex = 0; return false }
        if (indexParametersDirty && (!durationField.acceptableInput || (emotionChoice.currentIndex > 0 && (!emotionStrength.acceptableInput || Number(emotionStrength.text) <= 0)))) { draftError = qsTr("时长倍率须为 0.5～2，情绪强度须大于 0 且不超过 1。"); inspectorCollapsed = false; inspectorTabs.currentIndex = 0; return false }
        if (gptParametersDirty && !speedField.acceptableInput) { draftError = qsTr("语速倍率须为 0.5～2，小于 1 更慢。"); inspectorCollapsed = false; inspectorTabs.currentIndex = 0; return false }
        const draft = {id: draftId, text: bodyEditor.text, spoken: spokenEditor.text, manualSpoken: manualSpoken.checked, notes: notesEditor.text,
            gain: Number(gainField.text), pauseMs: Number(pauseField.text), trimStart: trimStart.text, trimEnd: trimEnd.text, seed: seedField.text,
            indexParameters: indexParametersDirty, durationFactor: Number(durationField.text), emotion: emotionChoice.values[emotionChoice.currentIndex], emotionStrength: Number(emotionStrength.text),
            gptParameters: gptParametersDirty, speedFactor: Number(speedField.text)}
        if (targetLanguageDirty) draft.language = targetLanguage.values[targetLanguage.currentIndex]
        const accepted = controller.commitDraft(draft)
        if (accepted) { draftDirty = false; loadDraft() }
        else draftError = controller.statusText
        return accepted
    }
    function editKey(event, field) {
        if (field.inputMethodComposing) return
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
            if (commitPending()) manuscript.forceActiveFocus()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            loadDraft(); manuscript.forceActiveFocus(); event.accepted = true
        }
    }
    function choose(id) { if (commitPending()) { controller.select(id); manuscript.currentIndex = controller.filteredIndex(id); if (manuscript.currentIndex >= 0) manuscript.positionViewAtIndex(manuscript.currentIndex, ListView.Contain); sequence.positionViewAtIndex(controller.segmentIndex(id), ListView.Contain) } }
    Connections {
        target: controller
        function onDocumentReset() { restoreSession(); loadDraft() }
        function onSelectionChanged() { manuscript.currentIndex = controller.filteredIndex(controller.selectedId); if (!workspace.draftDirty) workspace.loadDraft() }
        function onPreviewChanged() { if (workspace.visible && controller.preview.length > 0) changesDialog.open() }
        function onGenerationPreviewChanged() { if (workspace.visible && controller.generationPreview.needed !== undefined) generationRangeDialog.open(); else if (generationRangeDialog.visible) generationRangeDialog.close() }
        function onChanged() { if (historyDialog.restoreCandidates && !controller.modifying) { historyDialog.restoreCandidates = false; controller.loadCandidates() } }
        function onAuditionRequested(path, sha256) { if (workspace.visible && previewPlayer) previewPlayer.play(path, sha256) }
    }
    Connections {
        target: voices
        function onAuditionRequested(path, sha256) { if (workspace.visible && !externalModalEditing && previewPlayer) previewPlayer.play(path, sha256) }
        function onVoiceSelected(versionId, name) { if (controller && workspace.visible && !externalModalEditing) assignVoice(versionId, requestedVoiceScope, name) }
    }
    Connections {
        target: audio
        function onStaleExportRequested(file, normalize, sentences) { staleWavDialog.file = file; staleWavDialog.normalize = normalize; staleWavDialog.sentences = sentences; staleWavDialog.open() }
    }
    Connections {
        target: previewPlayer
        function onChanged() {
            if (!workspace.visible || !audio || !audio.ownsPlayback() || !controller || !previewPlayer.playing || workspace.textEditing || workspace.draftDirty || workspace.modalEditing) return
            const rows = audio.rows
            let first = 0, last = rows.length - 1
            while (first <= last) {
                const middle = Math.floor((first + last) / 2), row = rows[middle]
                const start = Number(row.position) * 1000000 / 48000
                const end = start + (Number(row.end) - Number(row.start) + Number(row.pause)) * 1000000 / 48000
                if (previewPlayer.positionUs >= start && previewPlayer.positionUs < end) {
                    if (controller.selectedId !== row.id) { controller.select(row.id); const index = controller.filteredIndex(row.id); if (index >= 0) manuscript.positionViewAtIndex(index, ListView.Contain); sequence.positionViewAtIndex(controller.segmentIndex(row.id), ListView.Contain) }
                    break
                }
                if (previewPlayer.positionUs < start) last = middle - 1
                else first = middle + 1
            }
        }
    }
                        TextArea {
                            textFormat: TextEdit.PlainText; id: bodyEditor; objectName: "voiceBodyEditor"
                            parent: manuscript.currentItem ? manuscript.currentItem.editorHost : workspace
                            visible: draftId !== "" && manuscript.currentItem !== null; width: parent ? parent.width : 0; height: workspace.height < 650 ? 56 : 84
                            selectByMouse: true; wrapMode: TextEdit.Wrap; placeholderText: qsTr("当前句正文"); enabled: draftId !== "" && controller && !controller.selected.locked; font.pixelSize: 16
                            onTextChanged: if (!loadingDraft) draftDirty = true
                            Keys.onPressed: function(event) { workspace.editKey(event, bodyEditor) }
                        }
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 10; spacing: 6
            MenuButton {
                text: qsTr("文件 ▾")
                onClicked: fileMenu.popup(this, 0, height)
                SubMenu {
                    id: fileMenu
                    MenuItem { action: workspace.Window.window && workspace.Window.window.newAction ? workspace.Window.window.newAction : null }
                    MenuItem { action: workspace.Window.window && workspace.Window.window.openAction ? workspace.Window.window.openAction : null }
                    MenuSeparator {}
                    MenuItem { action: workspace.Window.window && workspace.Window.window.saveAction ? workspace.Window.window.saveAction : null }
                    MenuItem { action: workspace.Window.window && workspace.Window.window.saveAsAction ? workspace.Window.window.saveAsAction : null }
                    MenuSeparator {}
                    MenuItem { text: qsTr("导出音频…"); enabled: audio && !audio.busy && manuscript.count > 0; onTriggered: requestExport() }
                }
            }
            SubButton { text: qsTr("添加文稿…"); enabled: controller && !controller.modifying; onClicked: appendText() }
            SubButton { text: qsTr("导入…"); enabled: controller && !controller.modifying; onClicked: openScriptDialog() }
            SubButton { action: workspace.Window.window && workspace.Window.window.saveAction ? workspace.Window.window.saveAction : null; text: qsTr("保存") }
            MenuButton {
                text: qsTr("文稿 ▾"); enabled: controller && !controller.modifying
                onClicked: documentMenu.popup(this, 0, height)
                SubMenu {
                    id: documentMenu
                    MenuItem { text: qsTr("从字幕引用…"); onTriggered: if (commitPending()) controller.previewSubtitleImport() }
                    MenuItem { text: qsTr("从粗剪导入…"); onTriggered: if (commitPending()) controller.previewRoughCutImport() }
                    MenuSeparator {}
                    MenuItem { text: qsTr("JSON 定向更新…"); onTriggered: if (commitPending()) { targetedImport = true; importDialog.open() } }
                    MenuItem { text: qsTr("导出文稿 JSON…"); enabled: manuscript.count > 0; onTriggered: if (commitPending()) exportDialog.open() }
                    MenuItem { text: qsTr("朗读替换表…"); onTriggered: if (commitPending()) { textOperation = "replacements"; pastedText.text = controller.replacementRules().map(function(rule) { return rule.from + "=" + rule.to }).join("\n"); textDialog.open() } }
                    MenuSeparator {}
                    MenuItem { text: qsTr("归档句子与历史…"); onTriggered: if (commitPending()) { historyDialog.offset = 0; historyDialog.rows = controller.archivedSegments(0); historyDialog.open() } }
                    MenuItem { text: qsTr("批量替换已应用版本…"); enabled: controller && !controller.modifying; onTriggered: if (commitPending()) controller.applySafeCandidates(true) }
                    MenuItem { text: qsTr("重新生成当前段"); enabled: controller && !controller.modifying && !generating; onTriggered: generate("paragraph") }
                    MenuItem { text: qsTr("重新生成全篇"); enabled: controller && !controller.modifying && !generating; onTriggered: generate("all") }
                    MenuSeparator {}
                    MenuItem { action: voiceUndoAction }
                    MenuItem { action: voiceRedoAction }
                }
            }
            SubButton { action: voiceUndoAction; visible: workspace.width >= 1000 }
            SubButton { action: voiceRedoAction; visible: workspace.width >= 1000 }
            Item { Layout.fillWidth: true }
            SubButton { text: qsTr("选择音色…"); enabled: voices && !voices.busy; onClicked: { voicesCollapsed = false; if (voices) voices.refresh(voices.selectedProfile) } }
            MenuButton {
                text: qsTr("面板 ▾")
                onClicked: viewMenu.popup(this, 0, height)
                SubMenu {
                    id: viewMenu
                    MenuItem { text: qsTr("恢复默认布局"); onTriggered: { voicesCollapsed = false; inspectorCollapsed = false; sequenceCollapsed = false; voiceRatio = 0.3; inspectorRatio = 0.3; voicePickerPanel.SplitView.preferredWidth = Qt.binding(function() { return (voiceWorkbenchSplit.width - 12) * workspace.voiceRatio }); voiceInspectorSlot.SplitView.preferredWidth = Qt.binding(function() { return (voiceWorkbenchSplit.width - 12) * workspace.inspectorRatio }); viewSaveDelay.restart() } }
                    MenuItem { text: qsTr("音色分配"); checkable: true; checked: !voicesCollapsed; onTriggered: { voicesCollapsed = !voicesCollapsed; if (!voicesCollapsed && voices) voices.refresh(voices.selectedProfile) } }
                    MenuItem { text: qsTr("底部编排"); checkable: true; checked: !sequenceCollapsed; onTriggered: sequenceCollapsed = !sequenceCollapsed }
                    MenuItem { text: qsTr("检查区"); checkable: true; checked: !inspectorCollapsed; onTriggered: inspectorCollapsed = !inspectorCollapsed }
                }
            }
            CloudTaskMenu { cloud: editor.cloudSettings; tasks: [{id: "speechSynthesis", name: "语音合成"}] }
            SubButton { text: qsTr("导出音频…"); enabled: audio && !audio.busy && !storageBusy && manuscript.count > 0; onClicked: requestExport() }
        }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: generationBar.implicitHeight + 16
            color: Theme.panelHeader
            RowLayout {
                id: generationBar
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; anchors.topMargin: 8; anchors.bottomMargin: 8; spacing: 8
                Label { text: qsTr("引擎"); color: Theme.secondaryText }
                SubComboBox {
                    id: localEngine; objectName: "voiceEngineSelector"
                    Layout.preferredWidth: 195; Layout.minimumWidth: 150; Layout.maximumWidth: 240
                    model: [qsTr("Qwen Base · 本地"), qsTr("Mock · 测试音"), qsTr("通义 Flash · 云端付费"), qsTr("IndexTTS 2.5 · 本地"), qsTr("GPT-SoVITS · 本地")]
                    readonly property string engineKey: currentIndex === 3 ? "index" : currentIndex === 4 ? "gpt" : "qwen"
                    property int acceptedIndex: 0
                    onActivated: {
                        const requested = currentIndex
                        currentIndex = acceptedIndex
                        if (!commitPending()) return
                        acceptedIndex = requested; currentIndex = requested; loadEngineSettings(); viewSaveDelay.restart()
                    }
                }
                SubButton {
                    objectName: "voiceEngineSettingsButton"
                    text: engineExpanded ? qsTr("收起配置") : qsTr("配置…")
                    onClicked: if (workspace.Window.window) workspace.Window.window.openSettings(localEngine.currentIndex === 2 ? "cloud" : "voice", localEngine.currentIndex === 2 ? "" : "settingsVoicePython")
                }
                Item { Layout.fillWidth: true }
                Label { visible: workspace.width >= 1000; text: qsTr("范围"); color: Theme.secondaryText }
                SubComboBox { id: generationScope; visible: workspace.width >= 1000; objectName: "voiceGenerationScope"; Layout.preferredWidth: 118; model: [qsTr("当前句"), qsTr("当前段"), qsTr("全部句子"), qsTr("缺失或失效句"), qsTr("选中句")] }
                PrimaryButton {
                    objectName: "voiceGenerateButton"
                    text: generating ? (cancelRequested ? qsTr("取消中…") : qsTr("生成中…")) : qsTr("生成%1").arg(workspace.width < 1000 ? qsTr("当前句") : generationScope.currentText)
                    shortcutHint: voiceGenerateAction.keySequence
                    enabled: controller && !controller.modifying && !storageBusy && !generating && draftId !== "" && (generationScope.currentIndex !== 0 || !controller.selected.locked)
                    onClicked: generate(workspace.width < 1000 ? "selected" : ["selected", "paragraph", "all", "invalid", "selection"][generationScope.currentIndex])
                }
                SubButton { objectName: "voiceGenerateMissingButton"; text: qsTr("补齐缺失 / 失效"); enabled: controller && !controller.modifying && !storageBusy && !generating && manuscript.count > 0; onClicked: generate("invalid") }
                SubButton { text: cancelRequested ? qsTr("取消中…") : qsTr("取消生成"); visible: generating; enabled: !cancelRequested; onClicked: { cancelRequested = true; ttsTasks.cancel() } }
            }
        }
        Label {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: 4
            visible: workspace.height >= 650 || readiness.action === ""
            text: readiness.text; color: readiness.state === "ready" ? Theme.secondaryText : Theme.warning; wrapMode: Text.Wrap
        }
        RowLayout {
            visible: readiness.action !== "" && (workspace.height >= 650 || readiness.action === "check" || readiness.action === "parameters"); Layout.leftMargin: 12; Layout.bottomMargin: 4
            SubButton {
                objectName: "voiceReadinessAction"
                enabled: readiness.action !== "parameters" || (controller && !controller.selected.locked && !controller.modifying && !storageBusy && !generating)
                text: readiness.action === "text" ? qsTr("添加文稿…") : readiness.action === "voice" ? qsTr("选择音色…") : readiness.action === "check" ? qsTr("检查当前环境") : readiness.action === "parameters" ? qsTr("清除当前句不兼容参数") : localEngine.currentIndex === 2 ? qsTr("连接 AI 服务…") : qsTr("打开引擎设置")
                onClicked: {
                    if (readiness.action === "text") appendText()
                    else if (readiness.action === "voice") { voicesCollapsed = false; if (voices) voices.refresh(voices.selectedProfile) }
                    else if (readiness.action === "check") controller.checkLocal(localEngine.engineKey, pythonPath.text, qwenModelPath.text)
                    else if (readiness.action === "parameters") { if (commitPending()) controller.clearUnsupportedParameters(localEngine.currentIndex === 1 ? "mock" : localEngine.currentIndex === 2 ? "cloud" : localEngine.engineKey) }
                    else if (workspace.Window.window) workspace.Window.window.openSettings(localEngine.currentIndex === 2 ? "cloud" : "voice", localEngine.currentIndex === 2 ? "" : "settingsVoicePython")
                }
            }
        }
        Label {
            objectName: "voiceDraftError"
            visible: draftError !== ""; Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: 6
            textFormat: Text.PlainText; text: draftError; wrapMode: Text.Wrap; color: Theme.danger
        }

        SplitView {
            Layout.fillWidth: true; Layout.fillHeight: true
            orientation: Qt.Vertical
            SplitView {
                id: voiceWorkbenchSplit
                onResizingChanged: if (!resizing && !voicesCollapsed && !inspectorCollapsed) {
                    voiceRatio = voicePickerPanel.width / Math.max(1, width - 12)
                    inspectorRatio = voiceInspectorSlot.width / Math.max(1, width - 12)
                    voicePickerPanel.SplitView.preferredWidth = Qt.binding(function() { return (voiceWorkbenchSplit.width - 12) * workspace.voiceRatio })
                    voiceInspectorSlot.SplitView.preferredWidth = Qt.binding(function() { return (voiceWorkbenchSplit.width - 12) * workspace.inspectorRatio })
                    viewSaveDelay.restart()
                }
                SplitView.fillHeight: true
                orientation: Qt.Horizontal
                Rectangle {
                    id: voicePickerPanel
                    visible: !voicesCollapsed
                    SplitView.preferredWidth: (voiceWorkbenchSplit.width - 12) * workspace.voiceRatio; SplitView.minimumWidth: 240
                    color: Theme.panel
                    ScrollView {
                        id: voicePickerScroll
                        anchors.fill: parent; anchors.margins: 12
                        contentWidth: availableWidth; clip: true; rightPadding: Theme.scrollBarSize
                        ColumnLayout {
                        width: voicePickerScroll.availableWidth; spacing: 8
                        RowLayout {
                            Label { text: qsTr("音色分配"); font.bold: true; color: Theme.text; Layout.fillWidth: true }
                            SubButton { text: qsTr("+ 添加音色"); onClicked: if (commitPending()) requestVoiceCreation() }
                        }
                        Label { text: qsTr("默认旁白用于新句与未覆盖句；局部音色保持独立。"); wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.secondaryText }
                        Label { text: qsTr("应用范围"); color: Theme.secondaryText }
                        SubComboBox { id: voiceScope; Layout.fillWidth: true; enabled: !voices || !voices.busy; model: [qsTr("默认旁白"), qsTr("当前句"), qsTr("当前段"), qsTr("全部未绑定句子"), qsTr("选中句")] }
                        SubTextField { id: cloudVoiceSearch; visible: localEngine.currentIndex === 2; Layout.fillWidth: true; placeholderText: qsTr("搜索音色名称") }
                        SubComboBox { id: builtinVoice; objectName: "voiceBuiltinSelector"; visible: localEngine.currentIndex === 2; Layout.fillWidth: true; textRole: "name"; model: cloudVoiceChoices
                            displayText: currentIndex < 0 ? qsTr("原选择不可用，请重新选择") : currentText
                            onActivated: { const row = model[currentIndex]; selectedCloudVoiceKey = (row.scope || cloudConnectionKey()) + "/" + row.id }
                            onModelChanged: { const key = cloudConnectionKey(); currentIndex = selectedCloudVoiceKey ? model.findIndex(function(row) { return (row.scope || key) + "/" + row.id === selectedCloudVoiceKey }) : model.length > 0 ? 0 : -1 }
                        }
                        RowLayout {
                            visible: localEngine.currentIndex === 2; Layout.fillWidth: true
                            SubButton { text: qsTr("选用"); enabled: voices && !voices.busy && selectedCloudVoice.compatible && (selectedCloudVoice.builtin || selectedCloudVoice.project || voices.voiceCatalog.state === "success") && draftId !== "" && controller && !controller.modifying; onClicked: if (commitPending()) { requestedVoiceScope = ["default", "selected", "paragraph", "unbound", "selection"][voiceScope.currentIndex]; if (selectedCloudVoice.project) assignVoice(selectedCloudVoice.versionId, requestedVoiceScope, selectedCloudVoice.displayName); else if (selectedCloudVoice.builtin) voices.useBuiltinVoice(selectedCloudVoice.id); else voices.useDiscoveredVoice(selectedCloudVoice.id) } }
                            SubButton { text: qsTr("刷新账号音色"); enabled: voices && !voices.busy; onClicked: voices.discoverVoices(true) }
                        }
                        SubCheckBox { visible: localEngine.currentIndex === 2 && !!selectedCloudVoice.id; text: qsTr("收藏当前音色"); checked: favoriteVoiceKeys.indexOf((selectedCloudVoice.scope || cloudConnectionKey()) + "/" + selectedCloudVoice.id) >= 0; onToggled: { const key = (selectedCloudVoice.scope || cloudConnectionKey()) + "/" + selectedCloudVoice.id; selectedCloudVoiceKey = key; favoriteVoiceKeys = checked ? favoriteVoiceKeys.concat([key]).slice(-500) : favoriteVoiceKeys.filter(function(value) { return value !== key }); if (controller) controller.persistSessionState(viewState()) } }
                        SubCheckBox { id: showUnavailableVoices; visible: localEngine.currentIndex === 2; text: qsTr("显示不可用音色") }
                        SubButton { visible: localEngine.currentIndex === 2 && !!selectedCloudVoice.hasPreview; text: selectedCloudVoice.builtin ? qsTr("试听官方样本") : qsTr("试听参考样本"); enabled: voices && !voices.busy && selectedCloudVoice.compatible; onClicked: { auditionSource = selectedCloudVoice.builtin ? qsTr("官方试听样本 · 不调用合成") : qsTr("音色参考样本 · 非合成效果"); voices.previewCloudVoice(selectedCloudVoice.id) } }
                        Label { visible: localEngine.currentIndex === 2; Layout.fillWidth: true; wrapMode: Text.Wrap; text: (voices && voices.statusText ? voices.statusText + "\n" : "") + (selectedCloudVoice.description ? selectedCloudVoice.description + qsTr(" · 中文\n") : "") + qsTr("官方目录 v2 · 2026-10-17 · 北京 Flash\n目录不代表权限；无样本时，选用后确认费用生成试听。") + (voices && voices.voiceCatalog.message ? "\n" + voices.voiceCatalog.message : ""); color: Theme.secondaryText; maximumLineCount: 5; elide: Text.ElideRight; ToolTip.visible: voiceCatalogHover.hovered; ToolTip.text: text; HoverHandler { id: voiceCatalogHover } }
                        SubButton { id: cloudVoiceAdvanced; visible: localEngine.currentIndex === 2; checkable: true; text: qsTr("高级音色详情") }
                        TextEdit { visible: cloudVoiceAdvanced.checked && localEngine.currentIndex === 2; readOnly: true; selectByMouse: true; Layout.fillWidth: true; wrapMode: TextEdit.WrapAnywhere; color: Theme.secondaryText; text: selectedCloudVoice.id || ""; textFormat: TextEdit.PlainText }
                        SubCheckBox { id: showUnavailableLocal; visible: localEngine.currentIndex !== 2; text: qsTr("显示不可用音色") }
                        SubTextField { id: localVoiceSearch; objectName: "localVoiceSearch"; visible: localEngine.currentIndex !== 2; Layout.fillWidth: true; placeholderText: qsTr("筛选本页本地音色"); selectByMouse: true }
                        ListView {
                            id: voiceProfiles
                            visible: localEngine.currentIndex !== 2
                            Layout.fillWidth: true; Layout.preferredHeight: Math.max(40, contentHeight); interactive: false; clip: true
                            model: voices ? voices.profiles.filter(function(row) { return row.name.toLowerCase().indexOf(localVoiceSearch.text.toLowerCase()) >= 0 }) : []
                            ScrollBar.vertical: SubScrollBar {}
                            delegate: ItemDelegate {
                                required property var modelData
                                width: voiceProfiles.width
                                contentItem: Label { textFormat: Text.PlainText; text: modelData.name; color: Theme.text; elide: Text.ElideRight }
                                highlighted: voices && voices.selectedProfile === modelData.id
                                background: Rectangle { color: parent.highlighted ? Theme.selection : parent.hovered ? Theme.listHover : Theme.panel; border.color: parent.activeFocus ? Theme.accent : "transparent" }
                                enabled: voices && !voices.busy
                                onClicked: voices.refresh(modelData.id)
                            }
                            Label { anchors.centerIn: parent; visible: voiceProfiles.count === 0; text: localVoiceSearch.text ? qsTr("本页没有匹配音色") : qsTr("先在音色库添加声音"); color: Theme.secondaryText }
                        }
                        RowLayout {
                            visible: localEngine.currentIndex !== 2
                            SubButton { text: qsTr("上一页"); enabled: voices && !voices.busy && voices.profileOffset > 0; onClicked: voices.refresh(voices.selectedProfile, false, Math.max(0, voices.profileOffset - 50), 0) }
                            SubButton { text: qsTr("下一页"); enabled: voices && !voices.busy && voices.profiles.length === 50; onClicked: voices.refresh(voices.selectedProfile, false, voices.profileOffset + 50, 0) }
                        }
                        ListView {
                            id: voiceVersions
                            visible: localEngine.currentIndex !== 2
                            Layout.fillWidth: true; Layout.preferredHeight: Math.max(40, contentHeight); interactive: false; clip: true; spacing: 8
                            model: workspace.suggestedVersions
                            ScrollBar.vertical: SubScrollBar {}
                            delegate: ColumnLayout {
                                required property var modelData
                                width: voiceVersions.width
                                Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.text; wrapMode: Text.Wrap; text: modelData.metadata.preset ? modelData.metadata.preset.name : (modelData.metadata.referenceText || qsTr("参考音色版本")); maximumLineCount: 2; elide: Text.ElideRight }
                                Label { text: (controller && controller.defaultVoiceVersionId === modelData.id ? qsTr("默认旁白 · ") : "") + (controller && controller.selected.voiceVersionId === modelData.id ? qsTr("当前句已分配 · ") : "") + (localVoiceUnavailableReason(modelData) || qsTr("已审核参考")); color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                SubButton { text: qsTr("试听参考样本"); visible: !!modelData.referencePath; enabled: previewPlayer && !previewPlayer.loading; onClicked: { auditionSource = qsTr("本地参考样本 · 非合成效果"); previewPlayer.play(modelData.referencePath, modelData.referenceSha256) } }
                                SubButton { text: qsTr("分配给%1").arg(voiceScope.currentText); enabled: controller && !controller.modifying && (voiceScope.currentIndex === 0 || controller.selectedId !== "") && !localVoiceUnavailableReason(modelData); onClicked: if (commitPending()) assignVoice(modelData.id, ["default", "selected", "paragraph", "unbound", "selection"][voiceScope.currentIndex], selectedVoiceName()) }
                            }
                        }
                        RowLayout {
                            visible: localEngine.currentIndex !== 2
                            SubButton { text: qsTr("较新版本"); enabled: voices && !voices.busy && voices.versionOffset > 0; onClicked: voices.refresh(voices.selectedProfile, false, voices.profileOffset, Math.max(0, voices.versionOffset - 50)) }
                            SubButton { text: qsTr("较旧版本"); enabled: voices && !voices.busy && voiceVersions.count === 50; onClicked: voices.refresh(voices.selectedProfile, false, voices.profileOffset, voices.versionOffset + 50) }
                        }
                    }
                    }
                }
                Rectangle {
                    SplitView.fillWidth: true; SplitView.minimumWidth: 320
                    color: Theme.background
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 12; spacing: 8
                        RowLayout {
                            Label { text: qsTr("配音文稿"); color: Theme.text; font.pixelSize: 18; font.bold: true; Layout.fillWidth: true }
                            Label { text: qsTr("%1 句").arg(manuscript.count); color: Theme.secondaryText }
                            MenuButton {
                                text: qsTr("句段操作 ▾"); enabled: draftId !== "" && controller && !controller.modifying
                                onClicked: sentenceMenu.popup(this, 0, height)
                                SubMenu {
                                    id: sentenceMenu
                                    MenuItem { text: qsTr("在正文光标处分句…"); onTriggered: { const offset = bodyEditor.cursorPosition; if (commitPending()) controller.previewSplit(offset) } }
                                    MenuItem { text: qsTr("与下一句合并…"); onTriggered: if (commitPending()) controller.previewMerge() }
                                    MenuItem { text: qsTr("重新分当前段…"); onTriggered: if (commitPending()) { textOperation = "resegment"; pastedText.text = controller.paragraphText(); textDialog.open() } }
                                    MenuSeparator {}
                                    MenuItem { text: qsTr("恢复默认旁白"); onTriggered: if (commitPending()) controller.restoreDefaultVoice() }
                                    MenuItem { text: qsTr("当前句上移"); onTriggered: if (commitPending()) controller.moveSelected(-1) }
                                    MenuItem { text: qsTr("当前句下移"); onTriggered: if (commitPending()) controller.moveSelected(1) }
                                    MenuItem { text: qsTr("当前段上移"); onTriggered: if (commitPending()) controller.moveSelected(-1, true) }
                                    MenuItem { text: qsTr("当前段下移"); onTriggered: if (commitPending()) controller.moveSelected(1, true) }
                                    MenuSeparator {}
                                    MenuItem { text: qsTr("删除当前句并归档…"); onTriggered: if (commitPending()) controller.previewDelete() }
                                }
                            }
                        }
                        RowLayout {
                            visible: draftId !== "" && draftDirty
                            Label { text: draftDirty ? qsTr("尚未应用 · Enter 应用，Shift+Enter 换行，Esc 撤回") : qsTr("↑ ↓ 切句 · Enter 编辑 · 空格试听"); Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.secondaryText; font.pixelSize: 12 }
                            SubButton { text: qsTr("撤回"); enabled: draftDirty; onClicked: loadDraft() }
                            SubButton { text: qsTr("应用编辑"); enabled: draftDirty; onClicked: commitPending() }
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider }
                        RowLayout {
                            Layout.fillWidth: true
                            SubTextField { id: manuscriptSearch; Layout.fillWidth: true; placeholderText: qsTr("查找文稿"); onTextEdited: manuscriptFilterDelay.restart() }
                            SubComboBox { id: manuscriptState; model: [qsTr("全部"), qsTr("未应用"), qsTr("已应用"), qsTr("已失效")]; onActivated: manuscriptFilterDelay.restart() }
                            SubSpinBox { id: paragraphFilter; from: 0; to: 10000; editable: true; ToolTip.visible: hovered; ToolTip.text: qsTr("段落编号；0 显示全部"); onValueModified: manuscriptFilterDelay.restart() }
                        }
                        Timer { id: manuscriptFilterDelay; interval: 150; onTriggered: { controller.setManuscriptFilter(manuscriptSearch.text, ["all", "missing", "applied", "stale"][manuscriptState.currentIndex], paragraphFilter.value); manuscript.currentIndex = controller.filteredIndex(controller.selectedId) } }
                        ListView {
                            id: manuscript; objectName: "voiceManuscript"
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4; reuseItems: true
                            model: controller ? controller.filteredModel : null
                            keyNavigationEnabled: false
                            ScrollBar.vertical: SubScrollBar {}
                            delegate: ItemDelegate {
                                required property string segmentId
                                required property string paragraphId
                                required property int paragraphNumber
                                required property string body
                                required property bool manualSpoken
                                required property bool locked
                                required property string generationState
                                required property bool stale
                                required property string voiceVersionId
                                required property string voiceName
                                required property var selectedTake
                                required property int index
                                property alias editorHost: sentenceEditorHost
                                property alias candidateHost: sentenceCandidateHost
                                width: manuscript.width
                                highlighted: controller && controller.selectedIds.indexOf(segmentId) >= 0
                                background: Rectangle { color: parent.highlighted ? Theme.selection : parent.hovered ? Theme.listHover : Theme.panel; radius: Theme.radiusInput }
                                contentItem: ColumnLayout {
                                    spacing: 4
                                    RowLayout {
                                        SubCheckBox { objectName: "voiceSentenceSelection"; checked: controller && controller.selectedIds.indexOf(segmentId) >= 0; onToggled: if (commitPending()) controller.selectMany(segmentId, true) }
                                        Label { text: locked ? qsTr("已锁定") : qsTr("可编辑"); color: locked ? Theme.warning : Theme.secondaryText; Layout.fillWidth: true }
                                        SubButton { text: locked ? qsTr("解锁") : qsTr("锁定"); enabled: controller && !controller.modifying; onClicked: if (commitPending()) { controller.select(segmentId); controller.setLocked(!locked) } }
                                    }
                                    Item { id: sentenceEditorHost; Layout.fillWidth: true; implicitHeight: controller && controller.selectedId === segmentId ? bodyEditor.height : 0; visible: controller && controller.selectedId === segmentId }
                                    Label { visible: !controller || controller.selectedId !== segmentId; textFormat: Text.PlainText; Layout.fillWidth: true; text: body; font.pixelSize: 15; lineHeight: 1.25; wrapMode: Text.Wrap; color: Theme.text }
                                    Label { text: qsTr("第 %1 段 · 句 %2").arg(paragraphNumber).arg(index + 1) + " · " + voiceName + " · " + ({unbound: qsTr("未选择音色"), pending: qsTr("待生成"), generating: qsTr("生成中"), candidate: qsTr("已有候选待应用"), applied: qsTr("已应用"), stale: qsTr("已失效"), failed: qsTr("生成失败，可重新生成")}[generationState] || qsTr("待生成")); color: stale || generationState === "failed" ? Theme.warning : Theme.secondaryText; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                    RowLayout {
                                        visible: controller && controller.selectedId === segmentId
                                        SubButton { objectName: "voiceSentencePlay"; text: qsTr("播放"); enabled: selectedTake && !!selectedTake.id && !storageBusy; onClicked: toggleAudition() }
                                        SubButton { objectName: "voiceSentenceGenerate"; text: selectedTake && selectedTake.id ? qsTr("重新生成") : qsTr("生成"); enabled: !locked && !generating && !storageBusy && controller && !controller.modifying; onClicked: generate("selected") }
                                        SubButton { objectName: "voiceSentenceCandidates"; text: candidatesExpanded ? qsTr("收起候选") : qsTr("展开候选"); onClicked: candidatesExpanded = !candidatesExpanded }
                                    }
                                    Item { id: sentenceCandidateHost; Layout.fillWidth: true; implicitHeight: controller && controller.selectedId === segmentId && candidatesExpanded ? inlineCandidates.implicitHeight : 0; visible: controller && controller.selectedId === segmentId && candidatesExpanded }
                                }
                                onClicked: { choose(segmentId); manuscript.forceActiveFocus() }
                                onDoubleClicked: { choose(segmentId); bodyEditor.forceActiveFocus() }
                            }
                            ColumnLayout {
                                anchors.centerIn: parent; width: Math.min(360, manuscript.width - 32)
                                visible: manuscript.count === 0; spacing: 12
                                Label { text: qsTr("先放入要朗读的文稿"); color: Theme.text; font.pixelSize: 18; font.bold: true; Layout.alignment: Qt.AlignHCenter }
                                Label { text: qsTr("添加文稿 → 分句与音色 → 生成 → 试听并应用 → 导出音频"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
                                PrimaryButton { text: qsTr("添加文稿…"); Layout.alignment: Qt.AlignHCenter; enabled: controller && !controller.modifying; onClicked: appendText() }
                                SubButton { text: qsTr("导入 TXT / JSON…"); Layout.alignment: Qt.AlignHCenter; enabled: controller && !controller.modifying; onClicked: openScriptDialog() }
                            }
                        }
                    }
                }
                Item {
                    id: voiceInspectorSlot
                    visible: workspace.width >= 900 && !inspectorCollapsed
                    SplitView.preferredWidth: (voiceWorkbenchSplit.width - 12) * workspace.inspectorRatio; SplitView.minimumWidth: 260
                    Rectangle {
                    id: voiceInspectorPanel; objectName: "voiceInspectorPanel"
                    parent: workspace.width < 900 ? inspectorDrawer.contentItem : voiceInspectorSlot
                    anchors.fill: parent
                    visible: !inspectorCollapsed
                    color: Theme.panel
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 10; spacing: 8
                        ColumnLayout {
                            visible: controller && controller.selectedIds.length > 1; Layout.fillWidth: true
                            Label { text: controller ? qsTr("已选 %1 句 · 锁定跳过 %2 句").arg(controller.selectedIds.length).arg(controller.selectionProperties.locked) : ""; color: Theme.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Label { text: controller && controller.selectionProperties.language ? qsTr("共同语言：%1").arg(controller.selectionProperties.language) : qsTr("语言：混合值"); color: Theme.secondaryText }
                            RowLayout {
                                SubButton { text: qsTr("锁定所选"); onClicked: if (commitPending()) controller.setLocked(true) }
                                SubButton { text: qsTr("解锁所选"); onClicked: controller.setLocked(false) }
                            }
                            SubButton { text: qsTr("当前句朗读参数应用到所选"); Layout.fillWidth: true; onClicked: if (commitPending()) controller.editSelection({language: controller.selected.language, parameters: controller.selected.parameters}) }
                            SubButton { text: qsTr("当前句增益与停顿应用到所选"); Layout.fillWidth: true; onClicked: if (commitPending()) controller.editSelection({gain: controller.selected.gain, pauseMs: controller.selected.pauseMs}) }
                            SubButton { text: qsTr("生成选中句"); enabled: !generating && !storageBusy && controller && !controller.modifying; onClicked: generate("selection") }
                        }
                        Label { visible: draftId === ""; text: qsTr("选择一句文稿，调整朗读或编排。"); color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        TabBar {
                            visible: draftId !== ""
                            id: inspectorTabs; objectName: "voiceInspectorTabs"; Layout.fillWidth: true
                            TabButton { text: qsTr("朗读"); background: Rectangle { color: parent.checked ? Theme.selection : Theme.panel; border.color: parent.checked ? Theme.accent : Theme.border } }
                            TabButton { text: qsTr("编排"); background: Rectangle { color: parent.checked ? Theme.selection : Theme.panel; border.color: parent.checked ? Theme.accent : Theme.border } }
                        }
                        StackLayout {
                            visible: draftId !== ""
                            currentIndex: inspectorTabs.currentIndex
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ScrollView {
                                id: readingScroll; contentWidth: availableWidth; clip: true
                                ColumnLayout {
                                    width: readingScroll.availableWidth; spacing: 8
                                    Label { text: qsTr("当前句朗读"); font.bold: true; color: Theme.text }
                                    Label { text: qsTr("目标语言"); color: Theme.secondaryText }
                                    SubComboBox { id: targetLanguage; objectName: "voiceTargetLanguage"; Layout.fillWidth: true; enabled: draftId !== ""; readonly property var values: capabilities.languages; model: values.map(function(value) { return value === "Chinese" ? qsTr("中文") : value === "English" ? qsTr("英语") : qsTr("日语") }); onActivated: if (!loadingDraft) { draftDirty = true; targetLanguageDirty = true } }
                                    SubCheckBox { id: manualSpoken; text: qsTr("使用朗读修正"); enabled: draftId !== ""; onToggled: if (!loadingDraft) draftDirty = true }
                                    TextArea { textFormat: TextEdit.PlainText; id: spokenEditor; objectName: "voiceSpokenEditor"; Layout.fillWidth: true; Layout.preferredHeight: 95; selectByMouse: true; wrapMode: TextEdit.Wrap; placeholderText: qsTr("修改发音，不改变正文；优先于替换表"); enabled: draftId !== "" && manualSpoken.checked; onTextChanged: if (!loadingDraft) draftDirty = true; Keys.onPressed: function(event) { workspace.editKey(event, spokenEditor) } }
                                    SubButton { text: qsTr("数字 / 日期 / 单位建议…"); enabled: draftId !== ""; onClicked: if (commitPending()) controller.previewNormalization() }
                                    ColumnLayout {
                                        visible: capabilities.parameters.indexOf("durationFactor") >= 0; Layout.fillWidth: true
                                        Label { text: qsTr("Index 2.5 · 表演"); color: Theme.text; font.bold: true }
                                        Label { text: qsTr("时长倍率 · 0.5 较快，2 较慢"); color: Theme.secondaryText }
                                        TextField { id: durationField; objectName: "indexDurationField"; Layout.fillWidth: true; text: "1"; enabled: draftId !== ""; validator: DoubleValidator { bottom: 0.5; top: 2; decimals: 2; locale: "C" } onTextEdited: if (!loadingDraft) { draftDirty = true; indexParametersDirty = true } }
                                        SubComboBox { id: emotionChoice; objectName: "indexEmotionChoice"; Layout.fillWidth: true; enabled: draftId !== ""; readonly property var values: ["", "happy", "angry", "sad", "afraid", "disgusted", "depressed", "surprised", "calm"]; model: [qsTr("沿用参考情绪"), qsTr("喜"), qsTr("怒"), qsTr("哀"), qsTr("惧"), qsTr("厌恶"), qsTr("低落"), qsTr("惊喜"), qsTr("平静")]; onActivated: if (!loadingDraft) { draftDirty = true; indexParametersDirty = true } }
                                        Label { visible: emotionChoice.currentIndex > 0; text: qsTr("情绪强度 · 大于 0，不超过 1"); color: Theme.secondaryText }
                                        TextField { id: emotionStrength; Layout.fillWidth: true; visible: emotionChoice.currentIndex > 0; text: "0.3"; enabled: draftId !== ""; validator: DoubleValidator { bottom: 0; top: 1; decimals: 2; locale: "C" } onTextEdited: if (!loadingDraft) { draftDirty = true; indexParametersDirty = true } }
                                        Label { text: qsTr("发音可用 <行|XING2> 标注。倍率不是精确时长，情绪强度由模型归一化。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                    }
                                    ColumnLayout {
                                        visible: capabilities.parameters.indexOf("speedFactor") >= 0; Layout.fillWidth: true
                                        Label { text: qsTr("语速倍率 · 小于 1 更慢"); color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                        TextField { id: speedField; objectName: "gptSpeedField"; Layout.fillWidth: true; text: "1"; enabled: draftId !== ""; validator: DoubleValidator { bottom: 0.5; top: 2; decimals: 2; locale: "C" } onTextEdited: if (!loadingDraft) { draftDirty = true; gptParametersDirty = true } }
                                        Label { text: qsTr("范围 0.5～2，1 为原速；可先用 0.85。重新生成才会生效。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                    }
                                    Label { visible: engineExpanded; text: qsTr("随机种子（可选）"); color: Theme.secondaryText }
                                    TextField { id: seedField; visible: engineExpanded; objectName: "voiceSeedField"; Layout.fillWidth: true; enabled: draftId !== ""; placeholderText: qsTr("留空随机，0～%1").arg(localEngine.currentIndex === 2 ? 65535 : 2147483647); validator: IntValidator { bottom: 0; top: localEngine.currentIndex === 2 ? 65535 : 2147483647 } onTextChanged: if (!loadingDraft) draftDirty = true }
                                    TextArea { textFormat: TextEdit.PlainText; id: notesEditor; objectName: "voiceNotesEditor"; Layout.fillWidth: true; Layout.preferredHeight: 58; selectByMouse: true; wrapMode: TextEdit.Wrap; placeholderText: qsTr("表演备注 · 仅保存，不发送给引擎"); enabled: draftId !== ""; onTextChanged: if (!loadingDraft) draftDirty = true; Keys.onPressed: function(event) { workspace.editKey(event, notesEditor) } }
                                    Label { text: qsTr("只显示当前引擎已接入的语言与参数；切换引擎不会改写已有句子或音频。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                    SubButton { text: engineExpanded ? qsTr("▾ 引擎配置") : qsTr("▸ 引擎配置"); checkable: true; checked: engineExpanded; Layout.fillWidth: true; onClicked: engineExpanded = !engineExpanded }
                                    ColumnLayout {
                                        id: engineConfiguration
                                        visible: engineExpanded; Layout.fillWidth: true; spacing: 8
                                        onVisibleChanged: if (visible) Qt.callLater(function() { readingScroll.contentItem.contentY = Math.max(0, engineConfiguration.y - 8) })
                                        Label { text: localEngine.currentText; font.bold: true; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                        TextField { id: pythonPath; readOnly: true; visible: localEngine.currentIndex !== 2; Layout.fillWidth: true; placeholderText: qsTr("在设置 → AI 配音配置 Python"); selectByMouse: true }
                                        Label { visible: localEngine.currentIndex === 2; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("使用 AI 服务的语音合成账号与 Workspace；音色需明确关联相同账号。") }
                                        SubButton { visible: localEngine.currentIndex === 2; text: qsTr("管理 AI 服务"); onClicked: if (workspace.Window.window) workspace.Window.window.openSettings("cloud", "") }
                                        RowLayout {
                                            visible: localEngine.currentIndex === 2; Layout.fillWidth: true
                                            Label { text: qsTr("软预算 ¥"); color: Theme.secondaryText }
                                            TextField { id: flashBudget; Layout.fillWidth: true; text: "1.00"; placeholderText: qsTr("当前范围预算，如 1.00") }
                                        }
                                        SubCheckBox { id: flashRegenerate; visible: localEngine.currentIndex === 2; text: qsTr("已有候选也重新付费生成"); checked: false }
                                        TextField { id: qwenModelPath; readOnly: true; Layout.fillWidth: true; visible: localEngine.currentIndex === 0 || localEngine.currentIndex >= 3; placeholderText: localEngine.currentIndex === 0 ? qsTr("Qwen Base 模型完整目录") : qsTr("引擎源码根目录，包含本地权重"); selectByMouse: true }
                                        SubButton { text: qsTr("预检环境与模型"); visible: localEngine.currentIndex === 0 || localEngine.currentIndex >= 3; enabled: controller && !controller.modifying && !generating; onClicked: if (commitPending()) controller.checkLocal(localEngine.engineKey, pythonPath.text, qwenModelPath.text) }
                                        Label { textFormat: Text.PlainText; visible: localEngine.currentIndex === 0 || localEngine.currentIndex >= 3; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller && controller.qwenStatus.device ? qsTr("预检：%1 · %2 GiB · %3\n切换引擎或路径后需重新预检。").arg(controller.qwenStatus.device).arg((Number(controller.qwenStatus.deviceBytes) / 1073741824).toFixed(1)).arg(controller.qwenStatus.provider) : qsTr("首次使用请配置环境并预检；缺少依赖会明确提示。") }
                                        Label { visible: localEngine.currentIndex === 4 && controller && !!controller.qwenStatus.auxiliaryProviders; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller && controller.qwenStatus.auxiliaryProviders ? qsTr("G2PW：%1；主配音使用 CUDA。").arg(controller.qwenStatus.auxiliaryProviders.join(" / ")) : "" }
                                        Label { textFormat: Text.PlainText; text: localEngine.currentIndex === 0 ? qsTr("Base 使用已审核参考与转写，不执行自由表演指令。") : localEngine.currentIndex === 3 ? qsTr("Index 支持 seed、时长倍率、单类情绪和发音标注；不发送备注。") : localEngine.currentIndex === 4 ? qsTr("GPT 使用已审核参考与转写，支持 seed 和语速倍率；专属权重控制待接入。") : localEngine.currentIndex === 2 ? qsTr("Flash 使用北京云声音版本。发送前确认文本与费用；不会上传本地参考，也不会自动重发。") : qsTr("Mock 仅产生测试音，不代表真实配音质量。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                    }
                                }
                            }
                            ScrollView {
                                id: arrangementScroll; contentWidth: availableWidth; clip: true
                                ColumnLayout {
                                    width: arrangementScroll.availableWidth; spacing: 8
                                    Label { text: qsTr("当前句编排"); color: Theme.text; font.bold: true }
                                    Label { text: qsTr("增益 · 0～4，1 为原音量"); color: Theme.secondaryText }
                                    TextField { id: gainField; Layout.fillWidth: true; enabled: draftId !== ""; validator: DoubleValidator { bottom: 0; top: 4; decimals: 3; locale: "C" } onTextChanged: if (!loadingDraft) draftDirty = true }
                                    Label { text: qsTr("句后停顿 · 毫秒"); color: Theme.secondaryText }
                                    TextField { id: pauseField; Layout.fillWidth: true; enabled: draftId !== ""; validator: DoubleValidator { bottom: 0; top: 60000; decimals: 3; locale: "C" } onTextChanged: if (!loadingDraft) draftDirty = true }
                                    Label { text: qsTr("裁切起点 / 终点 · 原生样本数"); color: Theme.secondaryText }
                                    RowLayout {
                                        TextField { id: trimStart; Layout.fillWidth: true; enabled: draftId !== "" && controller && !!controller.selected.selectedTake; onTextChanged: if (!loadingDraft) draftDirty = true }
                                        TextField { id: trimEnd; Layout.fillWidth: true; enabled: trimStart.enabled; onTextChanged: if (!loadingDraft) draftDirty = true }
                                    }
                                    Label { text: qsTr("也可在下方波形按住 Shift 拖动裁切。修改只影响编排，原始候选保留。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                    SubButton { text: qsTr("应用编辑"); enabled: controller && draftDirty; onClicked: commitPending() }
                                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider }
                                    Label { text: qsTr("字幕关联"); color: Theme.text; font.bold: true }
                                    RowLayout { SubCheckBox { id: syncText; text: qsTr("字幕文本"); checked: true } SubCheckBox { id: syncTime; text: qsTr("字幕时间") } }
                                    SubButton { text: qsTr("预览差异并同步…"); enabled: controller && draftId !== ""; onClicked: if (commitPending()) controller.previewSubtitleSync(syncText.checked, syncTime.checked) }
                                    Label { text: controller && controller.selected.sourceMissing ? qsTr("原字幕已删除或拆合，配音内容独立保留。") : ""; visible: text !== ""; color: Theme.warning; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                    SubButton { text: qsTr("关联另一个字幕…"); enabled: controller && draftId !== ""; onClicked: if (commitPending()) { relinkDialog.choices = controller.subtitleChoices(); relinkDialog.open() } }
                                }
                            }
                        }
                    }
                }
                }
            }
            Rectangle {
                id: voiceSequencePanel
                visible: !sequenceCollapsed
                SplitView.preferredHeight: workspace.height < 650 ? 96 : 178; SplitView.minimumHeight: 88
                color: Theme.panelSecondary
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    RowLayout {
                        Label { text: qsTr("配音序列"); font.bold: true; color: Theme.text }
                        Label { text: auditionSource; color: Theme.secondaryText; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                        SubButton { text: previewPlayer && (previewPlayer.playing || previewPlayer.loading) ? qsTr("暂停试听") : previewPlayer && previewPlayer.paused ? qsTr("继续试听") : qsTr("试听当前句"); shortcutHint: "Space"; enabled: previewPlayer && (previewPlayer.playing || previewPlayer.loading) || audio && !audio.busy && draftId !== ""; onClicked: toggleAudition() }
                        Label { visible: workspace.width >= 1200; text: qsTr("缩放"); color: Theme.secondaryText }
                        Slider { id: sequenceZoom; objectName: "voiceSequenceZoom"; visible: workspace.width >= 1200; Layout.preferredWidth: 90; from: 20; to: 120; value: 60; ToolTip.visible: hovered; ToolTip.text: qsTr("音频片段显示比例") }
                        SubButton { text: qsTr("连续试听"); enabled: audio && !audio.busy && manuscript.count > 0; onClicked: if (commitPending()) { auditionSource = qsTr("连续成稿 · 已应用音频"); audio.prepare(false) } }
                        SubButton { text: qsTr("导出音频…"); enabled: audio && !audio.busy && manuscript.count > 0; shortcutHint: "Ctrl+E"; onClicked: requestExport() }
                        SubButton { text: qsTr("取消音频处理"); visible: audio && audio.busy; onClicked: audio.cancel() }
                    }
                    ListView {
                        id: sequence; objectName: "voiceSequence"
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6; orientation: ListView.Horizontal; reuseItems: true
                        model: controller
                        keyNavigationEnabled: false
                        ScrollBar.horizontal: SubScrollBar {}
                        delegate: ItemDelegate {
                            required property string segmentId
                            required property string body
                            required property var selectedTake
                            required property real pauseMs
                            readonly property var wave: audio ? audio.rows.find(function(row) { return row.id === segmentId }) : null
                            readonly property var peaks: wave && audio ? audio.waveform(segmentId) : []
                            readonly property real playheadRatio: wave && previewPlayer && audio && audio.ownsPlayback() ? (previewPlayer.positionUs * 48000 / 1000000 - Number(wave.position)) / (Number(wave.end) - Number(wave.start)) : -1
                            onPeaksChanged: waveform.requestPaint()
                            width: Math.max(120, Math.min(1600, sequenceZoom.value * (wave ? (Number(wave.end) - Number(wave.start)) / 48000 : 2))); height: sequence.height
                            highlighted: controller && controller.selectedId === segmentId
                            contentItem: ColumnLayout {
                                spacing: 4
                                Label { textFormat: Text.PlainText; color: Theme.text; Layout.fillWidth: true; text: body; elide: Text.ElideRight; font.pixelSize: 12 }
                                Item {
                                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 28
                                    Canvas {
                                        id: waveform; objectName: "voiceWaveform"; anchors.fill: parent
                                        onPaint: {
                                            const ctx = getContext("2d")
                                            ctx.clearRect(0, 0, width, height); ctx.strokeStyle = Theme.waveform; ctx.beginPath()
                                            if (wave && peaks.length > 0) {
                                                for (let i = 0; i < peaks.length; ++i) {
                                                    const x = i * width / peaks.length
                                                    ctx.moveTo(x, height * (0.5 - Math.max(-1, Math.min(1, peaks[i][0])) * 0.45))
                                                    ctx.lineTo(x, height * (0.5 - Math.max(-1, Math.min(1, peaks[i][1])) * 0.45))
                                                }
                                            }
                                            ctx.stroke()
                                        }
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                    }
                                    Label { anchors.centerIn: parent; visible: !wave || peaks.length === 0; text: selectedTake && selectedTake.id ? qsTr("波形未就绪") : qsTr("待生成并应用音频"); color: Theme.secondaryText; font.pixelSize: 12 }
                                    // 静态波形只在数据或尺寸变化时绘制，播放时仅移动游标。
                                    Rectangle { visible: playheadRatio >= 0 && playheadRatio <= 1; x: Math.max(0, Math.min(parent.width - 1, parent.width * playheadRatio)); width: 1; height: parent.height; color: Theme.playhead }
                                    MouseArea {
                                        anchors.fill: parent; enabled: !audio || !audio.busy
                                        property bool trimming: false
                                        preventStealing: trimming
                                        property real startRatio: 0
                                        onPressed: function(mouse) { trimming = !!(mouse.modifiers & Qt.ShiftModifier); startRatio = Math.max(0, Math.min(1, mouse.x / width)) }
                                        onReleased: function(mouse) {
                                            if (!trimming || !wave || !selectedTake || !selectedTake.id) return
                                            trimming = false
                                            if (!commitPending()) return
                                            controller.select(segmentId)
                                            const finish = Math.max(0, Math.min(1, mouse.x / width))
                                            const base = Number(selectedTake.trimStart), count = Number(selectedTake.trimEnd) - base
                                            const begin = Math.round(base + Math.min(startRatio, finish) * count), end = Math.round(base + Math.max(startRatio, finish) * count)
                                            if (end <= begin) return
                                            trimStart.text = String(begin); trimEnd.text = String(end); commitPending()
                                        }
                                        onClicked: function(mouse) { if (mouse.modifiers & Qt.ShiftModifier) return; if (commitPending()) { choose(segmentId); if (wave && audio) audio.seek(segmentId, mouse.x / width); manuscript.forceActiveFocus() } }
                                    }
                                }
                                Label { textFormat: Text.PlainText; color: wave && wave.clipping ? Theme.danger : Theme.secondaryText; text: (selectedTake && selectedTake.id ? (selectedTake.mock ? qsTr("测试音已应用") : qsTr("音频已应用")) : qsTr("待生成 / 应用")) + (wave ? " · " + ((Number(wave.end) - Number(wave.start)) / 48000).toFixed(2) + "s" : "") + qsTr(" · 停顿 %1ms").arg(pauseMs); elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: 12 }
                            }
                            onClicked: if (commitPending()) { choose(segmentId); if (wave && audio) audio.seek(segmentId, 0); manuscript.forceActiveFocus() }
                        }
                        Label { visible: sequence.count === 0; anchors.centerIn: parent; text: qsTr("文稿分句后，逐句音频会显示在这里。"); color: Theme.secondaryText }
                    }
                }
            }
        }
        VoicePreviewTransport { sourceTitle: auditionSource; player: previewPlayer; preparing: (audio && audio.preparing) || (voices && voices.previewLoading); onStopRequested: { if (voices) voices.cancelPreview(); if (audio) audio.stop() } }
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 8; spacing: 8
            BusyIndicator { running: generating || controller && controller.modifying || audio && audio.busy; visible: running; implicitWidth: 20; implicitHeight: 20 }
            Label {
                textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.secondaryText; elide: Text.ElideRight
                text: generating ? ttsTasks.statusText : audio && audio.busy ? audio.statusText + " " + audio.progress + "%" : (controller ? controller.statusText : "") + (audio && audio.statusText ? " · " + audio.statusText : "") + (previewPlayer && previewPlayer.statusText ? " · " + previewPlayer.statusText : "")
                ToolTip.visible: statusHover.hovered && truncated
                ToolTip.text: text
                HoverHandler { id: statusHover }
            }
            SubButton { text: qsTr("取消当前处理"); visible: controller && controller.modifying; onClicked: controller.cancel() }
        }
    }
    Connections {
        target: controller
        function onChanged() { if (controller && !controller.modifying && controller.flashPreview.rows && controller.flashPreview.rows.length > 0 && !flashDialog.visible) flashDialog.open() }
    }
    Dialog {
        id: flashDialog; objectName: "voiceFlashDialog"; anchors.centerIn: parent; modal: true
        title: qsTr("确认云端生成"); width: Math.min(720, workspace.width - 32); height: Math.min(560, workspace.height - 32)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: if (commitPending()) controller.confirmFlash()
        onRejected: if (controller) controller.discardFlash()
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text; text: controller && controller.flashPreview.rows ? qsTr("北京云业务空间：%1\n待发送 %2 句；复用 %3 句（不再次收费）。").arg(controller.flashPreview.workspace).arg(controller.flashPreview.rows.length).arg(controller.flashPreview.reused) : "" }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("确定将发送下面的朗读文本与 Voice ID。生成后显示实际输入／输出 Token，并按当时原价折算金额，不抵扣未知优惠或免费额度。实际已知费用达到软预算后暂停后续请求；单次请求可能超出预算，用量未知也会暂停。") }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: controller && controller.flashPreview.rows ? controller.flashPreview.rows : []
                ScrollBar.vertical: SubScrollBar {}
                delegate: Label { required property var modelData; width: ListView.view.width; wrapMode: Text.Wrap; textFormat: Text.PlainText; color: Theme.text; text: modelData.voice + " · " + modelData.text }
            }
        }
    }
    FileDialog { id: importDialog; nameFilters: [qsTr("配音文稿 (*.txt *.json)")]; onAccepted: if (commitPending()) controller.previewJson(selectedFile, targetedImport) }
    FileDialog { id: exportDialog; fileMode: FileDialog.SaveFile; defaultSuffix: "json"; nameFilters: [qsTr("配音 JSON (*.json)")]; onAccepted: controller.exportJson(selectedFile) }
    Dialog {
        id: wavOptions; objectName: "voiceWavOptions"; anchors.centerIn: parent; modal: true; width: Math.min(560, workspace.width - 32); height: Math.min(460, workspace.height - 32); title: qsTr("导出配音音频"); standardButtons: Dialog.Ok | Dialog.Cancel
        property var summary: ({missing: 0, stale: 0, total: 0, problems: []})
        onOpened: { standardButton(Dialog.Ok).text = qsTr("选择输出位置…"); standardButton(Dialog.Ok).enabled = summary.total > 0 && summary.missing === 0 }
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("共 %1 句 · 未应用音频 %2 句 · 音频失效 %3 句").arg(wavOptions.summary.total).arg(wavOptions.summary.missing).arg(wavOptions.summary.stale); color: wavOptions.summary.missing > 0 || wavOptions.summary.stale > 0 ? Theme.warning : Theme.text }
            ListView {
                visible: wavOptions.summary.problems.length > 0; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                model: wavOptions.summary.problems; ScrollBar.vertical: SubScrollBar {}
                delegate: ItemDelegate {
                    required property var modelData; width: ListView.view.width
                    contentItem: Label { text: modelData.text + "\n" + modelData.reason; wrapMode: Text.Wrap; color: Theme.text; textFormat: Text.PlainText }
                    onClicked: { wavOptions.close(); choose(modelData.id); inspectorCollapsed = false; inspectorTabs.currentIndex = 1 }
                }
            }
            Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text; text: qsTr("48kHz · 24-bit PCM · 单声道。冻结当前选中 Take 和编排；缺句不会跳过。Mock 音频导出仍是测试音。"); }
            SubCheckBox { id: exportSentences; text: qsTr("句级文件，保存到一个新目录"); onToggled: if (checked) exportLevel.checked = false }
            SubCheckBox { id: exportLevel; text: qsTr("整轨 RMS 平均电平 −20dBFS，样本峰值上限 −1dBFS"); enabled: !exportSentences.checked }
            Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("默认关闭电平处理。RMS 不等同 LUFS／真峰值；只改变导出派生文件，不逐句拉齐，也不改原 Take。") }
        }
        onAccepted: { if (exportSentences.checked) sentenceFolderDialog.open(); else { wavDialog.defaultSuffix = "wav"; wavDialog.title = qsTr("导出整篇 WAV"); wavDialog.open() } }
    }
    FolderDialog { id: sentenceFolderDialog; title: qsTr("选择父目录，将新建配音输出目录"); onAccepted: if (commitPending()) audio.exportWav(selectedFolder, false, true, false) }
    FileDialog { id: wavDialog; fileMode: FileDialog.SaveFile; onAccepted: if (commitPending()) audio.exportWav(selectedFile, exportLevel.checked, exportSentences.checked, false) }
    Dialog {
        id: staleWavDialog; property url file; property bool normalize: false; property bool sentences: false
        anchors.centerIn: parent; modal: true; width: Math.min(500, workspace.width - 32); height: 180; title: qsTr("选中 Take 已过期"); standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { textFormat: Text.PlainText; text: qsTr("文本或生成条件已改变。确认仍按当前选中旧音频导出？取消后可局部重新生成并应用新候选。"); wrapMode: Text.Wrap; color: Theme.text }
        onAccepted: if (commitPending()) audio.exportWav(file, normalize, sentences, true)
    }
    Dialog {
        id: textDialog; anchors.centerIn: parent; modal: true; width: Math.min(650, workspace.width - 32); height: Math.min(500, workspace.height - 32)
        title: textOperation === "append" ? qsTr("追加长文并预览分段") : textOperation === "resegment" ? qsTr("仅重新分当前段落") : qsTr("朗读替换表 · 每行 原词=替换词")
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: TextArea { textFormat: TextEdit.PlainText; id: pastedText; selectByMouse: true; wrapMode: TextEdit.Wrap }
        onAccepted: {
            Qt.inputMethod.commit()
            if (textOperation === "append") controller.previewText(pastedText.text, localEngine.currentIndex === 1 ? "mock" : localEngine.currentIndex === 2 ? "cloud" : localEngine.engineKey)
            else if (textOperation === "resegment") controller.previewResegment(pastedText.text)
            else { const rules = []; for (const line of pastedText.text.split("\n")) { if (line.trim() === "") continue; const at = line.indexOf("="); rules.push({from: at > 0 ? line.substring(0, at) : "", to: at > 0 ? line.substring(at + 1) : ""}) } controller.previewReplacements(rules) }
        }
    }
    Dialog {
        id: changesDialog; objectName: "voiceChangesDialog"; anchors.centerIn: parent; modal: true
        width: Math.min(760, workspace.width - 32); height: Math.min(520, workspace.height - 32)
        title: qsTr("确认文稿变更"); standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ColumnLayout {
            Label { textFormat: Text.PlainText; text: controller ? controller.statusText : ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.secondaryText }
            ListView {
                id: differences; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: controller ? controller.preview : []
                ScrollBar.vertical: SubScrollBar {}
                delegate: Label {
                    textFormat: Text.PlainText; required property var modelData; width: differences.width; wrapMode: Text.Wrap; color: Theme.text
                    text: {
                        let description = (modelData.kind || qsTr("定向更新")) + "\n" + (modelData.beforeText || (modelData.before ? modelData.before.text : "") || "") + " → " + (modelData.afterText || (modelData.after ? modelData.after.text : "") || modelData.text || "")
                        if (modelData.beforeTime) description += "\n" + modelData.beforeTime + " → " + modelData.afterTime
                        if (modelData.before && modelData.after) {
                            for (const field of ["provider", "model", "modelVersion", "language", "voiceVersionId", "parameters", "notes"])
                                if (JSON.stringify(modelData.before[field]) !== JSON.stringify(modelData.after[field])) description += "\n" + field + ": " + JSON.stringify(modelData.before[field]) + " → " + JSON.stringify(modelData.after[field])
                        } else if (modelData.after) {
                            description += "\n" + qsTr("引擎：%1；语言：%2；参数：%3").arg(modelData.after.model).arg(modelData.after.language).arg(JSON.stringify(modelData.after.parameters))
                            if (modelData.after.requestedVoiceVersionId) description += "\n" + qsTr("请求声音：%1；%2").arg(modelData.after.requestedVoiceVersionId).arg(modelData.after.voiceVersionId ? qsTr("复用项目版本") : qsTr("导入后请分配声音"))
                        }
                        return description + (modelData.manualSpokenProtected ? qsTr("\n已有手工朗读保持不变") : "")
                    }
                }
            }
        }
        onAccepted: controller.applyPreview()
        onRejected: controller.discardPreview()
    }
    Dialog {
        id: generationRangeDialog; anchors.centerIn: parent; modal: true; width: Math.min(520, workspace.width - 32); height: Math.min(230, workspace.height - 32)
        title: qsTr("确认生成范围"); standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: controller ? qsTr("需要生成 %1 句，可复用 %2 句，不可生成 %3 句。\n新结果只进入候选；试听并应用后才用于成稿。已有音频和人工编排保持不变；已锁定句跳过。").arg(controller.generationPreview.needed || 0).arg(controller.generationPreview.reused || 0).arg(controller.generationPreview.blocked || 0) : ""; color: Theme.text; wrapMode: Text.Wrap }
        onAccepted: controller.confirmGeneration()
        onRejected: controller.discardGeneration()
    }
    Dialog {
        id: defaultVoiceDialog; property string versionId; property string voiceName; property int affected: 0
        anchors.centerIn: parent; modal: true; width: Math.min(520, workspace.width - 32); height: Math.min(230, workspace.height - 32)
        title: qsTr("修改默认旁白"); standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: qsTr("将使用“%1”，影响 %2 句继承句。局部音色保持不变；已有音频保留并标记失效，不自动生成或替换。").arg(defaultVoiceDialog.voiceName).arg(defaultVoiceDialog.affected); wrapMode: Text.Wrap; color: Theme.text; textFormat: Text.PlainText }
        onAccepted: controller.bindVoiceVersion(versionId, "default", voiceName)
    }
    Dialog {
        id: cropDialog; anchors.centerIn: parent; modal: true; width: Math.min(550, workspace.width - 32); title: qsTr("已有人工裁切，请比较候选")
        contentItem: ColumnLayout {
            Label { textFormat: Text.PlainText; text: qsTr("保留时间范围可能超过新音频，程序会拒绝，不静默截断。"); color: Theme.text; wrapMode: Text.Wrap }
            SubButton { text: qsTr("试听当前音频原音"); enabled: controller && !controller.modifying && previewPlayer && !previewPlayer.loading; onClicked: controller.auditionSelectedTake() }
            SubButton { text: qsTr("试听新候选原音"); enabled: previewPlayer && !previewPlayer.loading; onClicked: { for (const take of controller.candidates) if (take.id === cropCandidate) { previewPlayer.play(take.path, take.sha256); break } } }
            SubButton { text: qsTr("保留裁切时间"); onClicked: { controller.applyCandidate(cropCandidate, "keep"); cropDialog.close() } }
            SubButton { text: qsTr("重置为新音频全段"); onClicked: { controller.applyCandidate(cropCandidate, "reset"); cropDialog.close() } }
            SubButton { text: qsTr("取消"); onClicked: cropDialog.close() }
        }
        onClosed: { if (controller && controller.modifying) controller.cancel(); if (previewPlayer) previewPlayer.stop() }
    }
    ColumnLayout {
        id: inlineCandidates; objectName: "voiceInlineCandidates"
        parent: manuscript.currentItem ? manuscript.currentItem.candidateHost : workspace
        visible: candidatesExpanded && manuscript.currentItem !== null
        width: parent ? parent.width : 0
        height: implicitHeight
        spacing: 8
                                Label { text: qsTr("先试听候选，满意后应用"); color: Theme.text; font.bold: true }
                                Label { text: qsTr("只有已应用的音频会用于连续试听与导出。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                                SubButton { text: qsTr("刷新当前句候选"); enabled: controller && !controller.modifying && draftId !== ""; onClicked: controller.loadCandidates() }
                                ListView {
                                    id: candidatesList; objectName: "voiceCandidatesList"
                                    Layout.fillWidth: true; Layout.preferredHeight: Math.min(240, Math.max(80, contentHeight)); clip: true; spacing: 8
                                    model: controller ? controller.candidates.filter(function(row) { return row.segmentId === controller.selectedId }) : []
                                    ScrollBar.vertical: SubScrollBar {}
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: candidatesList.width; height: candidateContent.implicitHeight + 16; color: Theme.panelSecondary; radius: Theme.radiusInput
                                        ColumnLayout {
                                            id: candidateContent
                                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                                            Label { textFormat: Text.PlainText; text: qsTr("候选 %1").arg((controller ? controller.candidateOffset : 0) + index + 1) + (modelData.mock ? qsTr(" · 测试音") : "") + (controller && controller.selected.selectedTake && controller.selected.selectedTake.id === modelData.id ? qsTr(" · 已应用") : controller && controller.candidateIsCurrent(modelData.id) ? qsTr(" · 可应用") : qsTr(" · 输入已变化，仅试听")); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                            Label { textFormat: Text.PlainText; visible: modelData.warnings && modelData.warnings.length > 0; text: modelData.warnings ? qsTr("请试听：%1").arg(modelData.warnings.join("；")) : ""; color: Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 }
                                            Label { Layout.fillWidth: true; color: Theme.secondaryText; text: qsTr("%1 秒").arg((Number(modelData.sampleCount) / Number(modelData.sampleRate)).toFixed(2)) }
                                            RowLayout {
                                                SubButton { text: qsTr("试听"); enabled: previewPlayer && !previewPlayer.loading; onClicked: { auditionSource = qsTr("候选 %1 · 试听不改变应用版本").arg(controller.candidateOffset + index + 1); previewPlayer.play(modelData.path, modelData.sha256) } }
                                                SubButton { text: qsTr("应用此候选"); enabled: controller && !controller.modifying && controller.candidateIsCurrent(modelData.id); onClicked: if (commitPending()) { if (controller.selected.selectedTake && controller.selected.selectedTake.manualCrop) { cropCandidate = modelData.id; cropDialog.open() } else controller.applyCandidate(modelData.id) } }
                                            }
                                        }
                                    }
                                    Label { anchors.centerIn: parent; width: parent.width - 16; visible: candidatesList.count === 0; text: generating ? qsTr("正在生成，完成的候选会显示在这里。") : qsTr("当前句还没有候选。\n选择音色，点击上方“生成当前句”。"); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: Theme.secondaryText }
                                }
                                RowLayout {
                                    SubButton { text: qsTr("较新"); enabled: controller && !controller.modifying && controller.candidateOffset > 0; onClicked: controller.loadCandidates(Math.max(0, controller.candidateOffset - 50)) }
                                    SubButton { text: qsTr("较旧"); enabled: controller && !controller.modifying && controller.candidates.length === 50; onClicked: controller.loadCandidates(controller.candidateOffset + 50) }
                                }
                                SubButton { visible: localEngine.currentIndex === 2 && controller && Number(controller.flashPreview.reused || 0) > 0 && controller.flashPreview.rows && controller.flashPreview.rows.length === 0; text: qsTr("复用已有云端候选，不再次收费"); Layout.fillWidth: true; onClicked: if (commitPending()) controller.confirmFlash() }
                                SubButton { text: qsTr("补齐未应用句的有效候选"); Layout.fillWidth: true; enabled: controller && !controller.modifying && manuscript.count > 0; onClicked: if (commitPending()) controller.applySafeCandidates() }
                                Label { text: qsTr("批量应用保留人工裁切，跳过过期输入。"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 12 }
                            }

    Action { id: voicePlayAction; text: qsTr("播放 / 暂停"); property string keySequence: "Space"; enabled: previewPlayer && (previewPlayer.playing || previewPlayer.paused || previewPlayer.loading) || audio && !audio.busy && draftId !== ""; onTriggered: toggleAudition() }
    Action { id: voicePreviousAction; text: qsTr("上一句"); property string keySequence: "Up"; enabled: controller && manuscript.count > 0; onTriggered: navigate(-1) }
    Action { id: voiceNextAction; text: qsTr("下一句"); property string keySequence: "Down"; enabled: controller && manuscript.count > 0; onTriggered: navigate(1) }
    Action { id: voiceEditAction; text: qsTr("编辑当前句"); property string keySequence: "Return"; enabled: draftId !== ""; onTriggered: bodyEditor.forceActiveFocus() }
    Action { id: voiceGenerateAction; text: qsTr("生成当前句"); property string keySequence: "Ctrl+Return"; enabled: controller && !controller.modifying && !generating && draftId !== "" && !controller.selected.locked; onTriggered: generate("selected") }
    Action { id: voiceUndoAction; text: qsTr("撤销"); property string keySequence: "Ctrl+Z"; enabled: controller && controller.canUndo; onTriggered: if (commitPending()) controller.undo() }
    Action { id: voiceRedoAction; text: qsTr("重做"); property string keySequence: "Ctrl+Y"; enabled: controller && controller.canRedo; onTriggered: if (commitPending()) controller.redo() }
    Shortcut { sequence: voicePlayAction.keySequence; enabled: workspace.visible && !textEditing && !modalEditing && voicePlayAction.enabled; onActivated: voicePlayAction.trigger() }
    Shortcut { sequence: voicePreviousAction.keySequence; enabled: workspace.visible && !textEditing && !modalEditing && voicePreviousAction.enabled; onActivated: voicePreviousAction.trigger() }
    Shortcut { sequence: voiceNextAction.keySequence; enabled: workspace.visible && !textEditing && !modalEditing && voiceNextAction.enabled; onActivated: voiceNextAction.trigger() }
    Shortcut { sequence: "Shift+Up"; enabled: workspace.visible && !textEditing && !modalEditing; onActivated: navigate(-1, true) }
    Shortcut { sequence: "Shift+Down"; enabled: workspace.visible && !textEditing && !modalEditing; onActivated: navigate(1, true) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: workspace.visible && !textEditing && !modalEditing && voiceEditAction.enabled; onActivated: voiceEditAction.trigger() }
    Shortcut { sequences: ["Ctrl+Return", "Ctrl+Enter"]; enabled: workspace.visible && !textEditing && !modalEditing && voiceGenerateAction.enabled; onActivated: voiceGenerateAction.trigger() }
    Shortcut { sequence: voiceUndoAction.keySequence; enabled: workspace.visible && !textEditing && !modalEditing && voiceUndoAction.enabled; onActivated: voiceUndoAction.trigger() }
    Shortcut { sequences: [voiceRedoAction.keySequence, "Ctrl+Shift+Z"]; enabled: workspace.visible && !textEditing && !modalEditing && voiceRedoAction.enabled; onActivated: voiceRedoAction.trigger() }
    Shortcut { sequence: "Delete"; enabled: workspace.visible && !textEditing && !modalEditing && controller && !controller.modifying && draftId !== "" && !controller.selected.locked; onActivated: if (commitPending()) controller.previewDelete() }
    Shortcut { sequence: "Esc"; enabled: workspace.visible && !modalEditing && draftDirty && !bodyEditor.inputMethodComposing && !spokenEditor.inputMethodComposing && !notesEditor.inputMethodComposing; onActivated: { loadDraft(); manuscript.forceActiveFocus() } }
    Dialog {
        id: relinkDialog; anchors.centerIn: parent; modal: true; width: Math.min(600, workspace.width - 32); height: Math.min(400, workspace.height - 32)
        property var choices: []
        title: qsTr("明确选择字幕来源"); standardButtons: Dialog.Close
        contentItem: ListView {
            id: subtitleChoices; clip: true; model: relinkDialog.choices; ScrollBar.vertical: SubScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                width: subtitleChoices.width
                contentItem: Label { textFormat: Text.PlainText; text: modelData.text; wrapMode: Text.Wrap; color: Theme.text }
                onClicked: { controller.previewSubtitleRelink(modelData.id); relinkDialog.close() }
            }
        }
    }
    Dialog {
        id: historyDialog; anchors.centerIn: parent; modal: true; width: Math.min(760, workspace.width - 32); height: Math.min(540, workspace.height - 32)
        property int offset: 0
        property var rows: []
        property string cueId: ""
        property bool restoreCandidates: false
        title: qsTr("归档句子与生成历史"); standardButtons: Dialog.Close
        onClosed: { if (previewPlayer) previewPlayer.stop(); restoreCandidates = true; if (controller.modifying) controller.cancel(); else { restoreCandidates = false; controller.loadCandidates() } }
        contentItem: ColumnLayout {
            ListView {
                id: archivedList; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8; model: historyDialog.rows; ScrollBar.vertical: SubScrollBar {}
                delegate: ColumnLayout {
                    required property var modelData
                    width: archivedList.width
                    Label { textFormat: Text.PlainText; text: modelData.text; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { textFormat: Text.PlainText; text: modelData.manualSpoken ? qsTr("手工朗读：%1").arg(modelData.spoken) : ""; color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    SubButton { text: qsTr("读取该句的历史候选"); enabled: controller && !controller.modifying; onClicked: { historyDialog.cueId = modelData.id; controller.loadCandidates(0, modelData.id) } }
                }
            }
            Repeater {
                model: controller ? controller.candidates.filter(function(row) { return row.segmentId === historyDialog.cueId }) : []
                delegate: SubButton { required property var modelData; text: modelData.mock ? qsTr("试听历史 Mock 测试音") : qsTr("试听历史候选"); enabled: previewPlayer && !previewPlayer.loading; onClicked: previewPlayer.play(modelData.path, modelData.sha256) }
            }
            RowLayout {
                SubButton { text: qsTr("较新历史候选"); enabled: historyDialog.cueId !== "" && controller && !controller.modifying && controller.candidateOffset > 0; onClicked: controller.loadCandidates(Math.max(0, controller.candidateOffset - 50), historyDialog.cueId) }
                SubButton { text: qsTr("较旧历史候选"); enabled: historyDialog.cueId !== "" && controller && !controller.modifying && controller.candidates.length === 50; onClicked: controller.loadCandidates(controller.candidateOffset + 50, historyDialog.cueId) }
            }
            RowLayout {
                SubButton { text: qsTr("上一页归档"); enabled: historyDialog.offset > 0; onClicked: { historyDialog.offset = Math.max(0, historyDialog.offset - 50); historyDialog.rows = controller.archivedSegments(historyDialog.offset) } }
                SubButton { text: qsTr("下一页归档"); enabled: historyDialog.rows.length === 50; onClicked: { historyDialog.offset += 50; historyDialog.rows = controller.archivedSegments(historyDialog.offset) } }
            }
        }
    }
}




