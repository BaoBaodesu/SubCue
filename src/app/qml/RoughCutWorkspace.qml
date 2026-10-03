import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: window
    objectName: "roughCutWorkspace"
    property bool auxiliaryCollapsed: false
    readonly property var commandActions: [roughCommand0, roughCommand1, roughCommand2, roughCommand3, roughCommand4, roughCommand5, roughCommand6, roughCommand7, roughCommand8, roughCommand9, roughCommand10, roughCommand11, roughCommand12, roughCommand13, roughCommand14, roughCommand15, roughCommand16, roughCommand17, roughCommand18, roughCommand19, roughCommand20, roughCommand21]
    readonly property bool textEditing: window.Window.window && window.Window.window.activeFocusItem
        && typeof window.Window.window.activeFocusItem.inputMethodComposing !== "undefined"
    function openMediaDialog() { mediaDialog.open() }
    function captureSession() {
        roughCut.setSessionState(Object.assign({}, roughCut.sessionState, {
            auxiliaryCollapsed: auxiliaryCollapsed, selectedRecording: currentRecordingIndex(), selectedMonitor: selectedMonitor,
            auxiliaryRatio: roughAuxiliaryPanel.width / Math.max(1, workspaceSplit.width), resultRatio: roughResultPanel.width / Math.max(1, workspaceSplit.width),
            topRatio: workspaceSplit.height / Math.max(1, verticalSplit.height), resultFilter: roughCut.statusFilter,
            sourceTab: roughSourceTabs.currentIndex
        }))
    }
    function restoreLayout() {
        workspaceSplit.SplitView.preferredHeight = verticalSplit.height * Math.max(0.4, Math.min(0.75, roughCut.sessionState.topRatio || 0.6))
        roughAuxiliaryPanel.SplitView.preferredWidth = Math.max(240, workspaceSplit.width * Math.max(0.15, Math.min(0.3, roughCut.sessionState.auxiliaryRatio || 0.2)))
        roughResultPanel.SplitView.preferredWidth = Math.max(300, workspaceSplit.width * Math.max(0.25, Math.min(0.4, roughCut.sessionState.resultRatio || 0.3)))
    }
    function resetLayout() {
        auxiliaryCollapsed = false
        workspaceSplit.SplitView.preferredHeight = verticalSplit.height * 0.6
        roughAuxiliaryPanel.SplitView.preferredWidth = Math.max(240, workspaceSplit.width * 0.2)
        roughResultPanel.SplitView.preferredWidth = Math.max(300, workspaceSplit.width * 0.3)
    }
    function belongsTo(item, area) {
        while (item) { if (item === area) return true; item = item.parent }
        return false
    }
    property bool externalModalEditing: false
    readonly property bool modalEditing: externalModalEditing || scriptDialog.visible || mediaDialog.visible || exportDialog.visible || exportWavDialog.visible || rateDialog.visible || reanalysisDialog.visible || openProjectDialog.visible || saveProjectDialog.visible || relinkDialog.visible || moreMenu.visible || (settingsWindow && settingsWindow.visible)
    Connections { target: roughCut; function onProjectChanged() {
        window.restoreLayout()
        window.auxiliaryCollapsed = roughCut.sessionState.auxiliaryCollapsed || false
        resultList.currentIndex = roughCut.filterRowForSource(roughCut.sessionState.selectedRecording === undefined ? -1 : roughCut.sessionState.selectedRecording)
        selectedMonitor = roughCut.sessionState.selectedMonitor || 0
        roughCut.statusFilter = roughCut.sessionState.resultFilter || "ALL"
        roughSourceTabs.currentIndex = roughCut.sessionState.sourceTab || 0
    } }
    function commitPendingCue() { return true }
    function openScriptDialog() { scriptDialog.open() }
    function requestExport() { rateDialog.open() }
    function cycleFocus(direction) {
        const areas = [roughScriptEditor, sourceWaveform, resultList, roughTimeline].filter(function(item) {
            let current = item
            while (current) { if (!current.visible || !current.enabled) return false; current = current.parent }
            return item.width > 0 && item.height > 0
        })
        const current = areas.findIndex(function(item) { return window.belongsTo(window.Window.window.activeFocusItem, item) })
        if (!areas.length) return
        const next = areas[(current + direction + areas.length) % areas.length]
        if (next === sourceWaveform) window.activateMonitor(0)
        else if (next === roughTimeline || next === resultList) window.activateMonitor(1)
        next.forceActiveFocus(Qt.TabFocusReason)
    }
    function adjacentResult(delta) {
        if (!resultList.count) return
        resultList.currentIndex = Math.max(0, Math.min(resultList.count - 1, resultList.currentIndex + delta))
        resultList.positionViewAtIndex(resultList.currentIndex, ListView.Contain)
        roughCut.locateResult(currentRecordingIndex())
    }
    function requestAnalysis() {
        if (roughCut.resultCount > 0) reanalysisDialog.open()
        else roughCut.startAnalysis()
    }
    Dialog { id: reanalysisDialog; anchors.centerIn: parent; modal: true; title: qsTr("重新分析"); standardButtons: Dialog.Ok | Dialog.Cancel; Label { text: qsTr("分析成功后替换现有结果；失败或取消保留当前编辑。"); color: Theme.text } onAccepted: roughCut.startAnalysis() }
    Dialog {
        id: rateDialog; anchors.centerIn: parent; modal: true; title: qsTr("Premiere XML 导出"); standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label { text: roughCut.xmlExportReason || qsTr("音视频同步剪辑，引用原素材"); color: roughCut.xmlExportReason ? Theme.warning : Theme.text; wrapMode: Text.Wrap; Layout.maximumWidth: 420 }
            SubComboBox { id: exportRate; model: ["24/1", "25/1", "30/1", "50/1", "60/1", "24000/1001", "30000/1001", "60000/1001"]; currentIndex: model.indexOf(roughCut.sequenceFrameRate); onActivated: roughCut.setSequenceFrameRate(currentText) }
        }
        onAccepted: if (roughCut.xmlExportReason === "" && roughCut.canExport) exportDialog.open()
    }
    FileDialog { id: relinkDialog; title: qsTr("重新定位原素材"); parentWindow: window.Window.window; onAccepted: roughCut.relinkMedia(selectedFile) }
    property int selectedMonitor: 0
    function frameStep(count) {
        const rate = window.selectedMonitor === 1 ? roughCut.sequenceFrameRate.split("/") : null
        const fps = rate ? Number(rate[0]) / Number(rate[1]) : roughCut.sourceFrameRate
        const step = roughCut.hasVideo && fps > 0 ? 1000 / fps : 40
        if (window.selectedMonitor === 1) roughCut.seekTimeline(roughTimeline.playheadUs / 1000 + count * step)
        else roughCut.seek(roughCut.positionMs + count * step)
    }
    function activateMonitor(index) {
        if (selectedMonitor !== index) {
            roughCut.stopTimeline()
            selectedMonitor = index
        }
        (index === 0 ? sourceWaveform : roughTimeline).forceActiveFocus(Qt.MouseFocusReason)
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
        const hours = Math.floor(value / 3600000)
        const minutes = Math.floor(value / 60000) % 60
        const seconds = Math.floor(value / 1000) % 60
        const millis = value % 1000
        return String(hours).padStart(2, "0") + ":" + String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0")
               + "." + String(millis).padStart(3, "0")
    }
    function mediaName(path) {
        return String(path).replace(/\\/g, "/").split("/").pop()
    }
    function importAudio(url) {
        // 还没有音频时文案应保留。此时工程存不了，未保存提示会直接取消这次导入。
        if (roughCut.mediaPath === "")
            roughCut.loadMedia(url)
        else
            window.requestLeave(function() { roughCut.loadMedia(url) })
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
    function currentRecordingIndex() {
        if (resultList.count <= 0) return -1
        if (resultList.currentItem && resultList.currentItem.recordingIndex !== undefined)
            return resultList.currentItem.recordingIndex
        return -1
    }
    function decideCurrent(decision) {
        const row = window.currentRecordingIndex()
        if (row < 0 || roughCut.busy) return
        roughCut.setDecision(row, decision)
    }
    function auditionCurrent() {
        const row = window.currentRecordingIndex()
        if (row < 0 || roughCut.busy) return
        roughCut.audition(row)
    }
    function restoreCurrent() {
        const row = window.currentRecordingIndex()
        if (row < 0 || roughCut.busy) return
        roughCut.restoreAutoDecision(row)
    }
    FileDialog { id: openProjectDialog; title: qsTr("打开粗剪工程"); nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: { const url = selectedFile; window.requestLeave(function() { roughCut.openProject(url) }) } }
    FileDialog { id: saveProjectDialog; title: qsTr("保存粗剪工程"); fileMode: FileDialog.SaveFile; defaultSuffix: "subcue-roughcut"; nameFilters: [qsTr("SubCue 粗剪工程 (*.subcue-roughcut)")]; parentWindow: window.Window.window; onAccepted: roughCut.saveProject(selectedFile) }
    FileDialog { id: mediaDialog; title: qsTr("选择音视频素材"); nameFilters: [qsTr("音视频素材 (*.wav *.mp4 *.mov *.mkv)")]; parentWindow: window.Window.window; onAccepted: window.importAudio(selectedFile) }
    FileDialog { id: scriptDialog; title: qsTr("选择参考文案"); nameFilters: [qsTr("参考文案 (*.txt *.docx)")]; parentWindow: window.Window.window; onAccepted: { const url = selectedFile; window.requestLeave(function() { roughCut.loadScript(url) }) } }
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
            drag.accepted = /\.(wav|mp4|mov|mkv)$/.test(path) || path.endsWith(".txt") || path.endsWith(".docx")
        }
        onDropped: function(drop) {
            if (!drop.urls || drop.urls.length === 0) return
            const url = drop.urls[0]
            const path = String(url).toLowerCase()
            if (/\.(wav|mp4|mov|mkv)$/.test(path)) {
                window.importAudio(url)
                drop.acceptProposedAction()
            } else if (path.endsWith(".txt") || path.endsWith(".docx")) {
                drop.acceptProposedAction()
                window.requestLeave(function() { roughSourceTabs.currentIndex = 1; roughCut.loadScript(url) })
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
                text: qsTr("松开以导入音视频素材或 TXT / DOCX 文稿")
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
            MenuButton { action: window.Window.window.importAction }
            MenuButton { action: window.Window.window.scriptAction }
            MenuButton { text: qsTr("设置模型…"); visible: window.width >= 1280; enabled: !roughCut.busy; onClicked: window.openSettings() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { action: window.Window.window.openAction }
            MenuButton { action: window.Window.window.saveAction }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { action: roughCommand5; visible: window.width >= 1280; enabled: roughCut.canUndo && !roughCut.busy }
            MenuButton { action: roughCommand6; visible: window.width >= 1280; enabled: roughCut.canRedo && !roughCut.busy }
            MenuButton {
                id: moreButton; text: qsTr("更多"); onClicked: moreMenu.popup(moreButton, 0, moreButton.height)
                SubMenu {
                    id: moreMenu
                    MenuItem { text: qsTr("重置布局"); onTriggered: window.resetLayout() }
                    MenuItem { action: window.Window.window.newAction; text: action.text + "    " + action.keySequence }
                    MenuItem { action: window.Window.window.saveAsAction; text: action.text + "    " + action.keySequence }
                    MenuItem { text: qsTr("重新定位素材"); enabled: !roughCut.busy && roughCut.mediaPath !== ""; onTriggered: relinkDialog.open() }
                    MenuItem { text: window.auxiliaryCollapsed ? qsTr("展开辅助栏") : qsTr("折叠辅助栏"); onTriggered: { window.auxiliaryCollapsed = !window.auxiliaryCollapsed; roughCut.setSessionState(Object.assign({}, roughCut.sessionState, { auxiliaryCollapsed: window.auxiliaryCollapsed })) } }
                    MenuItem { text: qsTr("设置模型"); onTriggered: window.openSettings() }
                    MenuItem { text: qsTr("撤销"); enabled: roughCut.canUndo; onTriggered: roughCut.undo() }
                    MenuItem { text: qsTr("重做"); enabled: roughCut.canRedo; onTriggered: roughCut.redo() }
                    MenuItem { text: qsTr("AI 复核"); enabled: roughCut.canAiReview; onTriggered: roughCut.startAiReview() }
                    MenuItem { text: qsTr("辅助识别"); enabled: roughCut.resultCount > 0 && !roughCut.busy; onTriggered: roughCut.startAuxiliaryRecognition() }
                    MenuItem { text: qsTr("导出 WAV"); enabled: roughCut.canExport; onTriggered: exportWavDialog.open() }
                }
            }
            Item { Layout.fillWidth: true }
            MenuButton { text: qsTr("开始分析"); enabled: roughCut.mediaAvailable && !roughCut.busy; onClicked: window.requestAnalysis() }
            MenuButton {
                objectName: "roughCutAiReviewButton"
                text: qsTr("AI 复核")
                visible: window.width >= 1280
                enabled: roughCut.canAiReview
                onClicked: roughCut.startAiReview()
            }
            MenuButton { text: qsTr("辅助识别"); visible: window.width >= 1280; enabled: roughCut.resultCount > 0 && !roughCut.busy; onClicked: roughCut.startAuxiliaryRecognition() }
            MenuButton { text: qsTr("取消"); enabled: roughCut.busy && !roughCut.cancelling; onClicked: roughCut.cancelAnalysis() }
            MenuButton { text: qsTr("导出 WAV"); visible: window.width >= 1280; enabled: roughCut.canExport; onClicked: exportWavDialog.open() }
            PrimaryButton { text: qsTr("导出 XML"); enabled: roughCut.canExport; onClicked: window.requestExport() }
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
            SplitView.preferredHeight: verticalSplit.height * Math.max(0.4, Math.min(0.75, roughCut.sessionState.topRatio || 0.6))
            SplitView.minimumHeight: Math.min(300, verticalSplit.height * 0.55)
            handle: Rectangle { implicitWidth: 5; implicitHeight: 5; color: Theme.divider }

            Rectangle {
                visible: !window.auxiliaryCollapsed
                color: Theme.panel; border.color: Theme.border
                id: roughAuxiliaryPanel
                SplitView.preferredWidth: Math.max(240, workspaceSplit.width * Math.max(0.15, Math.min(0.3, roughCut.sessionState.auxiliaryRatio || 0.2)))
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
                                        Label { text: (roughCut.hasVideo ? qsTr("视频") : qsTr("音频")) + "  ·  " + window.formatTime(roughCut.durationMs); color: Theme.secondaryText; font.pixelSize: Theme.fontSizeTiny; Layout.fillWidth: true }
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    width: Math.max(100, parent.width - 32)
                                    visible: roughCut.mediaPath === ""
                                    text: qsTr("双击此处导入音视频素材\n或从资源管理器拖入文件")
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
                            Flickable {
                                id: roughScriptFlick
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                flickableDirection: Flickable.VerticalFlick
                                contentWidth: width
                                contentHeight: roughScriptEditor.implicitHeight
                                ScrollBar.vertical: SubScrollBar { }
                                ScriptTextView {
                                    id: roughScriptEditor
                                    objectName: "roughCutScriptEditor"
                                    width: roughScriptFlick.width
                                    height: implicitHeight
                                    scrollView: roughScriptFlick
                                    Component.onCompleted: text = roughCut.scriptText
                                    color: Theme.text
                                    selectionColor: Theme.selection
                                    selectedTextColor: Theme.text
                                    placeholderText: qsTr("输入、粘贴参考文案，或导入 TXT / DOCX")
                                    placeholderTextColor: Theme.placeholder
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
                SplitView.fillWidth: true; SplitView.minimumWidth: Math.min(420, window.width * 0.4)
                TapHandler { onPressedChanged: if (pressed) window.activateMonitor(0) }
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; height: 34; color: Theme.panelHeader; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: qsTr("源监视器") + (window.selectedMonitor === 0 ? qsTr(" · 当前播放目标") : ""); color: Theme.text; font.bold: true } }
                    VideoPreviewItem {
                        id: roughPreview; objectName: "roughCutVideoPreview"; visible: roughCut.hasVideo && roughCut.mediaAvailable
                        Layout.fillWidth: true; Layout.preferredHeight: visible ? Math.min(180, window.height / 4) : 0
                        Component.onCompleted: roughCut.setPreviewItem(roughPreview)
                    }
                    Label {
                        Layout.fillWidth: true; visible: !roughCut.mediaAvailable
                        text: roughCut.mediaPath !== "" ? qsTr("素材离线 · 编辑结果已保留，请重新定位素材") : qsTr("尚未关联媒体")
                        color: Theme.secondaryText; wrapMode: Text.Wrap; padding: 8
                    }
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
                                SubToolButton { objectName: "roughCutPlayPause"; text: roughCut.playing ? qsTr("暂停") : qsTr("播放"); shortcutHint: "Space"; icon.source: roughCut.playing ? "icons/pause.svg" : "icons/play.svg"; enabled: roughCut.mediaPath !== ""; onClicked: { window.activateMonitor(0); roughCut.togglePlay() } }
                                SubComboBox { objectName: "roughCutPlaybackRate"; Layout.preferredWidth: 76; enabled: roughCut.mediaPath !== "" && !roughCut.playing; model: ["0.5×", "0.75×", "1.0×", "1.25×", "1.5×", "1.75×", "2.0×", "2.5×", "3.0×"]; currentIndex: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0].indexOf(roughCut.playbackRate); onActivated: roughCut.setPlaybackRate(parseFloat(currentText)) }
                                Item { Layout.fillWidth: true }
                                SubToolButton { text: qsTr("缩小源波形"); shortcutHint: "-"; icon.source: "icons/minus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: sourceWaveform.adjustZoomPercent(-10) }
                                Label { text: sourceWaveform.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                                SubToolButton { text: qsTr("放大源波形"); shortcutHint: "="; icon.source: "icons/plus.svg"; enabled: roughCut.mediaPath !== ""; onClicked: sourceWaveform.adjustZoomPercent(10) }
                            }
                        }
                    }
                    TimelineZoomBar { id: sourceZoomBar; objectName: "roughCutSourceZoomBar"; Layout.fillWidth: true; timeline: sourceWaveform; onInteractingChanged: if (interacting) window.activateMonitor(0) }
                }
            }

            Rectangle {
                color: Theme.panel; border.color: Theme.border
                id: roughResultPanel
                SplitView.preferredWidth: Math.max(300, workspaceSplit.width * Math.max(0.25, Math.min(0.4, roughCut.sessionState.resultRatio || 0.3)))
                SplitView.minimumWidth: 280; SplitView.maximumWidth: 440
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; height: 34; color: Theme.panelHeader; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: qsTr("分析结果"); color: Theme.text; font.bold: true } }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 8
                        Label { text: qsTr("筛选"); color: Theme.secondaryText }
                        SubComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("全部 %1").arg(roughCut.resultCount), qsTr("保留 %1").arg(roughCut.resultModel.keepCount), qsTr("待复核 %1").arg(roughCut.resultModel.reviewCount), qsTr("剪除 %1").arg(roughCut.resultModel.cutCount)]
                            currentIndex: ["ALL", "KEEP", "REVIEW", "CUT"].indexOf(roughCut.statusFilter)
                            onActivated: roughCut.statusFilter = ["ALL", "KEEP", "REVIEW", "CUT"][currentIndex]
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.bottomMargin: 4; spacing: 4
                        SubButton {
                            text: qsTr("上一条待复核")
                            shortcutHint: "Up"
                            enabled: !roughCut.busy && roughCut.resultModel.reviewCount > 0
                            onClicked: window.goToReview(-1)
                        }
                        SubButton {
                            text: qsTr("下一条待复核")
                            shortcutHint: "Down"
                            enabled: !roughCut.busy && roughCut.resultModel.reviewCount > 0
                            onClicked: window.goToReview(1)
                        }
                    }
                    ListView {
                        id: resultList; objectName: "roughCutResultList"
                        onCurrentIndexChanged: roughCut.setSessionState(Object.assign({}, roughCut.sessionState, { selectedRecording: window.currentRecordingIndex(), selectedMonitor: window.selectedMonitor }))
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
                            required property real startMs; required property real endMs
                            required property int recordingIndex
                            required property string statusLabel
                            required property string shortReason
                            required property string failureLabel
                            required property string evidenceText
                            required property string technicalDetails
                            property bool expanded: false
                            width: resultList.width
                            height: Math.max(74, resultColumn.implicitHeight + 20)
                            color: ListView.isCurrentItem ? Theme.selection : index % 2 ? Theme.panelSecondary : Theme.panel
                            border.color: ListView.isCurrentItem ? Theme.accent : Theme.divider
                            border.width: ListView.isCurrentItem ? 2 : 1
                            Rectangle { width: 3; height: parent.height; color: resultRow.status === "CUT" ? Theme.error : resultRow.status === "KEEP" ? Theme.success : Theme.warning }
                            Label { x: 8; y: 12; width: 24; text: resultRow.recordingIndex + 1; color: Theme.muted; font.pixelSize: 12 }
                            Column {
                                x: 36; y: 12; spacing: 5
                                Label { text: window.formatTime(resultRow.startMs); font.family: "Consolas"; font.pixelSize: 12; color: Theme.secondaryText }
                                Label { text: window.formatTime(resultRow.endMs); font.family: "Consolas"; font.pixelSize: 12; color: Theme.secondaryText }
                            }
                            ColumnLayout {
                                id: resultColumn
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.leftMargin: 144; anchors.rightMargin: 8 + Theme.scrollBarSize; anchors.topMargin: 10
                                spacing: 4
                                Label {
                                    text: resultRow.text; color: Theme.text
                                    wrapMode: Text.Wrap; maximumLineCount: resultRow.expanded ? 1000 : 2
                                    elide: resultRow.expanded ? Text.ElideNone : Text.ElideRight; Layout.fillWidth: true
                                }
                                Label {
                                    text: resultRow.statusLabel + (resultRow.takeGroup >= 0 ? qsTr(" · 第 %1 组").arg(resultRow.takeGroup + 1) : "") + (resultRow.bestTake ? qsTr(" · 最佳 Take") : "")
                                    color: Theme.secondaryText; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight
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
                                Flow {
                                    visible: resultRow.ListView.isCurrentItem
                                    Layout.fillWidth: true; spacing: 4
                                    SubButton { text: qsTr("试听"); shortcutHint: "Return"; enabled: !roughCut.busy; onClicked: roughCut.audition(resultRow.recordingIndex) }
                                    SubButton { text: resultRow.expanded ? qsTr("收起") : qsTr("详情"); onClicked: resultRow.expanded = !resultRow.expanded }
                                    SubButton { text: qsTr("保留"); shortcutHint: "1"; enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "KEEP") }
                                    SubButton { text: qsTr("复核"); shortcutHint: "2"; enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "REVIEW") }
                                    SubButton { text: qsTr("剪除"); shortcutHint: "3"; enabled: !roughCut.busy; onClicked: roughCut.setDecision(resultRow.recordingIndex, "CUT") }
                                    SubToolButton { visible: resultRow.userOverride; enabled: !roughCut.busy; text: qsTr("恢复自动判断"); shortcutHint: "0"; icon.source: "icons/step-back.svg"; onClicked: roughCut.restoreAutoDecision(resultRow.recordingIndex) }
                                }
                            }
                            TapHandler {
                                onTapped: {
                                    resultList.currentIndex = resultRow.index
                                    window.activateMonitor(1)
                                    roughCut.locateResult(resultRow.recordingIndex)
                                    resultList.forceActiveFocus(Qt.MouseFocusReason)
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
            SplitView.minimumHeight: Math.min(220, verticalSplit.height * 0.4)
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; height: 34; color: Theme.panelHeader
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8
                        Label { text: qsTr("粗剪时间线") + (window.selectedMonitor === 1 ? qsTr(" · 当前播放目标") : ""); color: Theme.text; font.bold: true }
                        Label { text: qsTr("保留 / 复核"); color: Theme.muted }
                        Item { Layout.fillWidth: true }
                        Button {
                            id: sequencePlay
                            objectName: "roughCutSequencePlay"
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
                        SubToolButton { text: qsTr("缩小粗剪时间线"); shortcutHint: "-"; icon.source: "icons/minus.svg"; enabled: roughCut.resultCount > 0; onClicked: roughTimeline.adjustZoomPercent(-10) }
                        Label { text: roughTimeline.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 40; horizontalAlignment: Text.AlignHCenter }
                        SubToolButton { text: qsTr("放大粗剪时间线"); shortcutHint: "="; icon.source: "icons/plus.svg"; enabled: roughCut.resultCount > 0; onClicked: roughTimeline.adjustZoomPercent(10) }
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
                        // 选中块与点击位置独立；onUserSeeked 保留用户落点。
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
        height: roughCut.busy ? 42 : 0
        visible: roughCut.busy
        color: Theme.panelRaised
        z: 20
        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 8
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

    Action { id: roughCommand0; text: qsTr("上一结果"); property string keySequence: "PageUp"; enabled: true; onTriggered: window.adjacentResult(-1) }
    Shortcut { sequence: roughCommand0.keySequence; enabled: roughCommand0.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand0.trigger() }
    Action { id: roughCommand1; text: qsTr("下一结果"); property string keySequence: "PageDown"; enabled: true; onTriggered: window.adjacentResult(1) }
    Shortcut { sequence: roughCommand1.keySequence; enabled: roughCommand1.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand1.trigger() }
    Action { id: roughCommand2; text: qsTr("上一条待复核"); property string keySequence: "Shift+Up"; enabled: true; onTriggered: window.goToReview(-1) }
    Shortcut { sequence: roughCommand2.keySequence; enabled: roughCommand2.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand2.trigger() }
    Action { id: roughCommand3; text: qsTr("下一条待复核"); property string keySequence: "Shift+Down"; enabled: true; onTriggered: window.goToReview(1) }
    Shortcut { sequence: roughCommand3.keySequence; enabled: roughCommand3.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand3.trigger() }
    Action { id: roughCommand4; text: qsTr("播放／暂停"); property string keySequence: "Space"; enabled: true; onTriggered: window.selectedMonitor === 1 ? roughCut.toggleTimelinePlay() : roughCut.togglePlay() }
    Shortcut { sequence: roughCommand4.keySequence; enabled: roughCommand4.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand4.trigger() }
    Action { id: roughCommand5; text: qsTr("撤销"); property string keySequence: "Ctrl+Z"; enabled: !roughCut.busy; onTriggered: roughCut.undo() }
    Shortcut { sequence: roughCommand5.keySequence; enabled: roughCommand5.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand5.trigger() }
    Action { id: roughCommand6; text: qsTr("重做"); property string keySequence: "Ctrl+Y"; enabled: !roughCut.busy; onTriggered: roughCut.redo() }
    Shortcut { sequence: roughCommand6.keySequence; enabled: roughCommand6.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand6.trigger() }
    Action { id: roughCommand7; text: qsTr("停止播放"); property string keySequence: "K"; enabled: true; onTriggered: { if (window.selectedMonitor === 1) roughCut.stopTimeline(); else if (roughCut.playing) roughCut.togglePlay() } }
    Shortcut { sequence: roughCommand7.keySequence; enabled: roughCommand7.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand7.trigger() }
    Action { id: roughCommand8; text: qsTr("后退一帧"); property string keySequence: "Left"; enabled: true; onTriggered: window.frameStep(-1) }
    Shortcut { sequence: roughCommand8.keySequence; enabled: roughCommand8.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand8.trigger() }
    Action { id: roughCommand9; text: qsTr("前进一帧"); property string keySequence: "Right"; enabled: true; onTriggered: window.frameStep(1) }
    Shortcut { sequence: roughCommand9.keySequence; enabled: roughCommand9.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand9.trigger() }
    Action { id: roughCommand10; text: qsTr("后退五帧"); property string keySequence: "Shift+Left"; enabled: true; onTriggered: window.frameStep(-5) }
    Shortcut { sequence: roughCommand10.keySequence; enabled: roughCommand10.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand10.trigger() }
    Action { id: roughCommand11; text: qsTr("前进五帧"); property string keySequence: "Shift+Right"; enabled: true; onTriggered: window.frameStep(5) }
    Shortcut { sequence: roughCommand11.keySequence; enabled: roughCommand11.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand11.trigger() }
    Action { id: roughCommand12; text: qsTr("放大当前时间轴"); property string keySequence: "="; enabled: true; onTriggered: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).adjustZoomPercent(10) }
    Shortcut { sequence: roughCommand12.keySequence; enabled: roughCommand12.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand12.trigger() }
    Action { id: roughCommand13; text: qsTr("缩小当前时间轴"); property string keySequence: "-"; enabled: true; onTriggered: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).adjustZoomPercent(-10) }
    Shortcut { sequence: roughCommand13.keySequence; enabled: roughCommand13.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand13.trigger() }
    Action { id: roughCommand14; text: qsTr("适配当前时间轴"); property string keySequence: "\\"; enabled: true; onTriggered: (window.selectedMonitor === 1 ? roughTimeline : sourceWaveform).fit() }
    Shortcut { sequence: roughCommand14.keySequence; enabled: roughCommand14.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand14.trigger() }
    Action { id: roughCommand15; text: qsTr("上一条待复核"); property string keySequence: "Up"; enabled: true; onTriggered: window.goToReview(-1) }
    Shortcut { sequence: roughCommand15.keySequence; enabled: roughCommand15.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand15.trigger() }
    Action { id: roughCommand16; text: qsTr("下一条待复核"); property string keySequence: "Down"; enabled: true; onTriggered: window.goToReview(1) }
    Shortcut { sequence: roughCommand16.keySequence; enabled: roughCommand16.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand16.trigger() }
    Action { id: roughCommand17; text: qsTr("保留"); property string keySequence: "1"; enabled: !roughCut.busy; onTriggered: window.decideCurrent("KEEP") }
    Shortcut { sequence: roughCommand17.keySequence; enabled: roughCommand17.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand17.trigger() }
    Action { id: roughCommand18; text: qsTr("待复核"); property string keySequence: "2"; enabled: !roughCut.busy; onTriggered: window.decideCurrent("REVIEW") }
    Shortcut { sequence: roughCommand18.keySequence; enabled: roughCommand18.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand18.trigger() }
    Action { id: roughCommand19; text: qsTr("剪除"); property string keySequence: "3"; enabled: !roughCut.busy; onTriggered: window.decideCurrent("CUT") }
    Shortcut { sequence: roughCommand19.keySequence; enabled: roughCommand19.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand19.trigger() }
    Action { id: roughCommand20; text: qsTr("试听当前结果"); property string keySequence: "Return"; enabled: true; onTriggered: window.auditionCurrent() }
    Shortcut { sequence: roughCommand20.keySequence; enabled: roughCommand20.enabled && (visible && !window.textEditing && !window.modalEditing); onActivated: roughCommand20.trigger() }
    Shortcut { sequence: "Enter"; enabled: roughCommand20.enabled && window.visible && !window.textEditing && !window.modalEditing; onActivated: roughCommand20.trigger() }
    Action { id: roughCommand21; text: qsTr("恢复自动判断"); property string keySequence: "0"; enabled: !roughCut.busy; onTriggered: window.restoreCurrent() }
    Shortcut { sequence: roughCommand21.keySequence; enabled: roughCommand21.enabled && (visible && !window.textEditing && !window.modalEditing && !roughCut.busy); onActivated: roughCommand21.trigger() }
}
