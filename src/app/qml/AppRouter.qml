import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

ApplicationWindow {
    id: appWindow
    objectName: "mainWindow"
    width: 1440
    height: 900
    minimumWidth: Math.min(860, Screen.desktopAvailableWidth)
    minimumHeight: Math.min(560, Screen.desktopAvailableHeight - 64)
    visible: true
    title: (appRouter.workspace === "voice" ? qsTr("SubCue · AI 配音") : appRouter.workspace === "roughcut" ? qsTr("SubCue · 自动粗剪") : qsTr("SubCue · 自动字幕打轴"))
           + ((sharedProject && sharedProject.active ? sharedProject.modified
               : (appRouter.workspace === "roughcut" ? roughCut.modified : editor.modified)) ? " *" : "")
    flags: Qt.Window | (nativeTheme.expandedClientArea ? (Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint) : 0)
    color: nativeTheme.expandedClientArea ? Theme.titleBar : Theme.background
    topPadding: SafeArea.margins.top
    leftPadding: SafeArea.margins.left
    rightPadding: SafeArea.margins.right
    bottomPadding: SafeArea.margins.bottom
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    palette {
        window: Theme.background; windowText: Theme.text; base: Theme.input
        alternateBase: Theme.panelSecondary; text: Theme.text; button: Theme.button
        buttonText: Theme.text; highlight: Theme.selection; highlightedText: Theme.text
        placeholderText: Theme.placeholder; mid: Theme.divider; midlight: Theme.panelRaised
        dark: Theme.border; light: Theme.panelSecondary; shadow: Theme.divider
        toolTipBase: Theme.panelRaised; toolTipText: Theme.text
    }

    Component.onCompleted: {
        nativeTheme.applyDarkTitleBar(appWindow, Theme.titleBar, Theme.text, Theme.border)
        Qt.callLater(function() { if (sharedProject) { recoveryDialog.projects = sharedProject.recoverableProjects(); if (recoveryDialog.projects.length > 0) recoveryDialog.open() } })
    }
    onVisibilityChanged: if (visible) nativeTheme.applyDarkTitleBar(appWindow, Theme.titleBar, Theme.text, Theme.border)
    onClosing: function(close) {
        // 保存/打开进行中先等它完成再关闭：取消进行中的保存会让用户“刚保存过”却又看到未保存提示和失败弹窗。
        if (storageBusy) { close.accepted = false; closeAfterStorage = true; return }
        if (allowClose) {
            if (sharedProject) sharedProject.shutdown(true)
            roughCut.shutdown()
            editor.shutdown()
            return
        }
        if (!commitAllDrafts()) { close.accepted = false; return }
        if (editor.modified || roughCut.modified || (voiceController && voiceController.modified)) {
            close.accepted = false
            requestLeaveAll(function() {
                allowClose = true
                appWindow.close()
            })
            return
        }
        allowClose = true
        if (sharedProject) sharedProject.shutdown(true)
        roughCut.shutdown()
        editor.shutdown()
    }

    property alias newAction: newAction
    property alias saveAsAction: saveAsAction
    property alias exportAction: exportAction
    property alias importAction: importAction
    property alias scriptAction: scriptAction
    property alias openAction: openAction
    property alias saveAction: saveAction
    property int popupDepth: 0
    property bool allowClose: false
    readonly property var sharedProject: typeof projectSession === "undefined" ? null : projectSession
    readonly property var inferenceQueue: typeof gpuQueue === "undefined" ? null : gpuQueue
    readonly property var ttsController: typeof ttsTasks === "undefined" ? null : ttsTasks
    readonly property var voicesController: typeof voiceLibrary === "undefined" ? null : voiceLibrary
    readonly property var voiceController: typeof voiceStudio === "undefined" ? null : voiceStudio
    readonly property bool storageBusy: sharedProject ? sharedProject.busy : false
    property bool closeAfterStorage: false
    property url restoreSource: ""
    property string conversionWorkspace: "subtitle"
    property var pendingLeave: null
    function collectRecoveryDrafts() {
        if (!sharedProject || !sharedProject.unified) return
        const voice = voiceWorkspace.recoveryFields(), subtitle = subtitleWorkspace.recoveryFields()
        const drafts = sharedProject.recoveryDrafts || {}
        const next = ({})
        if (voice === null ? drafts.voice : voice.id) next.voice = voice === null ? drafts.voice : voice
        if (subtitle === null ? drafts.subtitle : subtitle.id) next.subtitle = subtitle === null ? drafts.subtitle : subtitle
        sharedProject.setRecoveryDrafts(next)
    }
    Timer { interval: 250; repeat: true; running: !appWindow.allowClose; onTriggered: appWindow.collectRecoveryDrafts() }
    property string fileWorkspace: "subtitle"
    readonly property bool modalEditing: popupDepth > 0 || unsavedDialog.visible || saveProjectDialog.visible || openProjectDialog.visible
        || resultDialog.visible || shortcutsDialog.visible || backupDialog.visible || backupFileDialog.visible
        || restoreSourceDialog.visible || restoreTargetDialog.visible || convertTargetDialog.visible || projectMenu.visible || gpuQueueMenu.visible || ttsTasksDialog.visible || voiceLibraryDialog.visible
        || (settingsWindow && settingsWindow.visible)
    Component { id: settingsWindowComponent; SettingsWindow { controller: editor } }
    property var settingsWindow: null
    function openSettings(categoryId, fieldId) {
        if (!settingsWindow) settingsWindow = settingsWindowComponent.createObject(appWindow)
        settingsWindow.route(categoryId || "asr", fieldId || "")
        settingsWindow.show()
        settingsWindow.requestActivate()
    }
    TtsTasksDialog { id: ttsTasksDialog; objectName: "ttsTasksDialog"; anchors.centerIn: parent; controller: appWindow.ttsController }
    VoiceLibraryDialog {
        transcriptRequired: voiceWorkspace.capabilities.referenceRequired !== true || voiceWorkspace.capabilities.transcriptRequired !== false
        id: voiceLibraryDialog; objectName: "voiceLibraryDialog"; anchors.centerIn: parent; controller: appWindow.voicesController; preview: typeof audioPreview === "undefined" ? null : audioPreview
        onUseVoice: function(versionId, displayName) { if (voiceController && voiceWorkspace.commitPending()) voiceWorkspace.assignVoice(versionId, voiceController.defaultVoiceVersionId === "" ? "default" : "selected", displayName) }
    }
    function controllerFor(workspace) { return workspace === "voice" ? voiceController : workspace === "roughcut" ? roughCut : editor }
    function commitAllDrafts() { return subtitleWorkspace.commitPendingCue() && voiceWorkspace.commitPending() }
    function cancelLeave() { pendingLeave = null }
    function beginLeave(workspaces, action) {
        if (pendingLeave || !commitAllDrafts()) return
        pendingLeave = { workspaces: workspaces, step: 0, action: action, waiting: false }
        advanceLeave()
    }
    function advanceLeave() {
        if (!pendingLeave) return
        while (pendingLeave.step < pendingLeave.workspaces.length) {
            const workspace = pendingLeave.workspaces[pendingLeave.step]
            if (controllerFor(workspace).modified) {
                unsavedDialog.workspace = workspace
                unsavedDialog.open()
                return
            }
            ++pendingLeave.step
        }
        const action = pendingLeave.action
        pendingLeave = null
        if (action) action()
    }
    function requestDestructiveAction(action) { beginLeave(["roughcut"], action) }
    function requestSubtitleDestructiveAction(action) { beginLeave(["subtitle"], action) }
    function requestLeaveAll(action) {
        const workspaces = appRouter.workspace === "roughcut" ? ["roughcut", "subtitle"] : ["subtitle", "roughcut"]
        if (voiceController) workspaces.push("voice")
        beginLeave(workspaces, action)
    }
    function requestSave(workspace, saveAs) {
        const controller = controllerFor(workspace)
        if (sharedProject && sharedProject.active) {
            if (!commitAllDrafts()) { cancelLeave(); return }
            subtitleWorkspace.captureSession()
            roughWorkspace.captureSession()
            voiceWorkspace.captureSession()
            appWindow.collectRecoveryDrafts()
            fileWorkspace = workspace
            if (!saveAs && sharedProject.path !== "") {
                if (!sharedProject.saveCurrentProject()) cancelLeave()
            } else {
                saveProjectDialog.nameFilters = sharedProject.unified ? [qsTr("SubCue 统一工程 (*.subcue-project)")]
                    : (sharedProject.legacyModule === "roughcut" ? [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")] : [qsTr("SubCue 字幕工程 (*.subcue)")])
                saveProjectDialog.defaultSuffix = sharedProject.unified ? "subcue-project"
                    : (sharedProject.legacyModule === "roughcut" ? "subcue-roughcut" : "subcue")
                saveProjectDialog.open()
            }
            return
        }
        if (workspace === "subtitle" && !subtitleWorkspace.commitPendingCue()) { cancelLeave(); return }
        (workspace === "subtitle" ? subtitleWorkspace : roughWorkspace).captureSession()
        if (!controller.canSave) { cancelLeave(); return }
        fileWorkspace = workspace
        if (!saveAs && controller.projectPath !== "") {
            if (!controller.saveCurrentProject()) cancelLeave()
        } else {
            saveProjectDialog.nameFilters = workspace === "roughcut"
                ? [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")] : [qsTr("SubCue 字幕工程 (*.subcue)")]
            saveProjectDialog.defaultSuffix = workspace === "roughcut" ? "subcue-roughcut" : "subcue"
            saveProjectDialog.open()
        }
    }
    function requestOpen(workspace) {
        fileWorkspace = workspace
        openProjectDialog.nameFilters = workspace === "voice" ? [qsTr("SubCue 统一工程 (*.subcue-project)")] : workspace === "roughcut"
            ? [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)"), qsTr("SubCue 统一工程 (*.subcue-project)")]
            : [qsTr("SubCue 字幕工程 (*.subcue)"), qsTr("SubCue 统一工程 (*.subcue-project)")]
        openProjectDialog.open()
    }
    function finishSave(workspace, success, path, message) {
        if (!success || !sharedProject || !sharedProject.active)
            showResult(success, path, message, function() { appWindow.requestSave(workspace, false) })
        if (!pendingLeave || !pendingLeave.waiting
            || pendingLeave.workspaces[pendingLeave.step] !== workspace) return
        if (!success || controllerFor(workspace).modified) { cancelLeave(); showResult(false, path, message || qsTr("保存期间产生了新编辑，请重新保存。"), function() { appWindow.requestSave(workspace, false) }); return }
        pendingLeave.waiting = false
        ++pendingLeave.step
        advanceLeave()
    }
    function showResult(success, path, message, retry) {
        resultDialog.title = success ? qsTr("操作完成") : qsTr("操作失败")
        resultDialog.path = path
        resultDialog.message = message
        resultDialog.retryAction = success ? null : (retry || null)
        // 关闭续接不插入额外的确认弹窗。
        if (!pendingLeave) resultDialog.open()
    }
    Connections {
        target: roughCut
        function onProjectSaveFinished(success, path, message) { if (!sharedProject || !sharedProject.active) appWindow.finishSave("roughcut", success, path, message) }
        function onProjectOpenFinished(success, path, message) { if ((!sharedProject || !sharedProject.active) && !path.toLowerCase().endsWith(".subcue-project")) appWindow.showResult(success, path, message, function() { appWindow.requestOpen("roughcut") }) }
        function onExportFinished(success, path, message) { appWindow.showResult(success, path, message, function() { roughWorkspace.requestExport() }) }
    }
    Connections {
        target: editor
        function onProjectSaveFinished(success, path, message) { if (!sharedProject || !sharedProject.active) appWindow.finishSave("subtitle", success, path, message) }
        function onProjectOpenFinished(success, path, message) { if ((!sharedProject || !sharedProject.active) && !path.toLowerCase().endsWith(".subcue-project")) appWindow.showResult(success, path, message, function() { appWindow.requestOpen("subtitle") }) }
        function onExportFinished(success, message, paths) { appWindow.showResult(success, paths.length ? paths[0] : "", message, function() { subtitleWorkspace.requestExport() }) }
    }
    Connections {
        target: sharedProject
        function onRecoveryRestored() {
            voiceWorkspace.restoreRecoveryFields(sharedProject.recoveryDrafts.voice)
            subtitleWorkspace.restoreRecoveryFields(sharedProject.recoveryDrafts.subtitle)
        }
        function onPreparingSave() {
            if (!appWindow.commitAllDrafts()) { sharedProject.cancel(); return }
            subtitleWorkspace.captureSession()
            roughWorkspace.captureSession()
            voiceWorkspace.captureSession()
            appWindow.collectRecoveryDrafts()
        }
        function onSaved(success, path, message) { if (!appWindow.allowClose) appWindow.finishSave(fileWorkspace, success, path, message) }
        function onOpened(success, path, message) { if (!appWindow.allowClose) appWindow.showResult(success, path, message) }
        function onBackedUp(success, path, message) { if (!appWindow.allowClose) appWindow.showResult(success, path, message) }
        function onBusyChanged() {
            if (!sharedProject.busy && closeAfterStorage) {
                closeAfterStorage = false
                appWindow.close()
            }
        }
    }
    Dialog {
        id: recoveryDialog; objectName: "recoveryDialog"; title: qsTr("恢复未保存的工作")
        property var projects: []
        anchors.centerIn: parent; width: Math.min(640, appWindow.width - 48); height: Math.min(420, appWindow.height - 64)
        modal: true; standardButtons: Dialog.Close
        contentItem: ListView {
            clip: true; model: recoveryDialog.projects; ScrollBar.vertical: SubScrollBar {}
            delegate: ItemDelegate {
                required property var modelData; width: ListView.view.width
                contentItem: ColumnLayout {
                    Label { Layout.fillWidth: true; text: modelData.path || qsTr("未命名工程"); wrapMode: Text.Wrap; color: Theme.text; textFormat: Text.PlainText }
                    Label { text: qsTr("恢复时间：%1").arg(new Date(modelData.time).toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm:ss")); color: Theme.secondaryText }
                    RowLayout {
                        SubButton { text: qsTr("恢复草稿"); onClicked: { const id = modelData.id; recoveryDialog.close(); appWindow.requestLeaveAll(function() { sharedProject.recoverProject(id) }) } }
                        SubButton { text: qsTr("放弃此恢复记录"); onClicked: { sharedProject.discardRecovery(modelData.id); recoveryDialog.projects = sharedProject.recoverableProjects() } }
                    }
                }
            }
        }
    }
    Dialog {
        id: unsavedDialog
        objectName: "roughCutUnsavedDialog"
        property string workspace: "roughcut"
        anchors.centerIn: parent
        modal: true
        title: qsTr("未保存的更改")
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        Label { text: unsavedDialog.workspace === "voice" ? qsTr("当前配音文稿有未保存的更改。") : unsavedDialog.workspace === "roughcut" ? qsTr("当前粗剪工程有未保存的更改。") : qsTr("当前字幕工程有未保存的更改。"); color: Theme.text }
        onAccepted: {
            if (!pendingLeave) return
            pendingLeave.waiting = true
            appWindow.requestSave(workspace, false)
        }
        onDiscarded: {
            if (!pendingLeave) return
            ++pendingLeave.step
            // Qt 在 discarded 回调返回后才关闭旧弹窗，下一次确认必须延后。
            Qt.callLater(appWindow.advanceLeave)
        }
        onRejected: cancelLeave()
    }
    FileDialog {
        id: saveProjectDialog
        objectName: "saveProjectDialog"
        title: qsTr("保存工程")
        fileMode: FileDialog.SaveFile
        parentWindow: appWindow
        onAccepted: {
            const success = sharedProject && sharedProject.active
                ? sharedProject.saveProject(selectedFile) : appWindow.controllerFor(fileWorkspace).saveProject(selectedFile)
            if (!success) appWindow.cancelLeave()
        }
        onRejected: appWindow.cancelLeave()
    }
    FileDialog {
        id: openProjectDialog
        title: qsTr("打开工程")
        parentWindow: appWindow
        onAccepted: {
            const file = selectedFile
            const workspace = fileWorkspace
            if (sharedProject && (sharedProject.active || file.toString().toLowerCase().endsWith(".subcue-project")))
                appWindow.requestLeaveAll(function() { appWindow.controllerFor(workspace).openProject(file) })
            else appWindow.beginLeave([workspace], function() { appWindow.controllerFor(workspace).openProject(file) })
        }
    }
    Dialog {
        id: backupDialog
        anchors.centerIn: parent; modal: true; title: qsTr("备份工程")
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ColumnLayout {
            Label { text: qsTr("备份包含已保存文稿、声音参考和生成历史。未保存的编辑不会包含在内。"); color: Theme.text; wrapMode: Text.Wrap; Layout.maximumWidth: 460 }
            SubCheckBox { id: portableBackup; text: qsTr("完整便携备份：同时复制外部媒体"); checked: false }
            Label { text: qsTr("引用模式保留外部媒体原路径；迁移电脑请选择完整备份。已有工程自带的资产始终包含。"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.maximumWidth: 460 }
        }
        onAccepted: backupFileDialog.open()
    }
    FileDialog {
        id: backupFileDialog; title: qsTr("保存备份到新文件"); fileMode: FileDialog.SaveFile
        defaultSuffix: "subcue-project"; nameFilters: [qsTr("SubCue 统一工程 (*.subcue-project)")]; parentWindow: appWindow
        onAccepted: sharedProject.backupProject(selectedFile, portableBackup.checked)
    }
    FileDialog {
        id: restoreSourceDialog; title: qsTr("选择待恢复的工程备份")
        nameFilters: [qsTr("SubCue 统一工程 (*.subcue-project *.sqlite)")]; parentWindow: appWindow
        onAccepted: { restoreSource = selectedFile; restoreTargetDialog.open() }
    }
    FileDialog {
        id: restoreTargetDialog; title: qsTr("恢复到新工程文件"); fileMode: FileDialog.SaveFile
        defaultSuffix: "subcue-project"; nameFilters: [qsTr("SubCue 统一工程 (*.subcue-project)")]; parentWindow: appWindow
        onAccepted: { const target = selectedFile; appWindow.requestLeaveAll(function() { sharedProject.restoreProject(restoreSource, target) }) }
    }
    FileDialog {
        id: convertTargetDialog; title: qsTr("转换为统一工程副本，保留原工程"); fileMode: FileDialog.SaveFile
        defaultSuffix: "subcue-project"; nameFilters: [qsTr("SubCue 统一工程 (*.subcue-project)")]; parentWindow: appWindow
        onAccepted: {
            const target = selectedFile
            const source = controllerFor(conversionWorkspace).projectPath
            appWindow.requestLeaveAll(function() { sharedProject.convertProject(source, conversionWorkspace, target) })
        }
    }
    Dialog {
        id: resultDialog
        objectName: "operationResultDialog"
        property string path: ""
        property string message: ""
        property var retryAction: null
        anchors.centerIn: parent
        modal: true
        width: Math.min(620, appWindow.width - 32)
        standardButtons: Dialog.Ok
        contentItem: ColumnLayout {
            Label { text: resultDialog.message; color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: resultDialog.path; visible: text !== ""; color: Theme.secondaryText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            SubButton { text: qsTr("重试"); visible: resultDialog.retryAction !== null; onClicked: {
                const action = resultDialog.retryAction
                resultDialog.close()
                if (action) action()
            } }
            SubButton { text: qsTr("打开文件夹"); visible: resultDialog.path !== ""; onClicked: {
                const path = resultDialog.path.replace(/\\/g, "/")
                Qt.openUrlExternally("file:///" + path.substring(0, path.lastIndexOf("/")).split("/").map(encodeURIComponent).join("/").replace(/^([A-Za-z])%3A/, "$1:"))
            } }
        }
    }
    Dialog {
        id: shortcutsDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("快捷键")
        width: Math.min(600, appWindow.width - 32)
        standardButtons: Dialog.Ok
        contentItem: ScrollView {
            implicitHeight: Math.min(520, appWindow.height - 160)
            Column {
                width: shortcutsDialog.availableWidth
                spacing: 8
                Repeater {
                    model: appWindow.fileActions.concat(appWindow.activeWorkspace().commandActions)
                    delegate: RowLayout {
                        required property var modelData
                        width: parent.width
                        Label { text: modelData.text; color: Theme.text; Layout.fillWidth: true }
                        Label { text: modelData.keySequence; color: Theme.secondaryText; font.family: "Consolas" }
                    }
                }
                Label { text: qsTr("检查区：Enter 应用 · Shift+Enter 换行 · Esc 撤回草稿"); color: Theme.secondaryText; wrapMode: Text.Wrap; width: parent.width }
            }
        }
    }

    readonly property bool fileCommandsEnabled: !editor.busy && !roughCut.busy
        && !modalEditing && !activeWorkspace().modalEditing
    function switchWorkspace(workspace) {
        if (!commitAllDrafts()) return
        activeWorkspace().captureSession()
        appRouter.switchTo(workspace)
    }
    readonly property var fileActions: [newAction, openAction, importAction, scriptAction, saveAction, saveAsAction, exportAction, subtitleAction, roughAction, voiceAction, settingsAction, helpAction, nextFocusAction, previousFocusAction]
    Action { id: importAction; text: qsTr("导入素材…"); property string keySequence: "Ctrl+I"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().openMediaDialog() }
    Shortcut { sequence: importAction.keySequence; enabled: importAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: importAction.trigger() }
    function activeWorkspace() { return appRouter.workspace === "voice" ? voiceWorkspace : appRouter.workspace === "roughcut" ? roughWorkspace : subtitleWorkspace }
    Action { id: newAction; text: qsTr("新建工程"); property string keySequence: "Ctrl+N"; enabled: !editor.busy && !roughCut.busy && !storageBusy; onTriggered: {
        if (sharedProject) appWindow.requestLeaveAll(function() { sharedProject.newProject() })
        else appWindow.beginLeave([appRouter.workspace], function() { appWindow.controllerFor(appRouter.workspace).newProject() })
    } }
    Shortcut { sequence: newAction.keySequence; enabled: newAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: newAction.trigger() }
    Action { id: openAction; text: qsTr("打开工程…"); property string keySequence: "Ctrl+O"; enabled: !editor.busy && !roughCut.busy && !storageBusy; onTriggered: appWindow.requestOpen(appRouter.workspace) }
    Shortcut { sequence: openAction.keySequence; enabled: openAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: openAction.trigger() }
    Action { id: saveAction; text: qsTr("保存工程"); property string keySequence: "Ctrl+S"; enabled: !editor.busy && !roughCut.busy && !storageBusy; onTriggered: appWindow.requestSave(appRouter.workspace, false) }
    Shortcut { sequence: saveAction.keySequence; enabled: saveAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: saveAction.trigger() }
    Action { id: saveAsAction; text: qsTr("另存为…"); property string keySequence: "Ctrl+Shift+S"; enabled: !editor.busy && !roughCut.busy && !storageBusy; onTriggered: appWindow.requestSave(appRouter.workspace, true) }
    Shortcut { sequence: saveAsAction.keySequence; enabled: saveAsAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: saveAsAction.trigger() }
    Action { id: scriptAction; text: qsTr("导入文稿…"); property string keySequence: "Ctrl+Shift+I"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().openScriptDialog() }
    Shortcut { sequence: scriptAction.keySequence; enabled: scriptAction.enabled && ((fileCommandsEnabled && !activeWorkspace().textEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: scriptAction.trigger() }
    Action { id: exportAction; text: qsTr("导出…"); property string keySequence: "Ctrl+E"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().requestExport() }
    Shortcut { sequence: exportAction.keySequence; enabled: exportAction.enabled && ((fileCommandsEnabled && !activeWorkspace().textEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: exportAction.trigger() }
    Action { id: subtitleAction; text: qsTr("切换字幕工作区"); property string keySequence: "Ctrl+1"; enabled: appRouter.canSwitch; onTriggered: appWindow.switchWorkspace("subtitle") }
    Shortcut { sequence: subtitleAction.keySequence; enabled: subtitleAction.enabled && (appRouter.canSwitch && !modalEditing && !activeWorkspace().modalEditing); onActivated: subtitleAction.trigger() }
    Action { id: roughAction; text: qsTr("切换粗剪工作区"); property string keySequence: "Ctrl+2"; enabled: appRouter.canSwitch; onTriggered: appWindow.switchWorkspace("roughcut") }
    Shortcut { sequence: roughAction.keySequence; enabled: roughAction.enabled && (appRouter.canSwitch && !modalEditing && !activeWorkspace().modalEditing); onActivated: roughAction.trigger() }
    Action { id: voiceAction; text: qsTr("切换配音工作区"); property string keySequence: "Ctrl+3"; enabled: voiceController && appRouter.canSwitch; onTriggered: appWindow.switchWorkspace("voice") }
    Shortcut { sequence: voiceAction.keySequence; enabled: voiceAction.enabled && !modalEditing && !activeWorkspace().modalEditing; onActivated: voiceAction.trigger() }
    Action { id: settingsAction; text: qsTr("设置…"); property string keySequence: "Ctrl+,"; onTriggered: appWindow.openSettings() }
    Shortcut { sequence: settingsAction.keySequence; enabled: !modalEditing && !activeWorkspace().modalEditing; onActivated: settingsAction.trigger() }
    Action { id: helpAction; text: qsTr("快捷键帮助"); property string keySequence: "F1"; enabled: true; onTriggered: shortcutsDialog.open() }
    Shortcut { sequence: helpAction.keySequence; enabled: helpAction.enabled && ((!modalEditing && !activeWorkspace().modalEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: helpAction.trigger() }
    Action { id: nextFocusAction; text: qsTr("下一编辑区域"); property string keySequence: "F6"; enabled: true; onTriggered: activeWorkspace().cycleFocus(1) }
    Shortcut { sequence: nextFocusAction.keySequence; enabled: nextFocusAction.enabled && ((!modalEditing && !activeWorkspace().modalEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: nextFocusAction.trigger() }
    Action { id: previousFocusAction; text: qsTr("上一编辑区域"); property string keySequence: "Shift+F6"; enabled: true; onTriggered: activeWorkspace().cycleFocus(-1) }
    Shortcut { sequence: previousFocusAction.keySequence; enabled: previousFocusAction.enabled && ((!modalEditing && !activeWorkspace().modalEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: previousFocusAction.trigger() }

    Rectangle {
        id: workspaceBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40
        color: Theme.menuBar
        z: 20
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.divider }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 4
            Label { text: qsTr("SubCue"); color: Theme.text; font.bold: true; Layout.rightMargin: 12 }
            MenuButton {
                text: qsTr("工程"); visible: sharedProject !== null
                onClicked: projectMenu.popup(this, 0, height)
                SubMenu {
                    id: projectMenu
                    MenuItem { text: qsTr("配音任务记录…"); enabled: ttsController && sharedProject && sharedProject.unified && !storageBusy; onTriggered: ttsTasksDialog.open() }
                    MenuItem { text: qsTr("声音库…"); enabled: voicesController !== null; onTriggered: voiceLibraryDialog.open() }
                    MenuItem { text: qsTr("备份已保存的统一工程…"); enabled: sharedProject && sharedProject.unified && sharedProject.path !== "" && !storageBusy; onTriggered: backupDialog.open() }
                    MenuItem { text: qsTr("恢复工程备份…"); enabled: !storageBusy; onTriggered: restoreSourceDialog.open() }
                    MenuItem { text: qsTr("恢复未保存的草稿…"); enabled: !storageBusy; onTriggered: { recoveryDialog.projects = sharedProject.recoverableProjects(); recoveryDialog.open() } }
                    MenuItem {
                        text: qsTr("转换当前旧工程为统一副本…")
                        enabled: sharedProject && sharedProject.active && !sharedProject.unified && !storageBusy
                        onTriggered: {
                            conversionWorkspace = sharedProject.legacyModule
                            if (controllerFor(conversionWorkspace).modified) {
                                appWindow.showResult(false, controllerFor(conversionWorkspace).projectPath, qsTr("请先保存当前工程，再转换为统一副本。"), function() { appWindow.requestSave(conversionWorkspace, false) })
                            } else convertTargetDialog.open()
                        }
                    }
                }
            }
            Label { text: sharedProject && sharedProject.unified ? (sharedProject.path === "" ? qsTr("未命名工程") : sharedProject.path.replace(/\\/g, "/").split("/").pop()) : qsTr("旧格式工程"); visible: sharedProject !== null; color: Theme.secondaryText }
            Label { text: sharedProject ? sharedProject.statusText : ""; visible: text !== ""; color: Theme.secondaryText; elide: Text.ElideRight; Layout.fillWidth: true; Layout.maximumWidth: 380; Layout.minimumWidth: 0 }
            SubButton { text: qsTr("取消"); visible: storageBusy; onClicked: sharedProject.cancel() }
            SubButton { text: qsTr("取消等待保存"); visible: sharedProject && sharedProject.savePending; onClicked: sharedProject.cancelPendingSave() }
            MenuButton {
                objectName: "gpuQueueButton"
                text: inferenceQueue ? qsTr("GPU 任务 %1").arg(inferenceQueue.tasks.length) : ""
                visible: inferenceQueue !== null && inferenceQueue.tasks.length > 0
                onClicked: gpuQueueMenu.popup(this, 0, height)
                SubMenu {
                    id: gpuQueueMenu
                    objectName: "gpuQueueMenu"
                    Repeater {
                        model: inferenceQueue ? inferenceQueue.tasks : []
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.title + (modelData.state === "running" ? qsTr(" · 执行中") : qsTr(" · 排队 %1").arg(modelData.position))
                            enabled: false
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: voiceController ? [{ key: "subtitle", label: qsTr("字幕编辑") }, { key: "roughcut", label: qsTr("自动粗剪") }, { key: "voice", label: qsTr("AI 配音") }] : [{ key: "subtitle", label: qsTr("字幕编辑") }, { key: "roughcut", label: qsTr("自动粗剪") }]
                delegate: MenuButton {
                    required property var modelData
                    text: modelData.label
                    enabled: appRouter.canSwitch
                    font.bold: appRouter.workspace === modelData.key
                    onClicked: appWindow.switchWorkspace(modelData.key)
                    background: Rectangle {
                        color: parent.hovered ? Theme.menuButtonHover : "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                            height: 2; color: Theme.accent; visible: appRouter.workspace === modelData.key
                        }
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ? modelData.label + " · Ctrl+" + (modelData.key === "voice" ? "3" : modelData.key === "roughcut" ? "2" : "1") : qsTr("任务进行中，暂时不能切换工作区")
                }
            }
            MenuButton { objectName: "globalSettingsButton"; text: qsTr("设置"); onClicked: appWindow.openSettings(); ToolTip.visible: hovered; ToolTip.text: qsTr("设置 · Ctrl+,") }
            BusyIndicator {
                running: editor.busy || roughCut.busy || storageBusy
                visible: running
                implicitWidth: 24; implicitHeight: 24
                palette.dark: Theme.accent
            }
        }
    }

    StackLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: workspaceBar.bottom
        anchors.bottom: parent.bottom
        currentIndex: appRouter.workspace === "voice" ? 2 : appRouter.workspace === "roughcut" ? 1 : 0
        SubtitleWorkspace { id: subtitleWorkspace; externalModalEditing: appWindow.modalEditing }
        RoughCutWorkspace { id: roughWorkspace; externalModalEditing: appWindow.modalEditing }
        VoiceWorkspace { id: voiceWorkspace; externalModalEditing: appWindow.modalEditing; onRequestVoiceLibrary: voiceLibraryDialog.open(); onRequestVoiceCreation: voiceLibraryDialog.openAdd() }
    }
}
