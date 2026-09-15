import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: window
    objectName: "roughCutWorkspace"
    property string filter: "ALL"

    function formatTime(milliseconds) {
        const value = Math.max(0, Math.round(milliseconds))
        const minutes = Math.floor(value / 60000)
        const seconds = Math.floor(value / 1000) % 60
        const millis = value % 1000
        return String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0")
               + "." + String(millis).padStart(3, "0")
    }
    function adjustZoom(timeline, delta) {
        if (!timeline || timeline.durationUs <= 0) return
        const percent = Math.max(100, Math.min(10000, timeline.zoomPercent + delta))
        const totalMs = Math.max(1, timeline.durationUs / 1000)
        const center = Math.max(0, Math.min(1, (timeline.scrollOffset + timeline.width / 2) / timeline.pixelsPerMs / totalMs))
        const span = 100 / percent
        const start = Math.max(0, Math.min(1 - span, center - span / 2))
        timeline.setVisibleRange(start, Math.min(1, start + span))
    }

    FileDialog { id: openProjectDialog; title: qsTr("打开粗剪工程"); nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: roughCut.openProject(selectedFile) }
    FileDialog { id: saveProjectDialog; title: qsTr("保存粗剪工程"); fileMode: FileDialog.SaveFile; defaultSuffix: "subcue-roughcut"; nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: roughCut.saveProject(selectedFile) }
    FileDialog { id: mediaDialog; title: qsTr("选择 WAV 音频"); nameFilters: [qsTr("WAV 音频 (*.wav)")]; parentWindow: window.Window.window; onAccepted: roughCut.loadMedia(selectedFile) }
    FileDialog { id: scriptDialog; title: qsTr("选择参考文案"); nameFilters: [qsTr("参考文案 (*.txt *.docx)")]; parentWindow: window.Window.window; onAccepted: roughCut.loadScript(selectedFile) }
    FileDialog { id: exportDialog; title: qsTr("导出 FCP7 XML"); fileMode: FileDialog.SaveFile; defaultSuffix: "xml"; nameFilters: [qsTr("FCP7 XML (*.xml)")]; parentWindow: window.Window.window; onAccepted: roughCut.exportXml(selectedFile) }

    DropArea {
        anchors.fill: parent
        z: 50
        keys: ["text/uri-list"]
        onDropped: function(drop) {
            if (!drop.urls || drop.urls.length === 0) return
            const url = drop.urls[0]
            if (String(url).toLowerCase().endsWith(".wav")) {
                roughCut.loadMedia(url)
                drop.acceptProposedAction()
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: parent.containsDrag
            color: "#994C8DFF"
            border.width: 2
            border.color: Theme.accentHover
            Label { anchors.centerIn: parent; text: qsTr("松开以导入 WAV 音频"); color: Theme.text; font.pixelSize: 18; font.bold: true }
        }
    }

    Rectangle {
        id: commandBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 40; color: Theme.menuBar; z: 10
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.divider }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 4
            MenuButton { text: qsTr("导入音频…"); enabled: !roughCut.busy; onClicked: mediaDialog.open() }
            MenuButton { text: qsTr("导入文案…"); enabled: !roughCut.busy; onClicked: scriptDialog.open() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("打开工程"); enabled: !roughCut.busy; onClicked: openProjectDialog.open() }
            MenuButton { text: qsTr("保存工程"); enabled: roughCut.resultCount > 0 && !roughCut.busy; onClicked: saveProjectDialog.open() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("撤销"); enabled: roughCut.canUndo; onClicked: roughCut.undo() }
            MenuButton { text: qsTr("重做"); enabled: roughCut.canRedo; onClicked: roughCut.redo() }
            Item { Layout.fillWidth: true }
            MenuButton { text: qsTr("开始分析"); enabled: roughCut.mediaPath !== "" && !roughCut.busy; onClicked: roughCut.startAnalysis() }
            MenuButton { text: qsTr("辅助识别"); enabled: roughCut.resultCount > 0 && !roughCut.busy; onClicked: roughCut.startAuxiliaryRecognition() }
            MenuButton { text: qsTr("取消"); enabled: roughCut.busy; onClicked: roughCut.cancelAnalysis() }
            PrimaryButton { text: qsTr("导出 XML"); enabled: roughCut.resultCount > 0 && !roughCut.busy; onClicked: exportDialog.open() }
        }
    }

    SplitView {
        id: verticalSplit
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: commandBar.bottom; anchors.bottom: statusBar.top
        orientation: Qt.Vertical
        handle: Rectangle { implicitWidth: 5; implicitHeight: 5; color: Theme.divider }

        SplitView {
            id: workspaceSplit
            orientation: Qt.Horizontal
            SplitView.preferredHeight: verticalSplit.height * 0.65
            SplitView.minimumHeight: 300
            handle: Rectangle { implicitWidth: 5; implicitHeight: 5; color: Theme.divider }

            Rectangle {
                color: Theme.panel; border.color: Theme.border
                SplitView.preferredWidth: Math.max(240, workspaceSplit.width * 0.2)
                SplitView.minimumWidth: 220; SplitView.maximumWidth: 360
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 42
                        color: Theme.panelHeader
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 4
                            MenuButton {
                                text: qsTr("项目：音频")
                                highlighted: roughSourceTabs.currentIndex === 0
                                onClicked: roughSourceTabs.currentIndex = 0
                            }
                            MenuButton {
                                text: qsTr("参考文案")
                                highlighted: roughSourceTabs.currentIndex === 1
                                onClicked: roughSourceTabs.currentIndex = 1
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: (roughCut.mediaPath !== "" ? 1 : 0) + qsTr(" 项")
                                color: Theme.muted
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                    StackLayout {
                        id: roughSourceTabs
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: 0

                        ColumnLayout {
                            spacing: 0
                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Label {
                                    anchors.centerIn: parent
                                    width: Math.max(100, parent.width - 32)
                                    text: roughCut.mediaPath !== ""
                                          ? roughCut.mediaPath
                                          : qsTr("双击此处导入 WAV 音频\n或从资源管理器拖入文件")
                                    color: Theme.muted
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    elide: roughCut.mediaPath !== "" ? Text.ElideMiddle : Text.ElideNone
                                    lineHeight: 1.6
                                }
                                TapHandler { onDoubleTapped: mediaDialog.open() }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                Layout.topMargin: 8
                                Layout.bottomMargin: 10
                                SubButton { text: qsTr("导入音频…"); onClicked: mediaDialog.open() }
                                Item { Layout.fillWidth: true }
                            }
                        }

                        ColumnLayout {
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                Layout.topMargin: 8
                                Label {
                                    text: roughCut.scriptPath !== "" ? roughCut.scriptPath : qsTr("可直接输入或粘贴文字")
                                    color: Theme.secondaryText
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                SubButton { text: qsTr("导入…"); onClicked: scriptDialog.open() }
                            }
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                clip: true
                                contentWidth: availableWidth
                                TextArea {
                                    id: roughScriptEditor
                                    objectName: "roughCutScriptEditor"
                                    text: roughCut.scriptText
                                    color: Theme.text
                                    selectionColor: Theme.selection
                                    selectedTextColor: Theme.text
                                    placeholderText: qsTr("输入、粘贴参考文案，或导入 TXT / DOCX")
                                    placeholderTextColor: Theme.placeholder
                                    wrapMode: TextEdit.Wrap
                                    background: Rectangle {
                                        color: Theme.input
                                        border.width: 1
                                        border.color: roughScriptEditor.activeFocus ? Theme.focusBorder : Theme.border
                                        radius: Theme.radiusInput
                                    }
                                    onTextChanged: if (text !== roughCut.scriptText) roughCut.setScriptText(text)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                Layout.bottomMargin: 10
                                SubButton { text: qsTr("粘贴"); onClicked: { roughScriptEditor.forceActiveFocus(); roughScriptEditor.paste() } }
                                SubButton {
                                    text: qsTr("复制全部")
                                    enabled: roughScriptEditor.length > 0
                                    onClicked: {
                                        roughScriptEditor.selectAll()
                                        roughScriptEditor.copy()
                                        roughScriptEditor.deselect()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }

            Rectangle {
                color: Theme.panel; border.color: Theme.border
                SplitView.fillWidth: true; SplitView.minimumWidth: 420
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; height: 34; color: Theme.panelHeader; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: qsTr("源监视器"); color: Theme.text; font.bold: true } }
                    TimelineSceneItem {
                        id: sourceWaveform
                        objectName: "roughCutSourceWaveform"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        viewportInteracting: sourceZoomBar.interacting
                        Component.onCompleted: roughCut.setSourceWaveformItem(sourceWaveform)
                        onUserSeeked: roughCut.seek(playheadUs / 1000)
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 64; color: Theme.panel
                        ColumnLayout {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 0
                            Slider {
                                id: sourceSeek
                                objectName: "roughCutSeek"
                                Layout.fillWidth: true; Layout.preferredHeight: 22
                                enabled: roughCut.mediaPath !== ""; from: 0; to: Math.max(1, roughCut.durationMs); value: roughCut.positionMs
                                onPressedChanged: if (pressed && roughCut.playing) roughCut.togglePlay()
                                onMoved: roughCut.seek(value)
                                background: Rectangle { x: sourceSeek.leftPadding; y: sourceSeek.topPadding + sourceSeek.availableHeight / 2 - height / 2; width: sourceSeek.availableWidth; height: 5; radius: 2.5; color: Theme.background; border.color: Theme.border; Rectangle { width: sourceSeek.visualPosition * parent.width; height: parent.height; radius: parent.radius; color: Theme.border } }
                                handle: Rectangle { x: sourceSeek.leftPadding + sourceSeek.visualPosition * (sourceSeek.availableWidth - width); y: sourceSeek.topPadding + sourceSeek.availableHeight / 2 - height / 2; width: 10; height: 10; radius: 5; color: Theme.panel; border.width: 2; border.color: sourceSeek.pressed ? Theme.text : Theme.muted }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 2
                                Label { text: window.formatTime(roughCut.positionMs); color: Theme.accent; font.family: "Consolas"; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                SubToolButton { objectName: "roughCutPlayPause"; text: roughCut.playing ? qsTr("暂停") : qsTr("播放"); icon.source: roughCut.playing ? "icons/pause.svg" : "icons/play.svg"; enabled: roughCut.mediaPath !== ""; onClicked: roughCut.togglePlay() }
                                SubComboBox { objectName: "roughCutPlaybackRate"; Layout.preferredWidth: 76; enabled: roughCut.mediaPath !== "" && !roughCut.playing; model: ["0.5×", "0.75×", "1.0×", "1.25×", "1.5×", "1.75×", "2.0×", "2.5×", "3.0×"]; currentIndex: 2; onActivated: roughCut.setPlaybackRate(parseFloat(currentText)) }
                                Item { Layout.fillWidth: true }
                                SubToolButton { text: qsTr("缩小源波形"); icon.source: "icons/minus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: window.adjustZoom(sourceWaveform, -10) }
                                Label { text: sourceWaveform.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                                SubToolButton { text: qsTr("放大源波形"); icon.source: "icons/plus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: window.adjustZoom(sourceWaveform, 10) }
                            }
                        }
                    }
                    TimelineZoomBar { id: sourceZoomBar; objectName: "roughCutSourceZoomBar"; Layout.fillWidth: true; timeline: sourceWaveform }
                }
            }

            Rectangle {
                color: Theme.panel; border.color: Theme.border
                SplitView.preferredWidth: Math.max(300, workspaceSplit.width * 0.25)
                SplitView.minimumWidth: 280; SplitView.maximumWidth: 440
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; height: 34; color: Theme.panelHeader; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: qsTr("分析结果"); color: Theme.text; font.bold: true } }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 4
                        Repeater {
                            model: [{key:"ALL", label:qsTr("全部")}, {key:"KEEP", label:qsTr("保留")}, {key:"REVIEW", label:qsTr("复核")}, {key:"CUT", label:qsTr("剪除")}]
                            SubButton { required property var modelData; text: modelData.label; checkable: true; checked: window.filter === modelData.key; Layout.fillWidth: true; onClicked: window.filter = modelData.key }
                        }
                    }
                    ListView {
                        id: resultList; objectName: "roughCutResultList"
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: roughCut.resultModel; ScrollBar.vertical: SubScrollBar { }
                        delegate: Rectangle {
                            id: resultRow
                            required property string status; required property string text; required property string reason
                            required property var evidence; required property int index; required property bool userOverride
                            width: resultList.width; height: visible ? 132 : 0
                            visible: window.filter === "ALL" || window.filter === status
                            color: status === "CUT" ? "#332126" : status === "KEEP" ? "#1D3028" : "#352F22"
                            border.color: Theme.divider
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 8; spacing: 4
                                Label { text: (resultRow.status === "KEEP" ? qsTr("保留") : resultRow.status === "CUT" ? qsTr("剪除") : qsTr("复核")) + "  " + resultRow.text; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label { text: resultRow.reason; color: Theme.secondaryText; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label { text: resultRow.evidence && resultRow.evidence.length ? qsTr("证据：") + resultRow.evidence.join("；") : qsTr("证据：无"); color: Theme.muted; elide: Text.ElideRight; Layout.fillWidth: true }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 4
                                    SubButton { text: qsTr("试听"); onClicked: roughCut.audition(resultRow.index) }
                                    Item { Layout.fillWidth: true }
                                    SubButton { text: qsTr("保留"); onClicked: roughCut.setDecision(resultRow.index, "KEEP") }
                                    SubButton { text: qsTr("复核"); onClicked: roughCut.setDecision(resultRow.index, "REVIEW") }
                                    SubButton { text: qsTr("剪除"); onClicked: roughCut.setDecision(resultRow.index, "CUT") }
                                    SubToolButton { visible: resultRow.userOverride; text: qsTr("恢复自动判断"); icon.source: "icons/step-back.svg"; onClicked: roughCut.restoreAutoDecision(resultRow.index) }
                                }
                            }
                            TapHandler { onDoubleTapped: roughCut.audition(resultRow.index) }
                        }
                        Label { anchors.centerIn: parent; visible: roughCut.resultCount === 0; text: qsTr("分析后在此复核结果"); color: Theme.secondaryText }
                    }
                }
            }
        }

        Rectangle {
            color: Theme.panel; border.color: Theme.border
            SplitView.preferredHeight: verticalSplit.height * 0.35
            SplitView.minimumHeight: 220
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; height: 34; color: Theme.panelHeader
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8
                        Label { text: qsTr("粗剪时间线"); color: Theme.text; font.bold: true }
                        Label { text: qsTr("保留 / 复核"); color: Theme.muted }
                        Item { Layout.fillWidth: true }
                        SubButton { text: qsTr("连续试听"); enabled: roughCut.resultCount > 0; onClicked: roughCut.playTimeline() }
                        SubButton { text: qsTr("停止"); onClicked: roughCut.stopTimeline() }
                        SubToolButton { text: qsTr("缩小粗剪时间线"); icon.source: "icons/minus.svg"; enabled: roughCut.resultCount > 0; onClicked: window.adjustZoom(roughTimeline, -10) }
                        Label { text: roughTimeline.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                        SubToolButton { text: qsTr("放大粗剪时间线"); icon.source: "icons/plus.svg"; enabled: roughCut.resultCount > 0; onClicked: window.adjustZoom(roughTimeline, 10) }
                    }
                }
                TimelineSceneItem { id: roughTimeline; objectName: "roughCutTimeline"; Layout.fillWidth: true; Layout.fillHeight: true; viewportInteracting: roughZoomBar.interacting; Component.onCompleted: roughCut.setTimelineItem(roughTimeline) }
                TimelineZoomBar { id: roughZoomBar; objectName: "roughCutTimelineZoomBar"; Layout.fillWidth: true; timeline: roughTimeline }
            }
        }
    }

    Rectangle {
        id: statusBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: 24; color: Theme.panelRaised
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 1; color: Theme.divider }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
            Label { text: roughCut.statusText; color: Theme.muted; elide: Text.ElideRight; Layout.fillWidth: true }
            Label { text: roughCut.mediaPath; color: Theme.muted; elide: Text.ElideMiddle; Layout.maximumWidth: 320 }
        }
    }

    Shortcut { sequence: "Space"; enabled: visible; onActivated: roughCut.togglePlay() }
    Shortcut { sequence: "Ctrl+Z"; enabled: visible; onActivated: roughCut.undo() }
    Shortcut { sequence: "Ctrl+Y"; enabled: visible; onActivated: roughCut.redo() }
    Shortcut { sequence: "="; enabled: visible; onActivated: window.adjustZoom(roughTimeline, 10) }
    Shortcut { sequence: "-"; enabled: visible; onActivated: window.adjustZoom(roughTimeline, -10) }
}
