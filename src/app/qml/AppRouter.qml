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
    minimumWidth: 1100
    minimumHeight: 700
    visible: true
    title: (appRouter.workspace === "roughcut" ? qsTr("SubCue · 自动粗剪") : qsTr("SubCue · 自动字幕打轴"))
           + (roughCut.modified ? " *" : "")
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
        if (allowClose || !roughCut.modified) {
            roughCut.shutdown()
            editor.shutdown()
            return
        }
        close.accepted = false
        requestDestructiveAction(function() {
            allowClose = true
            appWindow.close()
        })
    }

    property bool allowClose: false
    property var pendingUnsavedAction: null
    function requestDestructiveAction(action) {
        if (!roughCut.modified) {
            action()
            return
        }
        pendingUnsavedAction = action
        unsavedDialog.open()
    }
    function runPendingUnsavedAction() {
        const action = pendingUnsavedAction
        pendingUnsavedAction = null
        if (action) action()
    }
    Dialog {
        id: unsavedDialog
        objectName: "roughCutUnsavedDialog"
        anchors.centerIn: parent
        modal: true
        title: qsTr("未保存的更改")
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        Label { text: qsTr("当前粗剪工程有未保存的更改。"); color: Theme.text }
        onAccepted: {
            if (roughCut.projectPath !== "") {
                if (roughCut.saveCurrentProject()) runPendingUnsavedAction()
                else pendingUnsavedAction = null
            } else {
                saveThenContinue = true
                saveProjectDialog.open()
            }
        }
        onDiscarded: runPendingUnsavedAction()
        onRejected: pendingUnsavedAction = null
    }
    property bool saveThenContinue: false
    FileDialog {
        id: saveProjectDialog
        title: qsTr("保存粗剪工程")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "subcue-roughcut"
        nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]
        parentWindow: appWindow
        onAccepted: {
            if (!roughCut.saveProject(selectedFile)) {
                saveThenContinue = false
                pendingUnsavedAction = null
                return
            }
            if (saveThenContinue) {
                saveThenContinue = false
                runPendingUnsavedAction()
            }
        }
        onRejected: {
            saveThenContinue = false
            pendingUnsavedAction = null
        }
    }

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
                    onClicked: appRouter.switchTo(modelData.key)
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
        SubtitleWorkspace { }
        RoughCutWorkspace { }
    }
}
