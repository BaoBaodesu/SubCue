import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: window
    objectName: "subtitleWorkspace"

    property bool auxiliaryCollapsed: false
    property bool externalModalEditing: false
    readonly property bool textEditing: window.Window.window && window.Window.window.activeFocusItem
        && typeof window.Window.window.activeFocusItem.inputMethodComposing !== "undefined"
    readonly property bool modalEditing: externalModalEditing || mediaDialog.visible || scriptDialog.visible
        || exportNotice.visible || exportFolderDialog.visible || exportResultDialog.visible
        || preflightDialog.visible || omniSuggestionDialog.visible || activeSubtitlesDialog.visible
        || (settingsWindow && settingsWindow.visible) || fileMenu.visible || aiReviewMenu.visible
        || cueEditDialog.visible || relinkDialog.visible || timedSubtitleDialog.visible || subtitlePlaybackRate.popup.visible
    property bool draftActive: false
    property bool draftChanged: false
    property bool draftLoading: false
    property string draftId: ""
    property string draftError: ""
    property var inspectorCue: ({})
    property string originalText: ""
    property real originalStartUs: -1
    property real originalEndUs: -1
    property bool switchingSelection: false
    property bool taskDetailsExpanded: false
    Connections {
        target: window.Window.window
        function onActiveFocusItemChanged() {
            if (!window.visible || window.draftLoading || !window.draftChanged) return
            const item = window.Window.window.activeFocusItem
            window.ensureInspectorVisible(item)
            if (item && !window.belongsTo(item, inspectorRegion) && !window.modalEditing && !window.commitPendingCue())
                cueTextEditor.forceActiveFocus(Qt.OtherFocusReason)
        }
    }
    Dialog {
        id: cueEditDialog; objectName: "cueEditDialog"
        anchors.centerIn: parent; modal: true
        title: window.draftId ? qsTr("编辑字幕") : qsTr("新建字幕")
        width: Math.min(600, window.width - 32); height: Math.min(410, window.height - 32)
        closePolicy: Popup.CloseOnEscape
        onRejected: window.discardDraft()
        Shortcut { sequence: "Ctrl+S"; enabled: cueEditDialog.visible && !window.externalModalEditing; onActivated: if (window.commitPendingCue()) { cueEditDialog.close(); window.Window.window.saveAction.trigger() } }
        contentItem: ColumnLayout {
            id: inspectorRegion
            spacing: 8
            Flickable {
                id: inspectorScroll; objectName: "cueInspectorScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true; Layout.minimumHeight: 40
                boundsBehavior: Flickable.StopAtBounds; flickableDirection: Flickable.VerticalFlick
                contentWidth: width; contentHeight: cueInspector.implicitHeight + 16
                clip: true
                ScrollBar.vertical: SubScrollBar { orientation: Qt.Vertical; parent: inspectorScroll; anchors.top: parent.top; anchors.right: parent.right; anchors.bottom: parent.bottom }
                ColumnLayout {
                    id: cueInspector; objectName: "cueInspector"
                    width: inspectorScroll.width - 16 - Theme.scrollBarSize; x: 8; y: 8; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: window.draftId ? qsTr("字幕内容") : qsTr("新建字幕"); color: Theme.text; font.bold: true }
                        Label { text: window.draftChanged ? qsTr("未应用") : ""; color: Theme.warning; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                        SubCheckBox { id: cueTimed; text: qsTr("已定位"); enabled: window.draftActive && !editor.busy && window.originalStartUs < 0; onCheckedChanged: if (!window.draftLoading) window.draftChanged = true }
                    }
                    ScrollView {
                        id: cueTextScroll
                        Layout.fillWidth: true; Layout.preferredHeight: 112; Layout.maximumHeight: 160
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        TextArea {
                            id: cueTextEditor; objectName: "cueTextEditor"
                            width: cueTextScroll.availableWidth
                            enabled: window.draftActive && !editor.busy
                            placeholderText: qsTr("选择字幕，或按 Enter 新建")
                            color: Theme.text; wrapMode: TextEdit.Wrap; selectByMouse: true
                            background: Rectangle { color: Theme.input; border.color: cueTextEditor.activeFocus ? Theme.focusBorder : Theme.border; radius: Theme.radiusInput }
                            onTextChanged: if (!window.draftLoading) window.draftChanged = true
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Escape) { window.discardDraft(); event.accepted = true }
                                else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !inputMethodComposing && !(event.modifiers & Qt.ShiftModifier)) {
                                    if (window.commitPendingCue() && (event.modifiers & Qt.ControlModifier)) editor.confirmCurrentCue()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 4
                        SubTextField { id: cueStart; objectName: "cueStartTime"; Layout.fillWidth: true; enabled: window.draftActive && cueTimed.checked && !editor.busy; font.family: "Consolas"; font.pixelSize: 12; onTextChanged: if (!window.draftLoading) window.draftChanged = true; onAccepted: window.commitPendingCue() }
                        Label { text: "→"; color: Theme.secondaryText }
                        SubTextField { id: cueEnd; objectName: "cueEndTime"; Layout.fillWidth: true; enabled: window.draftActive && cueTimed.checked && !editor.busy; font.family: "Consolas"; font.pixelSize: 12; onTextChanged: if (!window.draftLoading) window.draftChanged = true; onAccepted: window.commitPendingCue() }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: window.draftActive
                        text: cueTimed.checked ? qsTr("时长 %1 秒").arg(((window.parseTime(cueEnd.text) - window.parseTime(cueStart.text)) / 1000000).toFixed(3)) : qsTr("未定位 · 应用时保留为文稿字幕")
                        color: Theme.secondaryText; font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true; visible: window.draftError !== "" || editor.cueOverlaps(window.draftId)
                        text: window.draftError || qsTr("当前字幕存在重叠；工程可保存，字幕导出前须处理。")
                        color: Theme.warning; wrapMode: Text.Wrap; font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true; font.pixelSize: 12; color: Theme.secondaryText
                        visible: window.inspectorCue.status === "LOW_CONFIDENCE" || window.inspectorCue.status === "REVIEW" || !!window.inspectorCue.candidateText
                        text: qsTr("复核：") + (window.inspectorCue.status === "LOW_CONFIDENCE" ? qsTr("低置信度") : qsTr("待复核"))
                        + (window.inspectorCue.candidateText ? " · " + window.inspectorCue.candidateText : "")
                        elide: Text.ElideRight
                        ToolTip.visible: reviewInfoHover.hovered; ToolTip.text: text
                        HoverHandler { id: reviewInfoHover }
                    }

                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 4
                SubButton { text: qsTr("应用并关闭"); enabled: window.draftChanged && !editor.busy; onClicked: if (window.commitPendingCue()) cueEditDialog.close() }
                SubButton { text: qsTr("取消"); enabled: !editor.busy; onClicked: window.discardDraft() }
                Item { Layout.fillWidth: true }
                SubButton { text: qsTr("确认并下一条"); shortcutHint: "Ctrl+Enter"; enabled: window.draftActive && !editor.busy; leftPadding: 6; rightPadding: 6; onClicked: window.runCommand(function() { editor.confirmCurrentCue() }) }
            }
        }
    }
    Action { id: alignmentAction; text: qsTr("自动打轴"); enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.startAlignment() }) }
    readonly property var commandActions: [subtitleCommand0, subtitleCommand1, subtitleCommand2, subtitleCommand3, subtitleCommand4, subtitleCommand5, subtitleCommand6, subtitleCommand7, subtitleCommand8, subtitleCommand9, subtitleCommand10, subtitleCommand11, subtitleCommand12, subtitleCommand13, subtitleCommand14, subtitleCommand15, subtitleCommand16, subtitleCommand17, subtitleCommand18, subtitleCommand19, subtitleCommand20, subtitleCommand21, subtitleCommand22, subtitleCommand23, subtitleCommand24, subtitleCommand25, subtitleCommand26, subtitleCommand27, subtitleCommand28, subtitleCommand29, subtitleCommand30, subtitleCommand31, subtitleCommand32, subtitleCommand33, subtitleCommand34, subtitleCommand35, subtitleCommand36, subtitleCommand37, subtitleCommand38, subtitleCommand39, subtitleCommand40, subtitleCommand41, subtitleCommand42, subtitleCommand43, subtitleCommand44, subtitleCommand45, subtitleCommand46, subtitleCommand47, subtitleCommand48, subtitleCommand49]
    function parseTime(text) {
        const match = /^(\d+):([0-5]\d):([0-5]\d)\.(\d{3})$/.exec(text.trim())
        return match ? (Number(match[1]) * 3600000 + Number(match[2]) * 60000 + Number(match[3]) * 1000 + Number(match[4])) * 1000 : NaN
    }
    function loadInspector(id, text, startUs, endUs) {
        draftLoading = true
        draftId = id || ""
        const row = editor.cueRowForId(draftId)
        const cue = row >= 0 ? editor.subtitleModel.get(row) : null
        inspectorCue = cue || {}
        draftActive = cue !== null || text !== undefined
        originalText = text !== undefined ? text : cue ? cue.text : ""
        originalStartUs = startUs !== undefined ? startUs : cue && cue.timed ? cue.startUs : -1
        originalEndUs = endUs !== undefined ? endUs : cue && cue.timed ? cue.endUs : -1
        cueTextEditor.text = originalText
        cueTimed.checked = originalStartUs >= 0 && originalEndUs > originalStartUs
        cueStart.text = editor.formatTime(Math.max(0, originalStartUs) / 1000)
        cueEnd.text = editor.formatTime(Math.max(0, originalEndUs) / 1000)
        draftChanged = text !== undefined && (!cue || cue.text !== originalText
            || (cue.timed ? cue.startUs : -1) !== originalStartUs || (cue.timed ? cue.endUs : -1) !== originalEndUs)
        draftError = ""
        draftLoading = false
    }
    function commitPendingCue() {
        if (!draftActive || !draftChanged) return true
        if (cueTextEditor.inputMethodComposing || cueStart.inputMethodComposing || cueEnd.inputMethodComposing) {
            draftError = qsTr("请先完成输入法候选确认。")
            return false
        }
        let start = -1, end = -1
        if (cueTimed.checked) {
            start = originalStartUs >= 0 && cueStart.text === editor.formatTime(originalStartUs / 1000) ? originalStartUs : parseTime(cueStart.text)
            end = originalEndUs >= 0 && cueEnd.text === editor.formatTime(originalEndUs / 1000) ? originalEndUs : parseTime(cueEnd.text)
            if (!isFinite(start) || !isFinite(end)) {
                draftError = qsTr("时间格式应为 HH:MM:SS.mmm，例如 00:01:23.450。")
                return false
            }
        }
        draftLoading = true
        const applied = editor.applyCueEdit(draftId, cueTextEditor.text, start, end)
        draftLoading = false
        if (!applied) { draftError = editor.statusText; return false }
        loadInspector(editor.selectedCueId)
        return true
    }
    function discardDraft() {
        cueEditDialog.close()
        loadInspector(editor.selectedCueId)
        timelineScene.forceActiveFocus(Qt.OtherFocusReason)
    }
    function selectById(id, seek) {
        if (!commitPendingCue()) return false
        const row = editor.cueRowForId(id)
        if (row < 0) return false
        editor.selectCue(row, seek)
        return true
    }
    function navigateFiltered(delta) {
        const model = editor.filteredSubtitleModel
        if (!model.count) return
        const row = model.rowForId(editor.selectedCueId)
        const next = row < 0 ? (delta > 0 ? 0 : model.count - 1) : Math.max(0, Math.min(model.count - 1, row + delta))
        window.selectById(model.get(next).id, true)
    }
    function runCommand(operation) {
        if (!commitPendingCue()) return
        operation()
    }
    function ensureInspectorVisible(item) {
        if (!item || !window.belongsTo(item, cueInspector)) return
        if (item === cueTextEditor) item = cueTextScroll
        const top = item.mapToItem(cueInspector, 0, 0).y + 8
        if (top < inspectorScroll.contentY) inspectorScroll.contentY = top
        else if (top + item.height > inspectorScroll.contentY + inspectorScroll.height)
            inspectorScroll.contentY = Math.max(0, Math.min(inspectorScroll.contentHeight - inspectorScroll.height, top + item.height - inspectorScroll.height))
    }
    function belongsTo(item, area) {
        while (item) { if (item === area) return true; item = item.parent }
        return false
    }
    function cycleFocus(direction) {
        if (!commitPendingCue()) return
        const areas = [sourceTabs.currentIndex === 1 ? scriptEditor : assetSearch, preview, cueList, cueTextEditor, timelineScene]
            .filter(function(item) {
                let current = item
                while (current) { if (!current.visible || !current.enabled) return false; current = current.parent }
                return item.width > 0 && item.height > 0
            })
        const current = areas.findIndex(function(item) { return window.belongsTo(window.Window.window.activeFocusItem, item) })
        if (areas.length) areas[(current + direction + areas.length) % areas.length].forceActiveFocus(Qt.TabFocusReason)
    }
    function captureSession() {
        editor.setSessionState(Object.assign({}, editor.sessionState, {
            auxiliaryCollapsed: auxiliaryCollapsed, topRatio: workspaceSplit.height / Math.max(1, verticalSplit.height),
            auxiliaryRatio: auxiliaryPanel.width / Math.max(1, workspaceSplit.width),
            subtitleRatio: subtitlePanel.width / Math.max(1, workspaceSplit.width), sourceTab: sourceTabs.currentIndex,
            subtitleFilter: editor.filteredSubtitleModel.statusFilter, subtitleSearch: cueSearch.text,
            subtitleScrollOffset: timelineScene.subtitleScrollOffset
        }))
    }
    function restoreLayout() {
        workspaceSplit.SplitView.preferredHeight = verticalSplit.height * Math.max(0.4, Math.min(0.7, editor.sessionState.topRatio || 0.55))
        auxiliaryPanel.SplitView.preferredWidth = workspaceSplit.width * Math.max(0.12, Math.min(0.3, editor.sessionState.auxiliaryRatio || 0.18))
        subtitlePanel.SplitView.preferredWidth = Math.max(320, workspaceSplit.width * Math.max(0.25, Math.min(0.45, editor.sessionState.subtitleRatio || 0.32)))
    }
    function resetLayout() {
        auxiliaryCollapsed = false
        workspaceSplit.SplitView.preferredHeight = verticalSplit.height * 0.55
        auxiliaryPanel.SplitView.preferredWidth = workspaceSplit.width * 0.18
        subtitlePanel.SplitView.preferredWidth = workspaceSplit.width * 0.32
    }
    FileDialog { id: relinkDialog; title: qsTr("重新定位原素材"); parentWindow: window.Window.window; onAccepted: editor.relinkMedia(selectedFile) }
    FileDialog { id: timedSubtitleDialog; title: qsTr("按时间码导入字幕"); nameFilters: [qsTr("SRT 字幕 (*.srt)")]; parentWindow: window.Window.window; onAccepted: { const url = selectedFile; window.requestSubtitleLeave(function() { editor.importTimedSubtitles(url) }) } }
    signal exportCancelled()

    Component.onCompleted: {
        editor.setPreviewItem(preview)
        editor.setTimelineItem(timelineScene)
        auxiliaryCollapsed = editor.sessionState.auxiliaryCollapsed || false
        sourceTabs.currentIndex = editor.sessionState.sourceTab || 0
        editor.filteredSubtitleModel.statusFilter = editor.sessionState.subtitleFilter || "ALL"
        cueSearch.text = editor.sessionState.subtitleSearch || ""
        timelineScene.subtitleScrollOffset = editor.sessionState.subtitleScrollOffset || 0
        loadInspector(editor.selectedCueId)
    }
    Connections { target: editor; function onProjectChanged() {
        window.restoreLayout()
        window.auxiliaryCollapsed = editor.sessionState.auxiliaryCollapsed || false
        sourceTabs.currentIndex = editor.sessionState.sourceTab || 0
        editor.filteredSubtitleModel.statusFilter = editor.sessionState.subtitleFilter || "ALL"
        cueSearch.text = editor.sessionState.subtitleSearch || ""
        timelineScene.subtitleScrollOffset = editor.sessionState.subtitleScrollOffset || 0
        window.loadInspector(editor.selectedCueId)
    } }

    Component {
        id: settingsWindowComponent
        SettingsWindow { controller: editor }
    }
    property var settingsWindow: null
    property var preflightIssues: []
    function openSettings() {
        if (!settingsWindow)
            settingsWindow = settingsWindowComponent.createObject(window)
        settingsWindow.show()
    }

    function requestExport() {
        if (!commitPendingCue()) return
        if (editor.overlapCount > 0) {
            editor.filteredSubtitleModel.statusFilter = "OVERLAP"
            editor.navigateOverlap(1)
            draftError = qsTr("请先处理全部重叠字幕，再导出。")
            return
        }
        let pending = 0
        let unlocated = 0
        for (let i = 0; i < editor.subtitleModel.count; ++i) {
            const cue = editor.subtitleModel.get(i)
            if (!cue.timed) ++unlocated
            else if (cue.status === "LOW_CONFIDENCE" || cue.status === "REVIEW") ++pending
        }
        exportNotice.text = "可导出 " + (editor.subtitleModel.count - unlocated) + " 条；待确认 " + pending
                + " 条将保留导出；未定位 " + unlocated + " 条不导出。"
        exportDirectory.text = editor.setting("outputDirectory") || ""
        exportNotice.open()
    }
    Dialog {
        id: exportNotice
        property string text: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("导出字幕")
        implicitWidth: 480
        width: Math.min(520, Math.max(400, window.width - 48))
        padding: 16
        standardButtons: Dialog.NoButton
        palette.window: Theme.panel
        palette.windowText: Theme.text
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.border
            radius: 2
        }
        contentItem: ColumnLayout {
            spacing: 12
            width: exportNotice.availableWidth
            Label {
                text: exportNotice.text
                color: Theme.text
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.preferredWidth: exportNotice.availableWidth
            }
            Label { text: qsTr("导出文件夹"); color: Theme.muted }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                SubTextField {
                    id: exportDirectory
                    Layout.fillWidth: true
                    placeholderText: qsTr("留空使用媒体所在文件夹")
                }
                SubButton { text: qsTr("浏览…"); onClicked: exportFolderDialog.open() }
            }
        }
        footer: DialogButtonBox {
            implicitHeight: 48
            alignment: Qt.AlignRight
            background: Rectangle { color: Theme.panelRaised }
            SubButton {
                text: qsTr("取消")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            PrimaryButton {
                text: qsTr("导出"); objectName: "subtitleExportConfirm"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
        onAccepted: editor.exportSubtitles(exportDirectory.text)
        onRejected: window.exportCancelled()
    }
    FolderDialog {
        id: exportFolderDialog
        title: qsTr("选择导出文件夹")
        onAccepted: exportDirectory.text = editor.localPath(selectedFolder)
    }
    Dialog {
        id: omniSuggestionDialog
        property int row: -1
        property var inspectorCue: ({})
    property string originalText: ""
        property string suggestedText: ""
        property string reason: ""
        property real confidence: 0
        anchors.centerIn: parent
        modal: true
        title: qsTr("字幕 AI Review")
        standardButtons: Dialog.NoButton
        width: 460
        contentItem: ColumnLayout {
            spacing: 8
            Label { text: qsTr("原字幕"); color: Theme.muted }
            Label { text: omniSuggestionDialog.originalText; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: qsTr("AI 建议"); color: Theme.muted }
            Label { text: omniSuggestionDialog.suggestedText || qsTr("（无文本修改）"); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: qsTr("原因"); color: Theme.muted }
            Label { text: omniSuggestionDialog.reason; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: qsTr("置信度：") + Math.round(omniSuggestionDialog.confidence * 100) + "%"; color: Theme.secondaryText }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                SubButton { text: qsTr("忽略"); onClicked: { editor.ignoreOmniSubtitleSuggestion(omniSuggestionDialog.row); omniSuggestionDialog.close() } }
                PrimaryButton { text: qsTr("接受"); onClicked: { editor.acceptOmniSubtitleSuggestion(omniSuggestionDialog.row); omniSuggestionDialog.close() } }
            }
        }
    }
    Dialog {
        id: exportResultDialog; objectName: "subtitleExportResult"
        property string text: ""
        anchors.centerIn: parent
        modal: true
        implicitWidth: 480
        width: Math.min(520, Math.max(400, window.width - 48))
        padding: 16
        standardButtons: Dialog.NoButton
        palette.window: Theme.panel
        palette.windowText: Theme.text
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.border
            radius: 2
        }
        contentItem: ColumnLayout {
            spacing: 10
            width: exportResultDialog.availableWidth
            Label {
                text: exportResultDialog.text
                color: Theme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredWidth: exportResultDialog.availableWidth
            }
        }
        footer: DialogButtonBox {
            implicitHeight: 48
            alignment: Qt.AlignRight
            background: Rectangle { color: Theme.panelRaised }
            PrimaryButton {
                text: qsTr("确定")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
    }

    Connections {
        target: editor
        function onAlignmentPreflightFailed(issues) {
            window.preflightIssues = issues
            preflightDialog.open()
        }
        function onExportFinished(success, message, paths) {
            if (window.Window.window && typeof window.Window.window.showResult === "function") return
            exportResultDialog.title = success ? qsTr("导出成功") : qsTr("导出失败")
            exportResultDialog.text = message
            exportResultDialog.open()
        }

    }

    Component {
        id: aboutWindowComponent
        AboutWindow { }
    }
    property var aboutWindow: null
    function openAbout() {
        if (!aboutWindow)
            aboutWindow = aboutWindowComponent.createObject(window)
        aboutWindow.show()
    }

    FileDialog {
        id: mediaDialog
        objectName: "mediaImportDialog"
        title: qsTr("导入素材")
        parentWindow: window.Window.window
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("支持的素材 (*.wav *.mp3 *.m4a *.flac *.aac *.mp4 *.mov *.mkv *.webm *.avi *.m4v *.wmv *.mpg *.mpeg *.mts *.m2ts *.ts *.mxf *.ogg *.opus *.wma *.aif *.aiff *.txt *.srt *.docx)"), qsTr("所有文件 (*)")]
        onAccepted: editor.importFiles(selectedFiles)
    }
    FileDialog {
        id: scriptDialog
        title: qsTr("导入字幕文稿")
        parentWindow: window.Window.window
        nameFilters: [qsTr("字幕文稿 (*.txt *.srt *.docx)"), qsTr("所有文件 (*)")]
        onAccepted: editor.importScript(selectedFile)
    }
    function requestSubtitleLeave(action) {
        const win = window.Window.window
        if (!window.commitPendingCue()) return
        if (win && typeof win.requestSubtitleDestructiveAction === "function")
            win.requestSubtitleDestructiveAction(action)
        else
            action()
    }
    function openMediaDialog() { requestSubtitleLeave(function() { mediaDialog.open() }) }
    function openScriptDialog() { requestSubtitleLeave(function() { scriptDialog.open() }) }

    Connections {
        target: editor
        function onScriptTextChanged() { if (!editor.projectPath) sourceTabs.currentIndex = 1 }
        function onCueDraftRequested(id, text, startUs, endUs) {
            if (!window.commitPendingCue()) return
            window.loadInspector(id, text, startUs, endUs)
            cueEditDialog.open()
            cueTextEditor.forceActiveFocus(Qt.OtherFocusReason)
            cueTextEditor.selectAll()
        }
        function onSelectedCueChanged() {
            if (window.draftLoading || window.switchingSelection) return
            const next = editor.selectedCueId
            if (window.draftId && editor.cueRowForId(window.draftId) < 0) {
                window.loadInspector(next)
                return
            }
            if (window.draftChanged && next !== window.draftId) {
                window.switchingSelection = true
                const previous = window.draftId
                if (window.commitPendingCue()) {
                    const row = editor.cueRowForId(next)
                    if (row >= 0) editor.selectCue(row, false)
                    window.loadInspector(next)
                } else {
                    const row = editor.cueRowForId(previous)
                    if (row >= 0) editor.selectCue(row, false)
                }
                window.switchingSelection = false
            } else if (!window.draftChanged) window.loadInspector(next)
        }
    }

    Rectangle {
        id: toolbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40
        color: Theme.menuBar
        z: 10
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.divider
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 4

            MenuButton {
                id: fileButton
                text: qsTr("文件")
                onClicked: fileMenu.popup(fileButton, 0, fileButton.height)
                SubMenu {
                    id: fileMenu
                    MenuItem { action: window.Window.window.newAction; text: action.text + "    " + action.keySequence }
                    MenuItem { action: window.Window.window.openAction; text: action.text + "    " + action.keySequence }
                    MenuItem { action: window.Window.window.saveAction; text: action.text + "    " + action.keySequence }
                    MenuItem { action: window.Window.window.saveAsAction; text: action.text + "    " + action.keySequence }
                    MenuItem { text: qsTr("重新定位素材…"); enabled: editor.mediaPath !== "" && !editor.busy; onTriggered: relinkDialog.open() }
                    MenuItem { text: qsTr("按时间码导入字幕…"); enabled: !editor.busy; onTriggered: timedSubtitleDialog.open() }
                    MenuSeparator { }
                    MenuItem { action: window.Window.window.importAction; text: action.text + "    " + action.keySequence }
                    MenuItem { action: window.Window.window.scriptAction; text: action.text + "    " + action.keySequence }
                    MenuSeparator { }
                    MenuItem { action: window.Window.window.exportAction; text: action.text + "    " + action.keySequence }
                    MenuItem { text: window.auxiliaryCollapsed ? qsTr("展开辅助栏") : qsTr("折叠辅助栏"); onTriggered: { window.auxiliaryCollapsed = !window.auxiliaryCollapsed; editor.setSessionState(Object.assign({}, editor.sessionState, { auxiliaryCollapsed: window.auxiliaryCollapsed })) } }
                    MenuSeparator { }
                    MenuItem { text: qsTr("ASR 复核"); enabled: editor.canReview && !editor.busy; onTriggered: window.runCommand(function() { editor.startReview() }) }
                    MenuItem { text: qsTr("AI 内容复核"); enabled: editor.canOmniReview && !editor.busy; onTriggered: window.runCommand(function() { editor.startOmniSubtitleReview() }) }
                    MenuItem { text: qsTr("关于"); onTriggered: window.openAbout() }
                    MenuItem { text: qsTr("重置布局"); onTriggered: window.resetLayout() }
                    MenuItem { text: qsTr("退出"); onTriggered: window.Window.window.close() }
                }
            }
            MenuButton { action: window.Window.window.importAction }
            MenuButton { action: window.Window.window.saveAction }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { action: alignmentAction }
            MenuButton { text: qsTr("取消"); visible: editor.busy; enabled: editor.busy; onClicked: editor.cancelAlignment() }
            MenuButton { text: qsTr("ASR 复核"); visible: window.width >= 1200; enabled: editor.canReview && !editor.busy; onClicked: editor.startReview() }
            MenuButton {
                id: aiReviewButton
                objectName: "aiReviewMenuButton"
                visible: window.width >= 1200
                text: qsTr("AI 复核")
                enabled: editor.canOmniReview && !editor.busy
                onClicked: aiReviewMenu.popup(aiReviewButton, 0, aiReviewButton.height)
                SubMenu {
                    id: aiReviewMenu
                    MenuItem {
                        objectName: "wordMappingReviewMenuItem"
                        text: qsTr("复核时间映射")
                        onTriggered: editor.startWordMappingReview()
                    }
                    MenuItem {
                        objectName: "subtitleContentReviewMenuItem"
                        text: qsTr("复核字幕内容")
                        onTriggered: editor.startOmniSubtitleReview()
                    }
                }
            }
            MenuButton { action: window.Window.window.exportAction }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("设置"); onClicked: window.openSettings() }
            MenuButton { text: qsTr("关于"); visible: window.width >= 1300; onClicked: window.openAbout() }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: editor.busy
                visible: running
                implicitWidth: 24
                implicitHeight: 24
                palette.dark: Theme.accent
            }
        }
    }

    SplitView {
        id: verticalSplit
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: alignmentProgressDialog.bottom
        anchors.bottom: statusBar.top
        orientation: Qt.Vertical
        handle: Rectangle {
            implicitWidth: 5
            implicitHeight: 5
            color: Theme.divider
        }

        SplitView {
            id: workspaceSplit
            orientation: Qt.Horizontal
            SplitView.preferredHeight: verticalSplit.height * Math.max(0.4, Math.min(0.7, editor.sessionState.topRatio || 0.55))
            SplitView.minimumHeight: Math.min(270, verticalSplit.height * 0.55)
            handle: Rectangle {
                implicitWidth: 5
                implicitHeight: 5
                color: Theme.divider
            }

            Rectangle {
                id: auxiliaryPanel
                visible: !window.auxiliaryCollapsed
                color: Theme.panel
                border.color: window.Window.window && window.belongsTo(window.Window.window.activeFocusItem, auxiliaryPanel) ? Theme.focusBorder : Theme.border
                SplitView.preferredWidth: workspaceSplit.width * Math.max(0.12, Math.min(0.3, editor.sessionState.auxiliaryRatio || 0.18))
                SplitView.minimumWidth: Math.min(240, window.width * 0.25)
                SplitView.maximumWidth: 480
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        MenuButton { text: qsTr("项目：素材"); onClicked: if (window.commitPendingCue()) sourceTabs.currentIndex = 0; highlighted: sourceTabs.currentIndex === 0 }
                        MenuButton { text: qsTr("文稿"); onClicked: if (window.commitPendingCue()) sourceTabs.currentIndex = 1; highlighted: sourceTabs.currentIndex === 1 }
                        Item { Layout.fillWidth: true }
                        Label { text: editor.projectFiles.length + qsTr(" 项"); color: Theme.muted }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                    StackLayout {
                        id: sourceTabs
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: 0
                        ColumnLayout {
                            spacing: 6
                            SubTextField {
                                id: assetSearch
                                objectName: "assetSearch"
                                Layout.fillWidth: true
                                Layout.margins: 8
                                placeholderText: qsTr("搜索项目素材")
                                Accessible.name: qsTr("搜索项目素材")
                            }
                            ListView {
                                id: assetList
                                objectName: "projectFileList"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: editor.projectFiles
                                currentIndex: -1
                                ScrollBar.vertical: SubScrollBar { }
                                MouseArea {
                                    anchors.fill: parent
                                    z: -1
                                    onDoubleClicked: window.openMediaDialog()
                                }
                                Label {
                                    anchors.centerIn: parent
                                    width: parent.width - 24
                                    visible: editor.projectFiles.length === 0
                                    text: qsTr("双击此处导入素材\n或从资源管理器拖入文件")
                                    color: Theme.muted
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    lineHeight: 1.6
                                }
                                delegate: ItemDelegate {
                                    id: assetRow
                                    objectName: "projectAssetRow"
                                    required property int index
                                    required property var modelData
                                    width: ListView.view.width
                                    visible: modelData.name.toLowerCase().indexOf(assetSearch.text.toLowerCase()) >= 0
                                    height: visible ? 56 : 0
                                    highlighted: assetList.currentIndex === index
                                    Accessible.name: modelData.name
                                    background: Rectangle {
                                        color: assetRow.highlighted ? Theme.selection : (assetRow.hovered ? Theme.listHover : Theme.panel)
                                        Rectangle { width: 3; height: parent.height; color: Theme.accent; visible: assetRow.modelData.path === editor.mediaPath }
                                    }
                                    contentItem: ColumnLayout {
                                        spacing: 3
                                        Label { text: assetRow.modelData.name; color: Theme.text; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                        Label {
                                            text: assetRow.modelData.type + (assetRow.modelData.durationMs > 0 ? "  ·  " + editor.formatTime(assetRow.modelData.durationMs) : "")
                                            color: Theme.secondaryText
                                            font.pixelSize: Theme.fontSizeTiny
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                    onClicked: { assetList.currentIndex = index; assetList.forceActiveFocus() }
                                    onDoubleClicked: { const row = index; window.requestSubtitleLeave(function() { editor.openProjectFile(row) }) }
                                    TapHandler {
                                        acceptedButtons: Qt.RightButton
                                        onTapped: { assetList.currentIndex = assetRow.index; assetMenu.popup() }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                SubButton { text: qsTr("导入…"); onClicked: window.openMediaDialog() }
                                SubButton { text: qsTr("打开"); enabled: assetList.currentIndex >= 0; onClicked: { const row = assetList.currentIndex; window.requestSubtitleLeave(function() { editor.openProjectFile(row) }) } }
                                Item { Layout.fillWidth: true }
                                SubButton { text: qsTr("移除"); enabled: assetList.currentIndex >= 0; onClicked: { editor.removeProjectFile(assetList.currentIndex); assetList.currentIndex = -1 } }
                            }
                            SubMenu {
                                id: assetMenu
                                MenuItem { text: qsTr("打开素材"); onTriggered: { const row = assetList.currentIndex; window.requestSubtitleLeave(function() { editor.openProjectFile(row) }) } }
                                MenuItem { text: qsTr("在资源管理器中打开文件夹"); onTriggered: editor.showProjectFileFolder(assetList.currentIndex) }
                                MenuSeparator { }
                                MenuItem { text: qsTr("从项目中移除"); onTriggered: { editor.removeProjectFile(assetList.currentIndex); assetList.currentIndex = -1 } }
                            }
                        }
                        ColumnLayout {
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                Label { text: qsTr("每行一条字幕"); color: Theme.secondaryText; Layout.fillWidth: true }
                                SubButton { text: qsTr("导入文稿…"); onClicked: window.openScriptDialog() }
                            }
                            Flickable {
                                id: scriptFlick
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.margins: 8
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                flickableDirection: Flickable.VerticalFlick
                                contentWidth: width
                                contentHeight: scriptEditor.implicitHeight
                                ScrollBar.vertical: SubScrollBar { }
                                ScriptTextView {
                                    id: scriptEditor
                                    objectName: "scriptEditor"
                                    width: scriptFlick.width
                                    height: Math.max(implicitHeight, scriptFlick.height)
                                    scrollView: scriptFlick
                                    text: editor.scriptText
                                    color: Theme.text
                                    selectionColor: Theme.selection
                                    selectedTextColor: Theme.text
                                    placeholderText: qsTr("粘贴文稿，或导入 TXT / SRT")
                                    placeholderTextColor: Theme.placeholder
                                    background: Rectangle { color: Theme.input; border.color: scriptEditor.activeFocus ? Theme.focusBorder : Theme.border }
                                    onTextChanged: if (text !== editor.scriptText) editor.scriptText = text
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: previewPanel
                color: Theme.previewBackground
                border.color: window.Window.window && window.belongsTo(window.Window.window.activeFocusItem, previewPanel) ? Theme.focusBorder : Theme.border
                SplitView.fillWidth: true
                SplitView.minimumWidth: Math.min(360, window.width * 0.35)
                clip: true

                Rectangle {
                    id: monitorHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 34
                    color: Theme.panelHeader
                    Label {
                        anchors.fill: parent
                        anchors.margins: 8
                        text: qsTr("节目监视器") + (editor.hasMedia ? " · " + editor.mediaName : "")
                        color: Theme.text
                        elide: Text.ElideMiddle
                    }
                }
                Item {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: monitorHeader.bottom
                    anchors.bottom: transport.top
                    clip: true
                    TapHandler { onTapped: if (window.commitPendingCue()) preview.forceActiveFocus(Qt.MouseFocusReason) }
                    SubButton {
                        anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 8
                        visible: editor.currentSubtitleItems.length > 1
                        text: qsTr("同时显示 %1 条 · 查看").arg(editor.currentSubtitleItems.length)
                        onClicked: activeSubtitlesDialog.open()
                    }

                VideoPreviewItem {
                    id: preview
                    objectName: "videoPreview"
                    anchors.fill: parent
                    visible: editor.hasVideo && editor.hasMedia
                }
                Label {
                    visible: !editor.hasMedia
                    anchors.centerIn: parent
                    text: editor.mediaPath !== "" ? qsTr("素材离线\n编辑结果已保留；请从文件菜单重新定位素材") : qsTr("尚未关联媒体\n支持拖入视频或音频")
                    color: Theme.previewEmptyText
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    lineHeight: 1.6
                }
                Label {
                    visible: editor.hasMedia && !editor.hasVideo
                    anchors.centerIn: parent
                    text: qsTr("音频素材")
                    color: Theme.previewEmptyText
                    font.pixelSize: 22
                }
                Text {
                    id: previewSubtitle
                    visible: editor.currentSubtitleText.length > 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 30
                    anchors.rightMargin: 30
                    anchors.bottomMargin: Math.max(8, editor.subtitleBottomMargin * parent.height / 1080)
                    text: editor.currentSubtitleText
                    color: editor.subtitleFontColor
                    style: Text.Outline
                    styleColor: editor.subtitleOutlineColor
                    font.family: editor.subtitleFontFamily
                    font.pixelSize: Math.max(10, editor.subtitleFontSize * parent.height / 1080)
                    font.bold: true
                    horizontalAlignment: editor.subtitleAlignment === "bottom-left" ? Text.AlignLeft : (editor.subtitleAlignment === "bottom-right" ? Text.AlignRight : Text.AlignHCenter)
                    wrapMode: Text.Wrap
                }
                }
                Rectangle {
                    id: transport
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 64
                    color: Theme.panel
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 0
                        Slider {
                            id: monitorSeek
                            objectName: "monitorSeek"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 22
                            enabled: editor.hasMedia
                            from: 0
                            to: Math.max(1, editor.durationMs)
                            value: editor.positionMs
                            onPressedChanged: if (pressed) editor.stop()
                            onMoved: editor.seek(value)
                            background: Rectangle {
                                x: monitorSeek.leftPadding
                                y: monitorSeek.topPadding + monitorSeek.availableHeight / 2 - height / 2
                                width: monitorSeek.availableWidth
                                height: 5
                                radius: 2.5
                                color: Theme.background
                                border.color: Theme.border
                                Rectangle {
                                    width: monitorSeek.visualPosition * parent.width
                                    height: parent.height
                                    radius: parent.radius
                                    color: Theme.border
                                }
                            }
                            handle: Rectangle {
                                x: monitorSeek.leftPadding + monitorSeek.visualPosition * (monitorSeek.availableWidth - width)
                                y: monitorSeek.topPadding + monitorSeek.availableHeight / 2 - height / 2
                                width: 10
                                height: 10
                                radius: 5
                                color: Theme.panel
                                border.width: 2
                                border.color: monitorSeek.pressed ? Theme.text : Theme.muted
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: editor.formatTime(editor.positionMs); color: Theme.accent; font.family: "Consolas"; font.pixelSize: 11 }
                            Item { Layout.fillWidth: true }
                            SubToolButton { objectName: "stepBack"; text: qsTr("上一帧"); shortcutHint: "Left"; icon.source: "icons/step-back.svg"; enabled: editor.hasMedia; onClicked: editor.stepFrames(-1) }
                            SubToolButton { objectName: "playPause"; text: editor.playing ? qsTr("暂停") : qsTr("播放"); shortcutHint: "Space"; icon.source: editor.playing ? "icons/pause.svg" : "icons/play.svg"; enabled: editor.hasMedia; onClicked: editor.togglePlay() }
                            SubToolButton { objectName: "stepForward"; text: qsTr("下一帧"); shortcutHint: "Right"; icon.source: "icons/step-forward.svg"; enabled: editor.hasMedia; onClicked: editor.stepFrames(1) }
                            SubComboBox {
                                id: subtitlePlaybackRate
                                objectName: "playbackRateCombo"
                                Layout.preferredWidth: 76
                                enabled: editor.hasMedia && !editor.playing
                                model: ["0.5×", "0.75×", "1.0×", "1.25×", "1.5×", "1.75×", "2.0×", "2.5×", "3.0×"]
                                currentIndex: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0].indexOf(editor.playbackRate)
                                onActivated: editor.setPlaybackRate(parseFloat(currentText))
                            }
                            Item { Layout.fillWidth: true }

                        }
                    }
                }
            }

            Rectangle {
                id: subtitlePanel
                color: Theme.panel
                border.color: window.Window.window && window.belongsTo(window.Window.window.activeFocusItem, subtitlePanel) ? Theme.focusBorder : Theme.border
                SplitView.preferredWidth: Math.max(320, workspaceSplit.width * Math.max(0.25, Math.min(0.45, editor.sessionState.subtitleRatio || 0.32)))
                SplitView.minimumWidth: Math.min(320, window.width * 0.35)
                SplitView.maximumWidth: 540
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 6; spacing: 6
                        Label { text: qsTr("字幕"); color: Theme.text; font.bold: true }
                        Label { text: editor.filteredSubtitleModel.count + " / " + editor.subtitleModel.count; color: Theme.secondaryText }
                        Item { Layout.fillWidth: true }
                        SubComboBox {
                            id: cueFilter; Layout.preferredWidth: 112
                            model: [qsTr("全部"), qsTr("待确认"), qsTr("未定位"), qsTr("重叠")]
                            currentIndex: ["ALL", "REVIEW", "UNTIMED", "OVERLAP"].indexOf(editor.filteredSubtitleModel.statusFilter)
                            onActivated: function(index) {
                                if (window.commitPendingCue()) editor.filteredSubtitleModel.statusFilter = ["ALL", "REVIEW", "UNTIMED", "OVERLAP"][index]
                                else currentIndex = Qt.binding(function() { return ["ALL", "REVIEW", "UNTIMED", "OVERLAP"].indexOf(editor.filteredSubtitleModel.statusFilter) })
                            }
                        }
                    }
                    SubTextField {
                        id: cueSearch; objectName: "subtitleSearch"
                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.bottomMargin: 6
                        placeholderText: qsTr("搜索字幕文字")
                        onTextChanged: subtitleSearchTimer.restart()
                    }
                    Timer { id: subtitleSearchTimer; interval: 150; onTriggered: if (window.commitPendingCue()) editor.filteredSubtitleModel.searchText = cueSearch.text; else cueSearch.text = editor.filteredSubtitleModel.searchText }
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; spacing: 4
                        SubButton { text: qsTr("编辑字幕"); shortcutHint: "Enter"; enabled: !!editor.selectedCueId && !editor.busy; onClicked: editor.createOrEditCue() }
                        SubButton { text: qsTr("确认并下一条"); shortcutHint: "Ctrl+Enter"; enabled: !!editor.selectedCueId && !editor.busy; onClicked: editor.confirmCurrentCue() }
                        Item { Layout.fillWidth: true }
                    }
                    ListView {
                        id: cueList; objectName: "subtitleList"
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 40
                        model: editor.filteredSubtitleModel; clip: true
                        currentIndex: { editor.selectedCueId; editor.filteredSubtitleModel.count; editor.filteredSubtitleModel.statusFilter; editor.filteredSubtitleModel.searchText; return editor.filteredSubtitleModel.rowForId(editor.selectedCueId) }
                        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                        ScrollBar.vertical: SubScrollBar { }
                        delegate: Rectangle {
                            id: rowItem
                            required property int index
                            required property var model
                            readonly property string cueId: model.id
                            readonly property bool selected: editor.selectedCueId === cueId
                            readonly property bool pending: model.status === "REVIEW" || model.status === "LOW_CONFIDENCE"
                            readonly property bool overlapping: { editor.overlappingCueCount; return editor.cueOverlaps(cueId) }
                            width: ListView.view.width; height: Math.max(74, rowText.implicitHeight + 26)
                            objectName: "subtitleRow_" + (model.sourceIndex - 1)
                            color: selected ? Theme.selection : rowMouse.containsMouse ? Theme.listHover : index % 2 ? Theme.panel : Theme.panelSecondary
                            border.color: selected ? Theme.focusBorder : "transparent"
                            Rectangle { width: 3; height: parent.height - 12; y: 6; visible: rowItem.overlapping; color: Theme.warning }
                            Label {
                                x: 8; y: 12; width: 22
                                text: rowItem.model.sourceIndex; color: Theme.muted; font.pixelSize: 12
                            }
                            Column {
                                x: 36; y: 12; width: 100; spacing: 5
                                Label { text: rowItem.model.timed ? editor.formatTime(rowItem.model.startMs) : qsTr("未定位"); color: Theme.secondaryText; font.family: "Consolas"; font.pixelSize: 12 }
                                Label { text: rowItem.model.timed ? editor.formatTime(rowItem.model.endMs) : ""; color: Theme.secondaryText; font.family: "Consolas"; font.pixelSize: 12 }
                            }
                            Column {
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.leftMargin: 144; anchors.rightMargin: 12 + Theme.scrollBarSize; anchors.topMargin: 10; spacing: 5
                                Label { id: rowText; width: parent.width; text: rowItem.model.text || qsTr("（空字幕）"); color: Theme.text; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight }
                                Label { width: parent.width; text: [rowItem.pending ? qsTr("待确认") : "", rowItem.overlapping ? qsTr("重叠") : ""].filter(function(value) { return value !== "" }).join(" · "); visible: text !== ""; color: Theme.warning; font.pixelSize: 12; elide: Text.ElideRight }
                            }
                            SubButton {
                                id: rowAction; objectName: "confirmCueButton"
                                anchors.right: parent.right; anchors.rightMargin: Theme.scrollBarSize + 4; anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                                width: visible ? 54 : 0; leftPadding: 4; rightPadding: 4
                                visible: rowItem.selected && (!rowItem.model.timed || rowItem.pending)
                                enabled: !editor.busy
                                text: rowItem.model.timed ? qsTr("确认") : qsTr("定位")
                                onClicked: window.runCommand(function() {
                                    editor.selectCue(editor.cueRowForId(rowItem.cueId), false)
                                    if (rowItem.model.timed) editor.confirmCue(editor.cueRowForId(rowItem.cueId))
                                    else { window.loadInspector(rowItem.cueId, rowItem.model.text, editor.positionUs, Math.min(editor.durationUs, editor.positionUs + 2000000)); cueEditDialog.open() }
                                })
                            }
                            MouseArea {
                                id: rowMouse; objectName: "subtitleRowMouse"
                                anchors.fill: parent; anchors.rightMargin: rowAction.visible ? 70 : Theme.scrollBarSize
                                hoverEnabled: true
                                onClicked: if (window.selectById(rowItem.cueId, true)) cueList.forceActiveFocus(Qt.MouseFocusReason)
                                onDoubleClicked: if (window.selectById(rowItem.cueId, false)) editor.createOrEditCue()
                            }
                            SubToolTip {
                                visible: rowMouse.containsMouse
                                text: rowItem.model.text + "\n" + (rowItem.model.timed ? editor.formatTime(rowItem.model.startMs) + " → " + editor.formatTime(rowItem.model.endMs) : qsTr("未定位"))
                                    + (rowItem.model.candidateText ? "\n" + qsTr("识别候选：") + rowItem.model.candidateText : "")
                            }
                        }
                        Label { anchors.centerIn: parent; visible: cueList.count === 0; text: qsTr("没有符合条件的字幕"); color: Theme.muted }
                    }
                }
            }
        }

        Item {
            SplitView.preferredHeight: verticalSplit.height * 0.45
            SplitView.minimumHeight: Math.min(230, verticalSplit.height * 0.4)
            Rectangle {
                id: timelineTools
                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                height: 36; color: Theme.panelHeader
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 4
                    Label { text: qsTr("字幕时间轴"); font.bold: true; color: Theme.text }
                    SubButton { text: qsTr("吸附"); checkable: true; checked: editor.snapEnabled; shortcutHint: "S"; onClicked: editor.toggleSnap(); leftPadding: 8; rightPadding: 8 }
                    SubButton { text: qsTr("切分"); shortcutHint: "C"; enabled: !editor.busy; leftPadding: 8; rightPadding: 8; onClicked: window.runCommand(function() { editor.splitCurrentCue() }) }
                    SubButton { text: qsTr("合并"); shortcutHint: "T"; enabled: !editor.busy; leftPadding: 8; rightPadding: 8; onClicked: window.runCommand(function() { editor.joinAroundPlayhead() }) }
                    Label { text: editor.overlapCount > 0 ? qsTr("%1 处重叠").arg(editor.overlapCount) : ""; color: Theme.warning }
                    SubToolButton { visible: editor.overlapCount > 0; text: qsTr("上一处重叠"); icon.source: "icons/step-back.svg"; onClicked: window.runCommand(function() { editor.navigateOverlap(-1) }) }
                    SubToolButton { visible: editor.overlapCount > 0; text: qsTr("下一处重叠"); icon.source: "icons/step-forward.svg"; onClicked: window.runCommand(function() { editor.navigateOverlap(1) }) }
                    Item { Layout.fillWidth: true }
                    SubToolButton { text: qsTr("缩小时间轴"); shortcutHint: "-"; icon.source: "icons/minus.svg"; onClicked: editor.adjustZoomPercent(-10, timelineScene.width) }
                    Label { objectName: "transportZoom"; text: editor.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 12; Layout.minimumWidth: 40 }
                    SubToolButton { text: qsTr("放大时间轴"); shortcutHint: "="; icon.source: "icons/plus.svg"; onClicked: editor.adjustZoomPercent(10, timelineScene.width) }
                    SubButton { text: qsTr("适配"); shortcutHint: "\\"; leftPadding: 8; rightPadding: 8; onClicked: editor.fitTimeline(timelineScene.width) }
                }
            }
            TimelineSceneItem {
                id: timelineScene
                anchors.top: timelineTools.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: timelineZoomBar.top
                objectName: "timelineScene"
                editable: !editor.busy
                interactionGuard: function() { return window.commitPendingCue() }
                followDirection: editor.playing ? editor.direction : 0
                viewportInteracting: timelineZoomBar.interacting
                durationUs: editor.durationUs > 0 ? editor.durationUs : 60000000
                playheadUs: editor.positionUs
                inPointUs: editor.inPointUs
                outPointUs: editor.outPointUs
            }
            Rectangle {
                anchors.left: timelineScene.left; anchors.top: timelineScene.top; anchors.margins: 32
                visible: Object.keys(timelineScene.dragPreview).length > 0
                width: Math.min(parent.width - 64, dragFeedback.implicitWidth + 20); height: dragFeedback.implicitHeight + 16
                color: Theme.panelRaised; border.color: Theme.accent; radius: 3; z: 5
                Label {
                    id: dragFeedback; anchors.fill: parent; anchors.margins: 8; color: Theme.text; wrapMode: Text.Wrap
                    text: {
                        const cue = timelineScene.dragPreview
                        return editor.formatTime((cue.startUs || 0) / 1000) + " → " + editor.formatTime((cue.endUs || 0) / 1000)
                            + " · Δ " + ((cue.deltaUs || 0) / 1000000).toFixed(3) + "s"
                            + (editor.snapEnabled && cue.target ? " · " + qsTr("贴合") + cue.target : "")
                            + (cue.overlap ? qsTr(" · 存在重叠") : "") + qsTr(" · Esc 取消")
                    }
                }
            }
            SubToolTip {
                visible: Object.keys(timelineScene.hoveredCue).length > 0
                text: {
                    const cue = timelineScene.hoveredCue
                    return (cue.text || "") + "\n" + editor.formatTime((cue.startUs || 0) / 1000) + " → " + editor.formatTime((cue.endUs || 0) / 1000)
                        + " · " + (((cue.endUs || 0) - (cue.startUs || 0)) / 1000000).toFixed(3) + "s"
                        + (cue.review ? qsTr(" · 待确认") : "") + (cue.overlap ? qsTr(" · 重叠") : "")
                }
            }
            SubScrollBar {
                anchors.right: timelineScene.right; y: timelineScene.y + 28
                height: timelineScene.subtitleViewportHeight
                orientation: Qt.Vertical
                visible: timelineScene.subtitleContentHeight > timelineScene.subtitleViewportHeight
                size: Math.min(1, timelineScene.subtitleViewportHeight / timelineScene.subtitleContentHeight)
                position: timelineScene.subtitleScrollOffset / timelineScene.subtitleContentHeight
                onPositionChanged: if (pressed) timelineScene.subtitleScrollOffset = position * timelineScene.subtitleContentHeight
            }
            TimelineZoomBar {
                id: timelineZoomBar
                objectName: "timelineZoomBar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                timeline: timelineScene
            }
        }
    }

    Rectangle {
        id: statusBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 24
        color: Theme.panelRaised
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.divider
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            Label { text: editor.statusText; color: Theme.muted; elide: Text.ElideRight; Layout.fillWidth: true }
            Label { text: editor.hasMedia ? editor.mediaName : ""; color: Theme.muted; elide: Text.ElideMiddle; Layout.maximumWidth: 260 }
        }
    }



    Dialog {
        id: activeSubtitlesDialog; title: qsTr("当前同时出现的字幕"); modal: true; anchors.centerIn: parent
        width: Math.min(560, window.width - 32); height: Math.min(480, window.height - 32); standardButtons: Dialog.Close
        ListView { anchors.fill: parent; clip: true; model: editor.currentSubtitleItems; ScrollBar.vertical: SubScrollBar { }
            delegate: Label { required property var modelData; width: ListView.view.width; text: modelData.text; color: Theme.text; wrapMode: Text.Wrap; padding: 8 }
        }
    }
    Rectangle {
        id: alignmentProgressDialog
        objectName: "alignmentProgressDialog"
        property double startedAt: 0
        property int elapsedSeconds: 0
        onVisibleChanged: if (visible) { startedAt = Date.now(); elapsedSeconds = 0 }
        Timer {
            interval: 1000
            repeat: true
            running: editor.busy
            onTriggered: parent.elapsedSeconds = Math.floor((Date.now() - parent.startedAt) / 1000)
        }
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: editor.busy ? (window.taskDetailsExpanded ? 66 : 40) : 0
        visible: editor.busy
        color: Theme.panelRaised
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8
            SubButton { text: window.taskDetailsExpanded ? qsTr("收起") : qsTr("详情"); leftPadding: 8; rightPadding: 8; onClicked: window.taskDetailsExpanded = !window.taskDetailsExpanded }
            Label { text: (editor.busyTaskTitle || qsTr("正在自动打轴")) + " · " + parent.parent.elapsedSeconds + "s"; color: Theme.text }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: editor.alignmentProgressText; color: Theme.secondaryText; visible: window.taskDetailsExpanded; Layout.fillWidth: true; elide: Text.ElideRight }
                ProgressBar {
                    id: alignmentProgressBar
                    objectName: "alignmentProgressBar"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 16
                    from: 0
                    to: 100
                    value: editor.alignmentProgress
                    indeterminate: editor.alignmentIndeterminate
                    background: Rectangle {
                        color: Theme.scrollTrack
                        border.color: Theme.border
                    }
                    contentItem: Item {
                        clip: true
                        Rectangle {
                            objectName: "alignmentProgressFill"
                            width: alignmentProgressBar.visualPosition * parent.width
                            height: parent.height
                            visible: !alignmentProgressBar.indeterminate
                            color: Theme.accent
                        }
                        Rectangle {
                            id: progressIndeterminate
                            width: Math.max(28, parent.width * 0.24)
                            height: parent.height
                            visible: alignmentProgressBar.indeterminate
                            color: Theme.accent
                            SequentialAnimation on x {
                                running: alignmentProgressBar.indeterminate && alignmentProgressDialog.visible
                                loops: Animation.Infinite
                                NumberAnimation {
                                    from: -progressIndeterminate.width
                                    to: progressIndeterminate.parent.width
                                    duration: 900
                                    easing.type: Easing.InOutQuad
                                }
                            }
                        }
                    }
                }
            }
            Label { text: editor.alignmentIndeterminate ? qsTr("处理中…") : editor.alignmentProgress + "%"; color: Theme.text }
            SubButton { text: qsTr("取消"); onClicked: editor.cancelAlignment() }
        }
    }
    Dialog {
        id: preflightDialog
        title: qsTr("自动打轴前检查")
        modal: true
        anchors.centerIn: parent
        width: Math.min(560, window.width - 48)
        height: Math.min(420, window.height - 48)
        padding: 16
        palette.window: Theme.panel
        palette.windowText: Theme.text
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.border
            radius: 2
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                text: qsTr("请先处理以下问题：")
                color: Theme.text
                font.bold: true
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: window.preflightIssues
                spacing: 6
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: issueColumn.implicitHeight + 16
                    color: Theme.panelSecondary
                    border.color: Theme.border
                    Column {
                        id: issueColumn
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 3
                        Label { text: "• " + modelData.title; color: Theme.text; font.bold: true }
                        Label {
                            visible: modelData.detail && modelData.detail.length > 0
                            text: modelData.detail || ""
                            color: Theme.muted
                            width: parent.width
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: Theme.panelRaised }
            SubButton {
                text: qsTr("打开设置")
                visible: window.preflightIssues.some(function(item) { return item.settingsSection !== "" })
                onClicked: {
                    preflightDialog.close()
                    window.openSettings()
                }
            }
            SubButton { text: qsTr("关闭"); onClicked: preflightDialog.close() }
        }
    }

    DropArea {
        id: fileDropArea
        objectName: "fileDropArea"
        anchors.fill: parent
        enabled: !editor.busy && !window.modalEditing
        onEntered: function(drag) { drag.accepted = drag.hasUrls && editor.canImportFiles(drag.urls) }
        onDropped: function(drop) {
            if (drop.hasUrls && editor.canImportFiles(drop.urls)) {
                const urls = Array.from(drop.urls)
                drop.acceptProposedAction()
                window.requestSubtitleLeave(function() {
                    editor.importFiles(urls)
                })
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            visible: fileDropArea.containsDrag
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
        }
    }


    Action { id: subtitleCommand0; text: qsTr("播放／暂停"); property string keySequence: "Space"; enabled: true; onTriggered: window.runCommand(function() { editor.togglePlay() } ) }
    Shortcut { sequence: subtitleCommand0.keySequence; enabled: subtitleCommand0.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand0.trigger() }
    Action { id: subtitleCommand1; text: qsTr("撤销"); property string keySequence: "Ctrl+Z"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.undo() } ) }
    Shortcut { sequence: subtitleCommand1.keySequence; enabled: subtitleCommand1.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand1.trigger() }
    Action { id: subtitleCommand2; text: qsTr("重做"); property string keySequence: "Ctrl+Y"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.redo() } ) }
    Shortcut { sequence: subtitleCommand2.keySequence; enabled: subtitleCommand2.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand2.trigger() }
    Action { id: subtitleCommand3; text: qsTr("删除当前字幕"); property string keySequence: "Delete"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.deleteCue() } ) }
    Shortcut { sequence: subtitleCommand3.keySequence; enabled: subtitleCommand3.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand3.trigger() }
    Action { id: subtitleCommand4; text: qsTr("反向播放"); property string keySequence: "J"; enabled: true; onTriggered: window.runCommand(function() { editor.playReverse() } ) }
    Shortcut { sequence: subtitleCommand4.keySequence; enabled: subtitleCommand4.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand4.trigger() }
    Action { id: subtitleCommand5; text: qsTr("停止播放"); property string keySequence: "K"; enabled: true; onTriggered: window.runCommand(function() { editor.stop() } ) }
    Shortcut { sequence: subtitleCommand5.keySequence; enabled: subtitleCommand5.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand5.trigger() }
    Action { id: subtitleCommand6; text: qsTr("正向播放"); property string keySequence: "L"; enabled: true; onTriggered: window.runCommand(function() { editor.playForward() } ) }
    Shortcut { sequence: subtitleCommand6.keySequence; enabled: subtitleCommand6.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand6.trigger() }
    Action { id: subtitleCommand7; text: qsTr("后退一帧"); property string keySequence: "Left"; enabled: true; onTriggered: window.runCommand(function() { editor.stepFrames(-1) } ) }
    Shortcut { sequence: subtitleCommand7.keySequence; enabled: subtitleCommand7.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand7.trigger() }
    Action { id: subtitleCommand8; text: qsTr("前进一帧"); property string keySequence: "Right"; enabled: true; onTriggered: window.runCommand(function() { editor.stepFrames(1) } ) }
    Shortcut { sequence: subtitleCommand8.keySequence; enabled: subtitleCommand8.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand8.trigger() }
    Action { id: subtitleCommand9; text: qsTr("后退五帧"); property string keySequence: "Shift+Left"; enabled: true; onTriggered: window.runCommand(function() { editor.stepFrames(-5) } ) }
    Shortcut { sequence: subtitleCommand9.keySequence; enabled: subtitleCommand9.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand9.trigger() }
    Action { id: subtitleCommand10; text: qsTr("前进五帧"); property string keySequence: "Shift+Right"; enabled: true; onTriggered: window.runCommand(function() { editor.stepFrames(5) } ) }
    Shortcut { sequence: subtitleCommand10.keySequence; enabled: subtitleCommand10.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand10.trigger() }
    Action { id: subtitleCommand11; text: qsTr("上一条结束到播放头"); property string keySequence: "Q"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setPreviousEnd() } ) }
    Shortcut { sequence: subtitleCommand11.keySequence; enabled: subtitleCommand11.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand11.trigger() }
    Action { id: subtitleCommand12; text: qsTr("下一条开始到播放头"); property string keySequence: "W"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setFollowingStart() } ) }
    Shortcut { sequence: subtitleCommand12.keySequence; enabled: subtitleCommand12.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand12.trigger() }
    Action { id: subtitleCommand13; text: qsTr("下一条开始到播放头"); property string keySequence: "3"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setFollowingStart() } ) }
    Shortcut { sequence: subtitleCommand13.keySequence; enabled: subtitleCommand13.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand13.trigger() }
    Action { id: subtitleCommand14; text: qsTr("上一条结束到播放头"); property string keySequence: "4"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setPreviousEnd() } ) }
    Shortcut { sequence: subtitleCommand14.keySequence; enabled: subtitleCommand14.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand14.trigger() }
    Action { id: subtitleCommand15; text: qsTr("合并播放头附近字幕"); property string keySequence: "T"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.joinAroundPlayhead() } ) }
    Shortcut { sequence: subtitleCommand15.keySequence; enabled: subtitleCommand15.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand15.trigger() }
    Action { id: subtitleCommand16; text: qsTr("设置当前开始"); property string keySequence: "["; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setCurrentStart() } ) }
    Shortcut { sequence: subtitleCommand16.keySequence; enabled: subtitleCommand16.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand16.trigger() }
    Action { id: subtitleCommand17; text: qsTr("设置当前结束"); property string keySequence: "]"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.setCurrentEnd() } ) }
    Shortcut { sequence: subtitleCommand17.keySequence; enabled: subtitleCommand17.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand17.trigger() }
    Action { id: subtitleCommand18; text: qsTr("切分当前字幕"); property string keySequence: "C"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.splitCurrentCue() } ) }
    Shortcut { sequence: subtitleCommand18.keySequence; enabled: subtitleCommand18.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand18.trigger() }
    Action { id: subtitleCommand19; text: qsTr("编辑／新建字幕"); property string keySequence: "Return"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.createOrEditCue() } ) }
    Shortcut { sequence: subtitleCommand19.keySequence; enabled: subtitleCommand19.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand19.trigger() }
    Shortcut { sequence: "Enter"; enabled: subtitleCommand19.enabled && window.visible && !window.textEditing && !window.modalEditing; onActivated: subtitleCommand19.trigger() }
    Action { id: subtitleCommand20; text: qsTr("定位下一条文稿"); property string keySequence: "Shift+Return"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.createNextScriptCue() } ) }
    Shortcut { sequence: subtitleCommand20.keySequence; enabled: subtitleCommand20.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand20.trigger() }
    Shortcut { sequence: "Shift+Enter"; enabled: subtitleCommand20.enabled && window.visible && !window.textEditing && !window.modalEditing; onActivated: subtitleCommand20.trigger() }
    Action { id: subtitleCommand21; text: qsTr("确认并下一待确认"); property string keySequence: "Ctrl+Return"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.confirmCurrentCue() } ) }
    Shortcut { sequence: subtitleCommand21.keySequence; enabled: subtitleCommand21.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand21.trigger() }
    Shortcut { sequence: "Ctrl+Enter"; enabled: subtitleCommand21.enabled && window.visible && !window.textEditing && !window.modalEditing; onActivated: subtitleCommand21.trigger() }
    Action { id: subtitleCommand22; text: qsTr("上一条字幕"); property string keySequence: "Up"; enabled: true; onTriggered: window.runCommand(function() { window.navigateFiltered(-1) } ) }
    Shortcut { sequence: subtitleCommand22.keySequence; enabled: subtitleCommand22.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand22.trigger() }
    Action { id: subtitleCommand23; text: qsTr("下一条字幕"); property string keySequence: "Down"; enabled: true; onTriggered: window.runCommand(function() { window.navigateFiltered(1) } ) }
    Shortcut { sequence: subtitleCommand23.keySequence; enabled: subtitleCommand23.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand23.trigger() }
    Action { id: subtitleCommand24; text: qsTr("上一条字幕"); property string keySequence: "Shift+Tab"; enabled: true; onTriggered: window.runCommand(function() { window.navigateFiltered(-1) } ) }
    Shortcut { sequence: subtitleCommand24.keySequence; enabled: subtitleCommand24.enabled && (window.visible && (cueList.activeFocus || timelineScene.activeFocus) && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand24.trigger() }
    Action { id: subtitleCommand25; text: qsTr("下一条字幕"); property string keySequence: "Tab"; enabled: true; onTriggered: window.runCommand(function() { window.navigateFiltered(1) } ) }
    Shortcut { sequence: subtitleCommand25.keySequence; enabled: subtitleCommand25.enabled && (window.visible && (cueList.activeFocus || timelineScene.activeFocus) && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand25.trigger() }
    Action { id: subtitleCommand26; text: qsTr("上一条待确认"); property string keySequence: "Shift+Up"; enabled: true; onTriggered: window.runCommand(function() { editor.navigatePendingCue(-1) } ) }
    Shortcut { sequence: subtitleCommand26.keySequence; enabled: subtitleCommand26.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand26.trigger() }
    Action { id: subtitleCommand27; text: qsTr("下一条待确认"); property string keySequence: "Shift+Down"; enabled: true; onTriggered: window.runCommand(function() { editor.navigatePendingCue(1) } ) }
    Shortcut { sequence: subtitleCommand27.keySequence; enabled: subtitleCommand27.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand27.trigger() }
    Action { id: subtitleCommand28; text: qsTr("上一条不匹配"); property string keySequence: "Ctrl+Up"; enabled: true; onTriggered: window.runCommand(function() { editor.navigateMismatchCue(-1) } ) }
    Shortcut { sequence: subtitleCommand28.keySequence; enabled: subtitleCommand28.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand28.trigger() }
    Action { id: subtitleCommand29; text: qsTr("下一条不匹配"); property string keySequence: "Ctrl+Down"; enabled: true; onTriggered: window.runCommand(function() { editor.navigateMismatchCue(1) } ) }
    Shortcut { sequence: subtitleCommand29.keySequence; enabled: subtitleCommand29.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand29.trigger() }
    Action { id: subtitleCommand30; text: qsTr("设置入点"); property string keySequence: "I"; enabled: true; onTriggered: window.runCommand(function() { editor.setInPoint() } ) }
    Shortcut { sequence: subtitleCommand30.keySequence; enabled: subtitleCommand30.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand30.trigger() }
    Action { id: subtitleCommand31; text: qsTr("设置出点"); property string keySequence: "O"; enabled: true; onTriggered: window.runCommand(function() { editor.setOutPoint() } ) }
    Shortcut { sequence: subtitleCommand31.keySequence; enabled: subtitleCommand31.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand31.trigger() }
    Action { id: subtitleCommand32; text: qsTr("定位当前开始"); property string keySequence: "Shift+I"; enabled: true; onTriggered: window.runCommand(function() { editor.seekToCurrentStart() } ) }
    Shortcut { sequence: subtitleCommand32.keySequence; enabled: subtitleCommand32.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand32.trigger() }
    Action { id: subtitleCommand33; text: qsTr("定位当前结束"); property string keySequence: "Shift+O"; enabled: true; onTriggered: window.runCommand(function() { editor.seekToCurrentEnd() } ) }
    Shortcut { sequence: subtitleCommand33.keySequence; enabled: subtitleCommand33.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand33.trigger() }
    Action { id: subtitleCommand34; text: qsTr("定位时间轴开始"); property string keySequence: "Home"; enabled: true; onTriggered: window.runCommand(function() { editor.seekToTimelineStart() } ) }
    Shortcut { sequence: subtitleCommand34.keySequence; enabled: subtitleCommand34.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand34.trigger() }
    Action { id: subtitleCommand35; text: qsTr("定位时间轴结束"); property string keySequence: "End"; enabled: true; onTriggered: window.runCommand(function() { editor.seekToTimelineEnd() } ) }
    Shortcut { sequence: subtitleCommand35.keySequence; enabled: subtitleCommand35.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand35.trigger() }
    Action { id: subtitleCommand36; text: qsTr("微调开始向左"); property string keySequence: "Alt+Left"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentStart(-1) } ) }
    Shortcut { sequence: subtitleCommand36.keySequence; enabled: subtitleCommand36.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand36.trigger() }
    Action { id: subtitleCommand37; text: qsTr("微调开始向右"); property string keySequence: "Alt+Right"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentStart(1) } ) }
    Shortcut { sequence: subtitleCommand37.keySequence; enabled: subtitleCommand37.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand37.trigger() }
    Action { id: subtitleCommand38; text: qsTr("微调结束向左"); property string keySequence: "Alt+Shift+Left"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentEnd(-1) } ) }
    Shortcut { sequence: subtitleCommand38.keySequence; enabled: subtitleCommand38.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand38.trigger() }
    Action { id: subtitleCommand39; text: qsTr("微调结束向右"); property string keySequence: "Alt+Shift+Right"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentEnd(1) } ) }
    Shortcut { sequence: subtitleCommand39.keySequence; enabled: subtitleCommand39.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand39.trigger() }
    Action { id: subtitleCommand40; text: qsTr("微调开始向左五帧"); property string keySequence: "Ctrl+Alt+Left"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentStart(-5) } ) }
    Shortcut { sequence: subtitleCommand40.keySequence; enabled: subtitleCommand40.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand40.trigger() }
    Action { id: subtitleCommand41; text: qsTr("微调开始向右五帧"); property string keySequence: "Ctrl+Alt+Right"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentStart(5) } ) }
    Shortcut { sequence: subtitleCommand41.keySequence; enabled: subtitleCommand41.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand41.trigger() }
    Action { id: subtitleCommand42; text: qsTr("微调结束向左五帧"); property string keySequence: "Ctrl+Alt+Shift+Left"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentEnd(-5) } ) }
    Shortcut { sequence: subtitleCommand42.keySequence; enabled: subtitleCommand42.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand42.trigger() }
    Action { id: subtitleCommand43; text: qsTr("微调结束向右五帧"); property string keySequence: "Ctrl+Alt+Shift+Right"; enabled: !editor.busy; onTriggered: window.runCommand(function() { editor.nudgeCurrentEnd(5) } ) }
    Shortcut { sequence: subtitleCommand43.keySequence; enabled: subtitleCommand43.enabled && (window.visible && !window.textEditing && !window.modalEditing && !editor.busy); onActivated: subtitleCommand43.trigger() }
    Action { id: subtitleCommand44; text: qsTr("清除入点"); property string keySequence: "Alt+I"; enabled: true; onTriggered: window.runCommand(function() { editor.clearInPoint() } ) }
    Shortcut { sequence: subtitleCommand44.keySequence; enabled: subtitleCommand44.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand44.trigger() }
    Action { id: subtitleCommand45; text: qsTr("清除出点"); property string keySequence: "Alt+O"; enabled: true; onTriggered: window.runCommand(function() { editor.clearOutPoint() } ) }
    Shortcut { sequence: subtitleCommand45.keySequence; enabled: subtitleCommand45.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand45.trigger() }
    Action { id: subtitleCommand46; text: qsTr("切换吸附"); property string keySequence: "S"; enabled: true; onTriggered: window.runCommand(function() { editor.toggleSnap() } ) }
    Shortcut { sequence: subtitleCommand46.keySequence; enabled: subtitleCommand46.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand46.trigger() }
    Action { id: subtitleCommand47; text: qsTr("放大当前时间轴"); property string keySequence: "="; enabled: true; onTriggered: window.runCommand(function() { editor.adjustZoomPercent(10, timelineScene.width) } ) }
    Shortcut { sequence: subtitleCommand47.keySequence; enabled: subtitleCommand47.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand47.trigger() }
    Action { id: subtitleCommand48; text: qsTr("缩小当前时间轴"); property string keySequence: "-"; enabled: true; onTriggered: window.runCommand(function() { editor.adjustZoomPercent(-10, timelineScene.width) } ) }
    Shortcut { sequence: subtitleCommand48.keySequence; enabled: subtitleCommand48.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand48.trigger() }
    Action { id: subtitleCommand49; text: qsTr("适配当前时间轴"); property string keySequence: "\\"; enabled: true; onTriggered: window.runCommand(function() { editor.fitTimeline(timelineScene.width) } ) }
    Shortcut { sequence: subtitleCommand49.keySequence; enabled: subtitleCommand49.enabled && (window.visible && !window.textEditing && !window.modalEditing); onActivated: subtitleCommand49.trigger() }
}
