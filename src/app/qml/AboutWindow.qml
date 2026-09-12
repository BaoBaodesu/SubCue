import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: aboutWindow
    objectName: "aboutWindow"
    width: 520
    height: 420
    minimumWidth: 480
    minimumHeight: 360
    title: qsTr("关于 SubCue")
    color: Theme.background
    modality: Qt.ApplicationModal
    flags: Qt.Dialog

    Component.onCompleted: {
        if (typeof nativeTheme !== "undefined" && nativeTheme)
            nativeTheme.applyDarkTitleBar(aboutWindow, Theme.titleBar, Theme.text, Theme.border)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Label {
                text: qsTr("SubCue")
                color: Theme.text
                font.family: Theme.fontFamily
                font.bold: true
                font.pixelSize: 18
            }
            Label {
                text: qsTr("版本 %1").arg(Qt.application.version)
                color: Theme.muted
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("自动字幕打轴工具。本程序动态链接 Qt（LGPLv3）与 FFmpeg（LGPLv2.1）。FFmpeg 库保留原名，不包含 GPL/nonfree 编码器。")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("本程序已集成 whisper.cpp 本地识别运行时；模型由用户按需下载。完整第三方许可证在程序目录的 licenses 文件夹。")
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ScrollBar.vertical: SubScrollBar { }
                TextArea {
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.muted
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    text: qsTr("发布包随带 Qt、FFmpeg 与 MSVC 运行库，不需要 Python、Visual Studio 或外部 FFmpeg 命令行工具。对应源码获取方式见 licenses/NOTICE.txt。")
                    background: Rectangle {
                        color: Theme.panel
                        border.color: Theme.border
                        radius: Theme.radiusInput
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PrimaryButton { text: qsTr("关闭"); onClicked: aboutWindow.hide() }
            }
        }
    }
}
