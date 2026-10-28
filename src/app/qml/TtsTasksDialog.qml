import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SubCue

Dialog {
    id: dialog
    property var controller
    property int offset: 0
    property string batchFilter: ""
    title: qsTr("配音任务记录")
    modal: true
    width: Math.min(820, parent ? parent.width - 32 : 820)
    standardButtons: Dialog.Close
    function refresh() { if (controller) controller.loadRecords(offset, 50, batchFilter) }
    onOpened: { offset = 0; batchFilter = ""; refresh() }
    function stateText(state) {
        const labels = { queued: qsTr("排队"), running: qsTr("生成中"), validating: qsTr("校验中"), completed: qsTr("已生成候选"),
            failed: qsTr("失败"), canceled: qsTr("已取消"), interrupted: qsTr("中断"), outcome_unknown: qsTr("结果不确定") }
        return labels[state] || state
    }
    contentItem: ColumnLayout {
        spacing: 10
        Label { text: qsTr("中断或结果不确定的任务不会自动重发。生成候选不代表已应用到文稿。"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
        Label { text: controller ? controller.recordsError : ""; visible: text !== ""; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
        ListView {
            id: list
            objectName: "ttsTaskRecordsList"
            Layout.fillWidth: true
            Layout.preferredHeight: 380
            clip: true
            model: controller ? controller.records : []
            spacing: 4
            ScrollBar.vertical: SubScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: list.width
                height: content.implicitHeight + 20
                color: Theme.panelSecondary
                ColumnLayout {
                    id: content
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: dialog.stateText(modelData.state); color: Theme.text; font.bold: true }
                        Label { text: modelData.provider + " · " + modelData.id.substring(0, 8); color: Theme.secondaryText; Layout.fillWidth: true; elide: Text.ElideRight }
                    }
                    Label { text: modelData.input.spokenText || modelData.input.text || qsTr("旧任务输入"); color: Theme.text; Layout.fillWidth: true; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
                    Label { text: modelData.diagnostics.message || ""; visible: text !== ""; color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Label { visible: !!modelData.cloud; textFormat: Text.PlainText; text: modelData.usage.state === "known" && typeof modelData.usage.yuan === "number" ? qsTr("实际 Token：输入 %1，输出 %2；按当时原价折算 ¥%3。请求 %4").arg(modelData.usage.inputTokens).arg(modelData.usage.outputTokens).arg(Number(modelData.usage.yuan).toFixed(6)).arg(modelData.usage.requestId || "") : qsTr("实际用量或计价资料未知，不按零计算，不自动重发。"); color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Label { text: modelData.result.quality && modelData.result.quality.warnings ? modelData.result.quality.warnings.join(" ") : ""; visible: text !== ""; color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    SubButton { text: modelData.batchId ? qsTr("查看批次 %1").arg(modelData.batchId.substring(0, 8)) : qsTr("旧记录未分组"); enabled: modelData.batchId !== "" && controller && !controller.recordsLoading; onClicked: { dialog.batchFilter = modelData.batchId; dialog.offset = 0; dialog.refresh() } }
                }
            }
            Label { anchors.centerIn: parent; visible: list.count === 0; text: controller && controller.recordsLoading ? qsTr("读取任务…") : qsTr("本页没有任务记录"); color: Theme.secondaryText }
        }
        RowLayout {
            SubButton { text: qsTr("上一页"); enabled: offset > 0 && controller && !controller.recordsLoading; onClicked: { offset = Math.max(0, offset - 50); refresh() } }
            SubButton { text: qsTr("下一页"); enabled: list.count === 50 && controller && !controller.recordsLoading; onClicked: { offset += 50; refresh() } }
            SubButton { text: qsTr("刷新"); enabled: controller && !controller.recordsLoading; onClicked: refresh() }
            SubButton { text: qsTr("全部批次"); visible: batchFilter !== ""; enabled: controller && !controller.recordsLoading; onClicked: { batchFilter = ""; offset = 0; refresh() } }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: controller && controller.recordsLoading; visible: running; implicitWidth: 24; implicitHeight: 24 }
        }
    }
}
