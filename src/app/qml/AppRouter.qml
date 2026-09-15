import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SubCue

ApplicationWindow {
    id: appWindow
    objectName: "mainWindow"
    width: 1440
    height: 900
    minimumWidth: 1100
    minimumHeight: 700
    visible: true
    title: appRouter.workspace === "roughcut" ? qsTr("SubCue · 自动粗剪") : qsTr("SubCue · 自动字幕打轴")
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
    onClosing: editor.shutdown()

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
