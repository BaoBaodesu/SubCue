import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    property var host
    property var controller
    property var player
    id: page
    property int standardButtons: Dialog.Close
    property bool modal: false
    signal opened()
    signal closed()
    signal accepted()
    signal rejected()
    parent: host ? host.contentItem : null
    anchors.fill: parent
    visible: host && host.activePage === page
    function open() { host.activePage = page; opened() }
    function close() { host.activePage = null; closed() }
    background: Rectangle { color: Theme.background }
    header: RowLayout {
        SubButton { text: qsTr("返回声音库"); onClicked: page.close() }
        Label { text: page.title; color: Theme.text; font.bold: true; Layout.fillWidth: true }
    }
    footer: ColumnLayout {
        VoicePreviewTransport { sourceTitle: page.title + qsTr(" · 试听"); player: page.player; preparing: controller && controller.previewLoading; onStopRequested: if (controller) controller.cancelPreview() }
        Flow {
            Layout.fillWidth: true; spacing: 8
            SubButton { text: qsTr("取消"); onClicked: { page.rejected(); page.close() } }
            SubButton { visible: (page.standardButtons & Dialog.Ok) !== 0; text: qsTr("保存"); onClicked: { page.accepted(); page.close() } }
        }
    }
}
