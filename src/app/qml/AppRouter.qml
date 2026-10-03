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
    minimumWidth: Math.min(1100, Screen.desktopAvailableWidth)
    minimumHeight: Math.min(700, Screen.desktopAvailableHeight - 64)
    visible: true
    title: (appRouter.workspace === "roughcut" ? qsTr("SubCue · 自动粗剪") : qsTr("SubCue · 自动字幕打轴"))
           + ((appRouter.workspace === "roughcut" ? roughCut.modified : editor.modified) ? " *" : "")
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

    Component.onCompleted: nativeTheme.applyDarkTitleBar(appWindow, Theme.titleBar, Theme.text, Theme.border)
    onVisibilityChanged: if (visible) nativeTheme.applyDarkTitleBar(appWindow, Theme.titleBar, Theme.text, Theme.border)
    onClosing: function(close) {
        if (allowClose) {
            roughCut.shutdown()
            editor.shutdown()
            return
        }
        if (!subtitleWorkspace.commitPendingCue()) { close.accepted = false; return }
        if (editor.modified || roughCut.modified) {
            close.accepted = false
            requestLeaveAll(function() {
                allowClose = true
                appWindow.close()
            })
            return
        }
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
    property var pendingLeave: null
    property string fileWorkspace: "subtitle"
    readonly property bool modalEditing: popupDepth > 0 || unsavedDialog.visible || saveProjectDialog.visible || openProjectDialog.visible
        || resultDialog.visible || shortcutsDialog.visible
    function controllerFor(workspace) { return workspace === "roughcut" ? roughCut : editor }
    function cancelLeave() { pendingLeave = null }
    function beginLeave(workspaces, action) {
        if (pendingLeave || (workspaces.indexOf("subtitle") >= 0 && !subtitleWorkspace.commitPendingCue())) return
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
        beginLeave(appRouter.workspace === "roughcut" ? ["roughcut", "subtitle"] : ["subtitle", "roughcut"], action)
    }
    function requestSave(workspace, saveAs) {
        const controller = controllerFor(workspace)
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
        openProjectDialog.nameFilters = workspace === "roughcut"
            ? [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")] : [qsTr("SubCue 字幕工程 (*.subcue)")]
        openProjectDialog.open()
    }
    function finishSave(workspace, success, path, message) {
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
        function onProjectSaveFinished(success, path, message) { appWindow.finishSave("roughcut", success, path, message) }
        function onProjectOpenFinished(success, path, message) { appWindow.showResult(success, path, message, function() { appWindow.requestOpen("roughcut") }) }
        function onExportFinished(success, path, message) { appWindow.showResult(success, path, message, function() { roughWorkspace.requestExport() }) }
    }
    Connections {
        target: editor
        function onProjectSaveFinished(success, path, message) { appWindow.finishSave("subtitle", success, path, message) }
        function onProjectOpenFinished(success, path, message) { appWindow.showResult(success, path, message, function() { appWindow.requestOpen("subtitle") }) }
        function onExportFinished(success, message, paths) { appWindow.showResult(success, paths.length ? paths[0] : "", message, function() { subtitleWorkspace.requestExport() }) }
    }
    Dialog {
        id: unsavedDialog
        objectName: "roughCutUnsavedDialog"
        property string workspace: "roughcut"
        anchors.centerIn: parent
        modal: true
        title: qsTr("未保存的更改")
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        Label { text: unsavedDialog.workspace === "roughcut" ? qsTr("当前粗剪工程有未保存的更改。") : qsTr("当前字幕工程有未保存的更改。"); color: Theme.text }
        onAccepted: {
            if (!pendingLeave) return
            pendingLeave.waiting = true
            appWindow.requestSave(workspace, false)
        }
        onDiscarded: {
            if (!pendingLeave) return
            ++pendingLeave.step
            advanceLeave()
        }
        onRejected: cancelLeave()
    }
    FileDialog {
        id: saveProjectDialog
        title: qsTr("保存工程")
        fileMode: FileDialog.SaveFile
        parentWindow: appWindow
        onAccepted: if (!appWindow.controllerFor(fileWorkspace).saveProject(selectedFile)) appWindow.cancelLeave()
        onRejected: appWindow.cancelLeave()
    }
    FileDialog {
        id: openProjectDialog
        title: qsTr("打开工程")
        parentWindow: appWindow
        onAccepted: {
            const file = selectedFile
            const workspace = fileWorkspace
            appWindow.beginLeave([workspace], function() { appWindow.controllerFor(workspace).openProject(file) })
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
        if (!subtitleWorkspace.commitPendingCue()) return
        activeWorkspace().captureSession()
        appRouter.switchTo(workspace)
    }
    readonly property var fileActions: [newAction, openAction, importAction, scriptAction, saveAction, saveAsAction, exportAction, subtitleAction, roughAction, helpAction, nextFocusAction, previousFocusAction]
    Action { id: importAction; text: qsTr("导入素材…"); property string keySequence: "Ctrl+I"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().openMediaDialog() }
    Shortcut { sequence: importAction.keySequence; enabled: importAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: importAction.trigger() }
    function activeWorkspace() { return appRouter.workspace === "roughcut" ? roughWorkspace : subtitleWorkspace }
    Action { id: newAction; text: qsTr("新建工程"); property string keySequence: "Ctrl+N"; enabled: !editor.busy && !roughCut.busy; onTriggered: appWindow.beginLeave([appRouter.workspace], function() { appWindow.controllerFor(appRouter.workspace).newProject() } ) }
    Shortcut { sequence: newAction.keySequence; enabled: newAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: newAction.trigger() }
    Action { id: openAction; text: qsTr("打开工程…"); property string keySequence: "Ctrl+O"; enabled: !editor.busy && !roughCut.busy; onTriggered: appWindow.requestOpen(appRouter.workspace) }
    Shortcut { sequence: openAction.keySequence; enabled: openAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: openAction.trigger() }
    Action { id: saveAction; text: qsTr("保存工程"); property string keySequence: "Ctrl+S"; enabled: !editor.busy && !roughCut.busy; onTriggered: appWindow.requestSave(appRouter.workspace, false) }
    Shortcut { sequence: saveAction.keySequence; enabled: saveAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: saveAction.trigger() }
    Action { id: saveAsAction; text: qsTr("另存为…"); property string keySequence: "Ctrl+Shift+S"; enabled: !editor.busy && !roughCut.busy; onTriggered: appWindow.requestSave(appRouter.workspace, true) }
    Shortcut { sequence: saveAsAction.keySequence; enabled: saveAsAction.enabled && ((fileCommandsEnabled) && !modalEditing && !activeWorkspace().modalEditing); onActivated: saveAsAction.trigger() }
    Action { id: scriptAction; text: qsTr("导入文稿…"); property string keySequence: "Ctrl+Shift+I"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().openScriptDialog() }
    Shortcut { sequence: scriptAction.keySequence; enabled: scriptAction.enabled && ((fileCommandsEnabled && !activeWorkspace().textEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: scriptAction.trigger() }
    Action { id: exportAction; text: qsTr("导出…"); property string keySequence: "Ctrl+E"; enabled: !editor.busy && !roughCut.busy; onTriggered: activeWorkspace().requestExport() }
    Shortcut { sequence: exportAction.keySequence; enabled: exportAction.enabled && ((fileCommandsEnabled && !activeWorkspace().textEditing) && !modalEditing && !activeWorkspace().modalEditing); onActivated: exportAction.trigger() }
    Action { id: subtitleAction; text: qsTr("切换字幕工作区"); property string keySequence: "Ctrl+1"; enabled: !editor.busy && !roughCut.busy && appRouter.canSwitch; onTriggered: appWindow.switchWorkspace("subtitle") }
    Shortcut { sequence: subtitleAction.keySequence; enabled: subtitleAction.enabled && ((fileCommandsEnabled && appRouter.canSwitch) && !modalEditing && !activeWorkspace().modalEditing); onActivated: subtitleAction.trigger() }
    Action { id: roughAction; text: qsTr("切换粗剪工作区"); property string keySequence: "Ctrl+2"; enabled: !editor.busy && !roughCut.busy && appRouter.canSwitch; onTriggered: appWindow.switchWorkspace("roughcut") }
    Shortcut { sequence: roughAction.keySequence; enabled: roughAction.enabled && ((fileCommandsEnabled && appRouter.canSwitch) && !modalEditing && !activeWorkspace().modalEditing); onActivated: roughAction.trigger() }
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
            Item { Layout.fillWidth: true }
            Repeater {
                model: [{ key: "subtitle", label: qsTr("字幕编辑") }, { key: "roughcut", label: qsTr("自动粗剪") }]
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
                    ToolTip.visible: hovered && !enabled
                    ToolTip.text: qsTr("任务进行中，暂时不能切换工作区")
                }
            }
            BusyIndicator {
                running: editor.busy || roughCut.busy
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
        currentIndex: appRouter.workspace === "roughcut" ? 1 : 0
        SubtitleWorkspace { id: subtitleWorkspace; externalModalEditing: appWindow.modalEditing }
        RoughCutWorkspace { id: roughWorkspace; externalModalEditing: appWindow.modalEditing }
    }
}
