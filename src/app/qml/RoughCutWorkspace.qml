import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: window
    objectName: "roughCutWorkspace"
    property bool textEditing: roughScriptEditor.activeFocus || roughAssetSearch.activeFocus
                               || scriptDialog.visible || mediaDialog.visible
                               || (settingsWindow && settingsWindow.visible)
    property int selectedMonitor: 0
    function activateMonitor(index) {
        if (selectedMonitor !== index) {
            roughCut.stopTimeline()
            selectedMonitor = index
        }
        window.forceActiveFocus(Qt.MouseFocusReason)
    }
    Component {
        id: settingsWindowComponent
        SettingsWindow { controller: editor }
    }
    property var settingsWindow: null
    function openSettings() {
        if (!settingsWindow) settingsWindow = settingsWindowComponent.createObject(window)
        settingsWindow.show()
    }

    function formatTime(milliseconds) {
        const value = Math.max(0, Math.round(milliseconds))
        const minutes = Math.floor(value / 60000)
        const seconds = Math.floor(value / 1000) % 60
        const millis = value % 1000
        return String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0")
               + "." + String(millis).padStart(3, "0")
    }
    function requestLeave(action) {
        const win = window.Window.window
        if (win && typeof win.requestDestructiveAction === "function")
            win.requestDestructiveAction(action)
        else
            action()
    }
    function goToReview(delta) {
        const current = resultList.currentItem && resultList.currentItem.recordingIndex !== undefined
            ? resultList.currentItem.recordingIndex : -1
        const next = delta > 0 ? roughCut.nextReviewRow(current) : roughCut.previousReviewRow(current)
        if (next < 0) return
        roughCut.statusFilter = "REVIEW"
        const filterRow = roughCut.filterRowForSource(next)
        if (filterRow < 0) return
        resultList.currentIndex = filterRow
        resultList.positionViewAtIndex(filterRow, ListView.Contain)
        window.activateMonitor(1)
        roughCut.locateResult(next)
    }
    FileDialog { id: openProjectDialog; title: qsTr("打开粗剪工程"); nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: window.requestLeave(function() { roughCut.openProject(selectedFile) }) }
    FileDialog { id: saveProjectDialog; title: qsTr("保存粗剪工程"); fileMode: FileDialog.SaveFile; defaultSuffix: "subcue-roughcut"; nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: roughCut.saveProject(selectedFile) }
    FileDialog { id: mediaDialog; title: qsTr("选择 WAV 音频"); nameFilters: [qsTr("WAV 音频 (*.wav)")]; parentWindow: window.Window.window; onAccepted: window.requestLeave(function() { roughCut.loadMedia(selectedFile) }) }
    FileDialog { id: scriptDialog; title: qsTr("选择参考文案"); nameFilters: [qsTr("参考文案 (*.txt *.docx)")]; parentWindow: window.Window.window; onAccepted: roughCut.loadScript(selectedFile) }
    FileDialog { id: exportDialog; title: qsTr("导出 FCP7 XML"); fileMode: FileDialog.SaveFile; defaultSuffix: "xml"; nameFilters: [qsTr("FCP7 XML (*.xml)")]; parentWindow: window.Window.window; onAccepted: roughCut.exportXml(selectedFile) }
    FileDialog { id: exportWavDialog; title: qsTr("导出精简 WAV"); fileMode: FileDialog.SaveFile; defaultSuffix: "wav"; nameFilters: [qsTr("WAV 音频 (*.wav)")]; parentWindow: window.Window.window; onAccepted: roughCut.exportWav(selectedFile) }

    DropArea {
        id: roughCutFileDropArea
        anchors.fill: parent
        z: 50
        keys: ["text/uri-list"]
        onEntered: function(drag) {
            if (!drag.hasUrls || drag.urls.length === 0) return
            const path = String(drag.urls[0]).toLowerCase()
            drag.accepted = path.endsWith(".wav") || path.endsWith(".txt") || path.endsWith(".docx")
        }
        onDropped: function(drop) {
            if (!drop.urls || drop.urls.length === 0) return
            const url = drop.urls[0]
            const path = String(url).toLowerCase()
            if (path.endsWith(".wav")) {
                window.requestLeave(function() { roughCut.loadMedia(url) })
                drop.acceptProposedAction()
            } else if (path.endsWith(".txt") || path.endsWith(".docx")) {
                roughSourceTabs.currentIndex = 1
                roughCut.loadScript(url)
                drop.acceptProposedAction()
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: parent.containsDrag
            color: "#994C8DFF"
            border.width: 2
            border.color: Theme.accentHover
            Label {
                anchors.centerIn: parent
                text: qsTr("松开以导入 WAV 音频或 TXT / DOCX 文稿")
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
            }
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
            MenuButton { text: qsTr("设置模型…"); enabled: !roughCut.busy; onClicked: window.openSettings() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("打开工程"); enabled: !roughCut.busy; onClicked: openProjectDialog.open() }
            MenuButton { text: qsTr("保存工程"); enabled: roughCut.canSave; onClicked: roughCut.projectPath !== "" ? roughCut.saveCurrentProject() : saveProjectDialog.open() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("撤销"); enabled: roughCut.canUndo; onClicked: roughCut.undo() }
            MenuButton { text: qsTr("重做"); enabled: roughCut.canRedo; onClicked: roughCut.redo() }
            Item { Layout.fillWidth: true }
            MenuButton { text: qsTr("开始分析"); enabled: roughCut.mediaPath !== "" && !roughCut.busy; onClicked: roughCut.startAnalysis() }
            MenuButton {
                objectName: "roughCutAiReviewButton"
                text: qsTr("AI 复核")
                enabled: roughCut.canAiReview
                onClicked: roughCut.startAiReview()
            }
            MenuButton { text: qsTr("辅助识别"); enabled: roughCut.resultCount > 0 && !roughCut.busy; onClicked: roughCut.startAuxiliaryRecognition() }
            MenuButton { text: qsTr("取消"); enabled: roughCut.busy && !roughCut.cancelling; onClicked: roughCut.cancelAnalysis() }
            MenuButton { text: qsTr("导出 WAV"); enabled: roughCut.canExport; onClicked: exportWavDialog.open() }
            PrimaryButton { text: qsTr("导出 XML"); enabled: roughCut.canExport; onClicked: exportDialog.open() }
        }
    }

    SplitView {
        id: verticalSplit
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: analysisProgressDialog.bottom; anchors.bottom: statusBar.top
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
                                text: qsTr("项目：素材")
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
                            spacing: 6
                            SubTextField {
                                id: roughAssetSearch
                                Layout.fillWidth: true
                                Layout.margins: 8
                                placeholderText: qsTr("搜索项目素材")
                                Accessible.name: qsTr("搜索项目素材")
                            }
                            ListView {
                                id: roughAssetList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: roughCut.mediaPath !== "" ? 1 : 0
                                currentIndex: roughCut.mediaPath !== "" ? 0 : -1
                                ScrollBar.vertical: SubScrollBar { }
                                delegate: ItemDelegate {
                                    width: roughAssetList.width
                                    height: window.mediaName(roughCut.mediaPath).toLowerCase().indexOf(roughAssetSearch.text.toLowerCase()) >= 0 ? 56 : 0
                                    visible: height > 0
                                    highlighted: true
                                    background: Rectangle {
                                        color: Theme.selection
                                        Rectangle { width: 3; height: parent.height; color: Theme.accent }
                                    }
                                    contentItem: ColumnLayout {
                                        spacing: 3
                                        Label { text: window.mediaName(roughCut.mediaPath); color: Theme.text; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                        Label { text: qsTr("音频") + "  ·  " + window.formatTime(roughCut.durationMs); color: Theme.secondaryText; font.pixelSize: Theme.fontSizeTiny; Layout.fillWidth: true }
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    width: Math.max(100, parent.width - 32)
                                    visible: roughCut.mediaPath === ""
                                    text: qsTr("双击此处导入 WAV 音频\n或从资源管理器拖入文件")
                                    color: Theme.muted
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    lineHeight: 1.6
                                }
                                TapHandler { onDoubleTapped: if (roughCut.mediaPath === "") mediaDialog.open() }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                SubButton { text: qsTr("导入…"); onClicked: mediaDialog.open() }
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
                                id: roughScriptScrollView
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                clip: true
                                contentWidth: availableWidth
                                ScrollBar.vertical: SubScrollBar { }
                                TextArea {
                                    id: roughScriptEditor
                                    objectName: "roughCutScriptEditor"
                                    Component.onCompleted: text = roughCut.scriptText
                                    color: Theme.text
                                    selectionColor: Theme.selection
                                    selectedTextColor: Theme.text
                                    placeholderText: qsTr("输入、粘贴参考文案，或导入 TXT / DOCX")
                                    placeholderTextColor: Theme.placeholder
                                    wrapMode: TextEdit.Wrap
                                    readOnly: roughCut.busy
                                    background: Rectangle {
                                        color: Theme.input
                                        border.width: 1
                                        border.color: roughScriptEditor.activeFocus ? Theme.focusBorder : Theme.border
                                        radius: Theme.radiusInput
                                    }
                                    onTextChanged: if (text !== roughCut.scriptText) roughCut.setScriptText(text)
                                    Connections {
                                        target: roughCut
                                        function onScriptChanged() {
                                            if (roughScriptEditor.text !== roughCut.scriptText)
                                                roughScriptEditor.text = roughCut.scriptText
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                Layout.bottomMargin: 10
                                SubButton { text: qsTr("粘贴"); onClicked: { roughSourceTabs.currentIndex = 1; roughScriptEditor.forceActiveFocus(); roughScriptEditor.paste() } }
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
                color: Theme.panel; border.color: window.selectedMonitor === 0 ? Theme.accent : Theme.border
                border.width: window.selectedMonitor === 0 ? 2 : 1
                SplitView.fillWidth: true; SplitView.minimumWidth: 420
                TapHandler { onPressedChanged: if (pressed) window.activateMonitor(0) }
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; height: 34; color: Theme.panelHeader; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: qsTr("源监视器"); color: Theme.text; font.bold: true } }
                    TimelineSceneItem {
                        id: sourceWaveform
                        objectName: "roughCutSourceWaveform"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        followDirection: roughCut.playing ? 1 : 0
                        viewportInteracting: sourceZoomBar.interacting
                        Component.onCompleted: roughCut.setSourceWaveformItem(sourceWaveform)
                        onUserSeeked: { window.activateMonitor(0); roughCut.seek(playheadUs / 1000) }
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 64; color: Theme.panel
                        ColumnLayout {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 0
                            Slider {
                                id: sourceSeek
                                objectName: "roughCutSeek"
                                Layout.fillWidth: true; Layout.preferredHeight: 22
                                enabled: roughCut.mediaPath !== ""; from: 0; to: Math.max(1, roughCut.durationMs)
                                Binding on value { value: roughCut.positionMs; when: !sourceSeek.pressed }
                                onPressedChanged: {
                                    if (!pressed) return
                                    window.activateMonitor(0)
                                    if (roughCut.playing) roughCut.togglePlay()
                                }
                                onMoved: { window.activateMonitor(0); roughCut.seek(Math.round(value)) }
                                background: Rectangle { x: sourceSeek.leftPadding; y: sourceSeek.topPadding + sourceSeek.availableHeight / 2 - height / 2; width: sourceSeek.availableWidth; height: 5; radius: 2.5; color: Theme.background; border.color: Theme.border; Rectangle { width: sourceSeek.visualPosition * parent.width; height: parent.height; radius: parent.radius; color: Theme.border } }
                                handle: Rectangle { x: sourceSeek.leftPadding + sourceSeek.visualPosition * (sourceSeek.availableWidth - width); y: sourceSeek.topPadding + sourceSeek.availableHeight / 2 - height / 2; width: 10; height: 10; radius: 5; color: Theme.panel; border.width: 2; border.color: sourceSeek.pressed ? Theme.text : Theme.muted }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 2
                                Label { text: window.formatTime(roughCut.positionMs); color: Theme.accent; font.family: "Consolas"; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                SubToolButton { objectName: "roughCutPlayPause"; text: roughCut.playing ? qsTr("暂停") : qsTr("播放"); icon.source: roughCut.playing ? "icons/pause.svg" : "icons/play.svg"; enabled: roughCut.mediaPath !== ""; onClicked: { window.activateMonitor(0); roughCut.togglePlay() } }
                                SubComboBox { objectName: "roughCutPlaybackRate"; Layout.preferredWidth: 76; enabled: roughCut.mediaPath !== "" && !roughCut.playing; model: ["0.5×", "0.75×", "1.0×", "1.25×", "1.5×", "1.75×", "2.0×", "2.5×", "3.0×"]; currentIndex: 2; onActivated: roughCut.setPlaybackRate(parseFloat(currentText)) }
                                Item { Layout.fillWidth: true }
                                SubToolButton { text: qsTr("缩小源波形"); icon.source: "icons/minus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: sourceWaveform.adjustZoomPercent(-10) }
                                Label { text: sourceWaveform.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                                SubToolButton { text: qsTr("放大源波形"); icon.source: "icons/plus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: sourceWaveform.adjustZoomPercent(10) }
                            }
                        }
                    }
                    TimelineZoomBar { id: sourceZoomBar; objectName: "roughCutSourceZoomBar"; Layout.fillWidth: true; timeline: sourceWaveform; onInteractingChanged: if (interacting) window.activateMonitor(0) }
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
                            model: [
                                {key:"ALL", label:qsTr("全部"), count: roughCut.resultCount},
                                {key:"KEEP", label:qsTr("保留"), count: roughCut.resultModel.keepCount},
                                {key:"REVIEW", label:qsTr("复核"), count: roughCut.resultModel.reviewCount},
                                {key:"CUT", label:qsTr("剪除"), count: roughCut.resultModel.cutCount}
                            ]
                            SubButton {
                                required property var modelData
                                text: modelData.label + " " + modelData.count
                                checkable: true
                                checked: roughCut.statusFilter === modelData.key
                                Layout.fillWidth: true
                                onClicked: roughCut.statusFilter = modelData.key
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.bottomMargin: 4; spacing: 4
                        SubButton {
                            text: qsTr("上一条待复核")
                            enabled: !roughCut.busy && roughCut.resultModel.reviewCount > 0
                            onClicked: window.goToReview(-1)
                        }
                        SubButton {
                            text: qsTr("下一条待复核")
                            enabled: !roughCut.busy && roughCut.resultModel.reviewCount > 0
                            onClicked: window.goToReview(1)
                        }
                    }
                    ListView {
                        id: resultList; objectName: "roughCutResultList"
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: roughCut.resultFilterModel; ScrollBar.vertical: SubScrollBar { }
                        delegate: Rectangle {
                            id: resultRow
                            required property string status; required property string text; required property string reason
                            required property var evidence; required property int index; required property bool userOverride
                            required property int takeGroup; required property bool bestTake; required property real score
                            required property string failureType; required property real modelProbability
                            required property string scriptRange; required property string replacement
                            required property string decisionSource
                            required property int recordingIndex
                            required property string statusLabel
                            required property string shortReason
                            required property string failureLabel
                            required property string evidenceText
                            required property string technicalDetails
                            property bool expanded: false
                            width: resultList.width
                            height: resultColumn.implicitHeight + 16
                            color: ListView.isCurrentItem ? Theme.selection : status === "CUT" ? "#332126" : status === "KEEP" ? "#1D3028" : "#352F22"
                            border.color: ListView.isCurrentItem ? Theme.accent : Theme.divider
                            border.width: ListView.isCurrentItem ? 2 : 1
                            ColumnLayout {
                                id: resultColumn
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.leftMargin: 8; anchors.rightMargin: 8 + Theme.scrollBarSize; anchors.topMargin: 8
                                spacing: 4
                                Label {
                                    text: resultRow.statusLabel
                                          + (resultRow.takeGroup >= 0 ? qsTr("  第 %1 组").arg(resultRow.takeGroup + 1) : "")
                                          + (resultRow.bestTake ? qsTr("  最佳 Take") : "")
                                    color: Theme.text
                                    font.bold: true
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: resultRow.expanded ? resultRow.text : resultRow.text
                                    color: Theme.text
                                    wrapMode: resultRow.expanded ? Text.Wrap : Text.NoWrap
                                    elide: resultRow.expanded ? Text.ElideNone : Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: (resultRow.failureLabel !== "" ? resultRow.failureLabel + " · " : "")
                                          + (resultRow.expanded ? resultRow.reason : resultRow.shortReason)
                                          + " · " + resultRow.scriptRange
                                          + (resultRow.replacement !== "" ? " · " + resultRow.replacement : "")
                                    color: Theme.secondaryText
                                    wrapMode: resultRow.expanded ? Text.Wrap : Text.NoWrap
                                    elide: resultRow.expanded ? Text.ElideNone : Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    visible: resultRow.expanded
                                    text: qsTr("证据：") + resultRow.evidenceText
                                    color: Theme.muted
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }
                                Label {
                                    visible: resultRow.expanded && resultRow.technicalDetails !== ""
                                    text: resultRow.technicalDetails
                                    color: Theme.muted
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 4
                                    SubButton { text: qsTr("试听"); enabled: !roughCut.busy; onClicked: roughCut.audition(resultRow.recordingIndex) }
                                    SubButton { text: resultRow.expanded ? qsTr("收起") : qsTr("详情"); onClicked: resultRow.expanded = !resultRow.expanded }
                                    Item { Layout.fillWidth: true }
                                    SubButton { text: qsTr("保留"); enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "KEEP") }
                                    SubButton { text: qsTr("复核"); enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "REVIEW") }
                                    SubButton { text: qsTr("剪除"); enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "CUT") }
                                    SubToolButton { visible: resultRow.userOverride; enabled: !roughCut.busy; text: qsTr("恢复自动判断"); icon.source: "icons/step-back.svg"; onClicked: roughCut.restoreAutoDecision(resultRow.recordingIndex) }
                                }
                            }
                            TapHandler {
                                onTapped: {
                                    resultList.currentIndex = resultRow.index
                                    window.activateMonitor(1)
                                    roughCut.locateResult(resultRow.recordingIndex)
                                }
                                onDoubleTapped: roughCut.audition(resultRow.recordingIndex)
                            }
                        }
                        Label { anchors.centerIn: parent; visible: resultList.count === 0; text: qsTr("分析后在此复核结果"); color: Theme.secondaryText }
                    }
                }
            }
        }

        Rectangle {
            color: Theme.panel; border.color: window.selectedMonitor === 1 ? Theme.accent : Theme.border
            border.width: window.selectedMonitor === 1 ? 2 : 1
            TapHandler { onPressedChanged: if (pressed) window.activateMonitor(1) }
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
                        Button {
                            id: sequencePlay
                            text: roughCut.timelineActive && !roughCut.timelinePaused ? qsTr("暂停") : qsTr("播放")
                            icon.source: roughCut.timelineActive && !roughCut.timelinePaused ? "icons/pause.svg" : "icons/play.svg"
                            enabled: roughCut.canExport; implicitHeight: 30
                            onClicked: { window.activateMonitor(1); roughCut.toggleTimelinePlay() }
                            contentItem: RowLayout {
                                spacing: 5
                                Image { source: sequencePlay.icon.source; sourceSize: Qt.size(16, 16); opacity: sequencePlay.enabled ? 1 : 0.35 }
                                Label { text: sequencePlay.text; color: sequencePlay.enabled ? Theme.text : Theme.disabledText }
                            }
                            background: Rectangle { radius: Theme.radiusButton; color: sequencePlay.down ? Theme.buttonPressed : sequencePlay.hovered ? Theme.buttonHover : Theme.button; border.color: Theme.buttonBorder }
                        }
                        Button {
                            id: sequenceStop
                            text: qsTr("停止"); icon.source: "icons/stop.svg"
                            enabled: roughCut.timelineActive; implicitHeight: 30
                            onClicked: roughCut.stopTimeline()
                            contentItem: RowLayout {
                                spacing: 5
                                Image { source: sequenceStop.icon.source; sourceSize: Qt.size(16, 16); opacity: sequenceStop.enabled ? 1 : 0.35 }
                                Label { text: sequenceStop.text; color: sequenceStop.enabled ? Theme.text : Theme.disabledText }
                            }
                            background: Rectangle { radius: Theme.radiusButton; color: sequenceStop.down ? Theme.buttonPressed : sequenceStop.hovered ? Theme.buttonHover : Theme.button; border.color: Theme.buttonBorder }
                        }
                        SubCheckBox { text: qsTr("连续试听"); checked: roughCut.continuousAudition; onClicked: roughCut.setContinuousAudition(checked) }
                        SubToolButton { text: qsTr("缩小粗剪时间线"); icon.source: "icons/minus.svg"; enabled: roughCut.resultCount > 0; onClicked: roughTimeline.adjustZoomPercent(-10) }
                        Label { text: roughTimeline.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                        SubToolButton { text: qsTr("放大粗剪时间线"); icon.source: "icons/plus.svg"; enabled: roughCut.resultCount > 0; onClicked: roughTimeline.adjustZoomPercent(10) }
                    }
                }
                TimelineSceneItem {
                    id: roughTimeline
                    objectName: "roughCutTimeline"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    followDirection: roughCut.timelineActive && !roughCut.timelinePaused ? 1 : 0
                    viewportInteracting: roughZoomBar.interacting
                    Component.onCompleted: roughCut.setTimelineItem(roughTimeline)
                    onUserSeeked: { window.activateMonitor(1); roughCut.seekTimeline(playheadUs / 1000) }
                    onSelectedCueIdChanged: {
                        const row = Number(selectedCueId)
                        if (selectedCueId === "" || !Number.isInteger(row) || row < 0) return
                        roughCut.statusFilter = "ALL"
                        resultList.currentIndex = roughCut.filterRowForSource(row)
                        resultList.positionViewAtIndex(resultList.currentIndex, ListView.Contain)
                    }
                }
                TimelineZoomBar { id: roughZoomBar; objectName: "roughCutTimelineZoomBar"; Layout.fillWidth: true; timeline: roughTimeline; onInteractingChanged: if (interacting) window.activateMonitor(1) }
            }
        }
    }

    Rectangle {
        id: analysisProgressDialog
        objectName: "roughCutAnalysisProgressDialog"
        property double startedAt: 0
        property int elapsedSeconds: 0
        onVisibleChanged: if (visible) { startedAt = Date.now(); elapsedSeconds = 0 }
        Timer {
            interval: 1000
            repeat: true
            running: roughCut.busy
            onTriggered: parent.elapsedSeconds = Math.floor((Date.now() - parent.startedAt) / 1000)
        }
        anchors.top: commandBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: roughCut.busy ? 66 : 0
        visible: roughCut.busy
        color: Theme.panelRaised
        z: 20
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 16
            Label { text: (roughCut.busyTaskTitle || qsTr("正在处理")) + " · " + parent.parent.elapsedSeconds + "s"; color: Theme.text }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: roughCut.statusText; color: Theme.secondaryText }
                ProgressBar {
                    id: roughCutAnalysisProgress
                    objectName: "roughCutAnalysisProgress"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 16
                    from: 0
                    to: 100
                    indeterminate: roughCut.progressIndeterminate
                    value: roughCut.progressPercent
                    background: Rectangle {
                        color: Theme.scrollTrack
                        border.color: Theme.border
                    }
                    contentItem: Item {
                        clip: true
                        Rectangle {
                            objectName: "roughCutAnalysisProgressFill"
                            visible: !roughCutAnalysisProgress.indeterminate
                            width: roughCutAnalysisProgress.visualPosition * parent.width
                            height: parent.height
                            color: Theme.accent
                        }
                        Rectangle {
                            id: roughCutProgressIndeterminate
                            width: Math.max(28, parent.width * 0.24)
                            height: parent.height
                            visible: roughCutAnalysisProgress.indeterminate
                            color: Theme.accent
                            SequentialAnimation on x {
                                running: roughCutAnalysisProgress.indeterminate && analysisProgressDialog.visible
                                loops: Animation.Infinite
                                NumberAnimation {
                                    from: -roughCutProgressIndeterminate.width
                                    to: roughCutProgressIndeterminate.parent.width
                                    duration: 900
                                    easing.type: Easing.InOutQuad
                                }
                            }
                        }
                    }
                }
            }
            Label { text: roughCut.progressIndeterminate ? qsTr("处理中…") : (roughCut.progressPercent + "%"); color: Theme.text }
            SubButton {
                text: roughCut.cancelling ? qsTr("取消中…") : qsTr("取消")
                enabled: !roughCut.cancelling
                onClicked: roughCut.cancelAnalysis()
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

    Shortcut { sequence: "Ctrl+O"; enabled: visible && !window.textEditing; onActivated: mediaDialog.open() }
    Shortcut { sequence: "Ctrl+I"; enabled: visible && !window.textEditing; onActivated: mediaDialog.open() }
    Shortcut { sequence: "Space"; enabled: visible && !window.textEditing; onActivated: window.selectedMonitor === 1 ? roughCut.toggleTimelinePlay() : roughCut.togglePlay() }
    Shortcut { sequence: "Ctrl+Z"; enabled: visible && !window.textEditing; onActivated: roughCut.undo() }
    Shortcut { sequence: "Ctrl+Y"; enabled: visible && !window.textEditing; onActivated: roughCut.redo() }
    Shortcut { sequence: "K"; enabled: visible && !window.textEditing; onActivated: { if (window.selectedMonitor === 1) roughCut.stopTimeline(); else if (roughCut.playing) roughCut.togglePlay() } }
    Shortcut { sequence: "Left"; enabled: visible && !window.textEditing; onActivated: window.selectedMonitor === 1 ? roughCut.seekTimeline(roughTimeline.playheadUs / 1000 - 40) : roughCut.seek(roughCut.positionMs - 40) }
    Shortcut { sequence: "Right"; enabled: visible && !window.textEditing; onActivated: window.selectedMonitor === 1 ? roughCut.seekTimeline(roughTimeline.playheadUs / 1000 + 40) : roughCut.seek(roughCut.positionMs + 40) }
    Shortcut { sequence: "Shift+Left"; enabled: visible && !window.textEditing; onActivated: window.selectedMonitor === 1 ? roughCut.seekTimeline(roughTimeline.playheadUs / 1000 - 200) : roughCut.seek(roughCut.positionMs - 200) }
    Shortcut { sequence: "Shift+Right"; enabled: visible && !window.textEditing; onActivated: window.selectedMonitor === 1 ? roughCut.seekTimeline(roughTimeline.playheadUs / 1000 + 200) : roughCut.seek(roughCut.positionMs + 200) }
    Shortcut { sequence: "="; enabled: visible && !window.textEditing; onActivated: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).adjustZoomPercent(10) }
    Shortcut { sequence: "-"; enabled: visible && !window.textEditing; onActivated: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).adjustZoomPercent(-10) }
    Shortcut { sequence: "\\"; enabled: visible && !window.textEditing; onActivated: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).fit() }
}
