import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Item {
    id: window
    objectName: "subtitleWorkspace"

    property bool textEditing: scriptEditor.activeFocus || cueTextEditor.activeFocus || assetSearch.activeFocus
                               || cueEditor.visible || mediaDialog.visible || scriptDialog.visible
                               || (settingsWindow && settingsWindow.visible)

    Component.onCompleted: {
        editor.setPreviewItem(preview)
        editor.setTimelineItem(timelineScene)
    }

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
        let pending = 0
        let unlocated = 0
        for (let i = 0; i < editor.subtitleModel.count; ++i) {
            const cue = editor.subtitleModel.get(i)
            if (!cue.timed) ++unlocated
            else if (cue.status === "LOW_CONFIDENCE") ++pending
        }
        if (pending || unlocated) {
            exportNotice.text = "待确认 " + pending + " 条，将保留导出；未定位 " + unlocated + " 条，不导出。"
            exportNotice.open()
        } else editor.exportSubtitles()
    }
    Dialog {
        id: exportNotice
        property string text: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("导出字幕")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: exportNotice.text; color: Theme.text }
        onAccepted: editor.exportSubtitles()
    }

    Connections {
        target: editor
        function onAlignmentPreflightFailed(issues) {
            window.preflightIssues = issues
            preflightDialog.open()
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
        nameFilters: [qsTr("支持的素材 (*.wav *.mp3 *.m4a *.flac *.aac *.mp4 *.mov *.mkv *.webm *.avi *.m4v *.wmv *.mpg *.mpeg *.mts *.m2ts *.ts *.mxf *.ogg *.opus *.wma *.aif *.aiff *.txt *.srt)"), qsTr("所有文件 (*)")]
        onAccepted: editor.importFiles(selectedFiles)
    }
    FileDialog {
        id: scriptDialog
        title: qsTr("导入字幕文稿")
        parentWindow: window.Window.window
        nameFilters: [qsTr("字幕文稿 (*.txt *.srt)"), qsTr("所有文件 (*)")]
        onAccepted: editor.importScript(selectedFile)
    }
    function openMediaDialog() { mediaDialog.open() }
    function openScriptDialog() { scriptDialog.open() }

    Connections {
        target: editor
        function onScriptTextChanged() { sourceTabs.currentIndex = 1 }
        function onEditCueRequested(row, text) {
            cueEditorRow.text = String(row)
            cueTextEditor.text = text
            cueEditor.open()
            cueTextEditor.forceActiveFocus()
            cueTextEditor.selectAll()
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
                    MenuItem { text: qsTr("导入素材…    Ctrl+I"); onTriggered: window.openMediaDialog() }
                    MenuItem { text: qsTr("导入字幕文稿…"); onTriggered: window.openScriptDialog() }
                    MenuSeparator { }
                    MenuItem { text: qsTr("导出字幕"); enabled: editor.canExport && !editor.busy; onTriggered: window.requestExport() }
                    MenuSeparator { }
                    MenuItem { text: qsTr("退出"); onTriggered: window.Window.window.close() }
                }
            }
            MenuButton { text: qsTr("导入…"); onClicked: window.openMediaDialog() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("自动打轴"); enabled: !editor.busy; onClicked: editor.startAlignment() }
            MenuButton { text: qsTr("取消"); enabled: editor.busy; onClicked: editor.cancelAlignment() }
            MenuButton { text: qsTr("导出"); enabled: editor.canExport && !editor.busy; onClicked: window.requestExport() }
            Rectangle { width: 1; height: 24; color: Theme.divider }
            MenuButton { text: qsTr("设置"); onClicked: window.openSettings() }
            MenuButton { text: qsTr("关于"); onClicked: window.openAbout() }
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
            SplitView.preferredHeight: verticalSplit.height * 0.62
            SplitView.minimumHeight: 300
            handle: Rectangle {
                implicitWidth: 5
                implicitHeight: 5
                color: Theme.divider
            }

            Rectangle {
                color: Theme.panel
                border.color: Theme.border
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 240
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
                        MenuButton { text: qsTr("项目：素材"); onClicked: sourceTabs.currentIndex = 0; highlighted: sourceTabs.currentIndex === 0 }
                        MenuButton { text: qsTr("文稿"); onClicked: sourceTabs.currentIndex = 1; highlighted: sourceTabs.currentIndex === 1 }
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
                                    onDoubleClicked: editor.openProjectFile(index)
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
                                SubButton { text: qsTr("打开"); enabled: assetList.currentIndex >= 0; onClicked: editor.openProjectFile(assetList.currentIndex) }
                                Item { Layout.fillWidth: true }
                                SubButton { text: qsTr("移除"); enabled: assetList.currentIndex >= 0; onClicked: { editor.removeProjectFile(assetList.currentIndex); assetList.currentIndex = -1 } }
                            }
                            SubMenu {
                                id: assetMenu
                                MenuItem { text: qsTr("打开素材"); onTriggered: editor.openProjectFile(assetList.currentIndex) }
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
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.margins: 8
                                clip: true
                                contentWidth: availableWidth
                                TextArea {
                                    id: scriptEditor
                                    objectName: "scriptEditor"
                                    text: editor.scriptText
                                    color: Theme.text
                                    selectionColor: Theme.selection
                                    selectedTextColor: Theme.text
                                    placeholderText: qsTr("粘贴文稿，或导入 TXT / SRT")
                                    placeholderTextColor: Theme.placeholder
                                    wrapMode: TextEdit.Wrap
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
                border.color: Theme.border
                SplitView.fillWidth: true
                SplitView.minimumWidth: 360
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

                VideoPreviewItem {
                    id: preview
                    objectName: "videoPreview"
                    anchors.fill: parent
                    visible: editor.hasVideo
                }
                Label {
                    visible: !editor.hasMedia
                    anchors.centerIn: parent
                    text: qsTr("导入媒体开始编辑\n支持拖入视频或音频")
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
                            SubToolButton { objectName: "stepBack"; text: qsTr("上一帧"); icon.source: "icons/step-back.svg"; enabled: editor.hasMedia; onClicked: editor.stepFrames(-1) }
                            SubToolButton { objectName: "playPause"; text: editor.playing ? qsTr("暂停") : qsTr("播放"); icon.source: editor.playing ? "icons/pause.svg" : "icons/play.svg"; enabled: editor.hasMedia; onClicked: editor.togglePlay() }
                            SubToolButton { objectName: "stepForward"; text: qsTr("下一帧"); icon.source: "icons/step-forward.svg"; enabled: editor.hasMedia; onClicked: editor.stepFrames(1) }
                            SubComboBox {
                                objectName: "playbackRateCombo"
                                Layout.preferredWidth: 76
                                enabled: editor.hasMedia && !editor.playing
                                model: ["0.5×", "0.75×", "1.0×", "1.25×", "1.5×", "1.75×", "2.0×", "2.5×", "3.0×"]
                                currentIndex: 2
                                onActivated: editor.setPlaybackRate(parseFloat(currentText))
                            }
                            Item { Layout.fillWidth: true }
                            SubToolButton { text: qsTr("缩小时间轴"); icon.source: "icons/minus.svg"; enabled: editor.hasMedia; onClicked: editor.adjustZoomPercent(-10, timelineScene.width) }
                            Label { objectName: "transportZoom"; text: editor.zoomPercent + "%"; color: Theme.secondaryText; font.pixelSize: 11; Layout.minimumWidth: 34; horizontalAlignment: Text.AlignHCenter }
                            SubToolButton { text: qsTr("放大时间轴"); icon.source: "icons/plus.svg"; enabled: editor.hasMedia; onClicked: editor.adjustZoomPercent(10, timelineScene.width) }
                        }
                    }
                }
            }

            Rectangle {
                color: Theme.panel
                border.color: Theme.border
                SplitView.preferredWidth: 380
                SplitView.minimumWidth: 320
                SplitView.maximumWidth: 480

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        height: 34
                        color: Theme.panelHeader
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            Label { text: qsTr("字幕列表"); color: Theme.text; font.bold: true }
                            Item { Layout.fillWidth: true }
                            Label { text: editor.subtitleModel.count + qsTr(" 条"); color: Theme.muted }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 28
                        color: Theme.input
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            Label { text: "#"; color: Theme.muted; Layout.preferredWidth: 28 }
                            Label { text: qsTr("文本"); color: Theme.muted; Layout.fillWidth: true }
                            Label { text: qsTr("时间 / 状态"); color: Theme.muted; Layout.preferredWidth: 125 }
                        }
                    }
                    ListView {
                        id: cueList
                        objectName: "subtitleList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: editor.subtitleModel
                        currentIndex: editor.selectedCue
                        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                        ScrollBar.vertical: SubScrollBar { }

                        delegate: Rectangle {
                            id: rowItem
                            objectName: "subtitleRow_" + index
                            required property int index
                            required property int sourceIndex
                            required property string text
                            required property int startMs
                            required property int endMs
                            required property bool timed
                            required property real confidence
                            required property string status
                            required property string candidateText
                            required property string skipReason
                            width: ListView.view.width
                            height: 48
                            color: editor.selectedCue === index ? Theme.selection
                                 : (rowMouse.containsMouse ? Theme.listHover
                                 : (index % 2 ? Theme.panel : Theme.panelSecondary))
                            border.color: editor.selectedCue === index ? Theme.focusBorder : "transparent"
                            border.width: editor.selectedCue === index ? 1 : 0

                            SubToolTip {
                                visible: rowMouse.containsMouse && (rowItem.status === "LOW_CONFIDENCE" || !rowItem.timed)
                                text: "对齐置信度：" + Math.round(rowItem.confidence * 100) + "% · ASR置信度未知\n"
                                    + (rowItem.candidateText ? "识别候选：" + rowItem.candidateText : "暂无识别候选")
                                    + (rowItem.skipReason ? "\n" + rowItem.skipReason : "")
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 6
                                spacing: 6
                                Label { text: sourceIndex; color: timed ? Theme.listText : Theme.muted; Layout.preferredWidth: 28 }
                                Label {
                                    text: rowItem.text || qsTr("（空字幕）")
                                    color: timed ? Theme.listText : Theme.muted
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                ColumnLayout {
                                    Layout.preferredWidth: 125
                                    spacing: 1
                                    Label {
                                        text: timed ? editor.formatTime(startMs).substring(3, 12) + " - " + editor.formatTime(endMs).substring(3, 12) : qsTr("音频未检出")
                                        color: timed ? (status === "LOW_CONFIDENCE" ? Theme.warning : Theme.listTime) : Theme.muted
                                        font.family: timed ? "Consolas" : Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeTiny
                                    }
                                    Label { visible: !timed || status === "LOW_CONFIDENCE"; text: timed ? qsTr("对齐待确认 · ASR分数未知") : qsTr("可拖到时间轴"); color: Theme.warning; font.pixelSize: 9; elide: Text.ElideRight; Layout.fillWidth: true }
                                }
                            }
                            MouseArea {
                                id: rowMouse
                                objectName: "subtitleRowMouse"
                                anchors.fill: parent
                                hoverEnabled: true
                                property string cueId: ""
                                property point pressPoint
                                property bool placing: false
                                onPressed: function(event) { cueId = editor.subtitleModel.get(rowItem.index).id || ""; pressPoint = Qt.point(event.x, event.y); placing = false }
                                onPositionChanged: function(event) {
                                    if (pressed && !rowItem.timed && Math.abs(event.x - pressPoint.x) + Math.abs(event.y - pressPoint.y) > 8)
                                        placing = true
                                }
                                onReleased: function(event) {
                                    if (!placing) return
                                    const point = mapToItem(timelineScene, event.x, event.y)
                                    if (point.x >= 0 && point.x <= timelineScene.width && point.y >= 0 && point.y <= timelineScene.height)
                                        editor.locateCueAt(cueId, Math.round((point.x + timelineScene.scrollOffset) / timelineScene.pixelsPerMs))
                                    placing = false
                                }
                                preventStealing: !rowItem.timed
                                cursorShape: placing ? Qt.ClosedHandCursor : Qt.ArrowCursor
                                onClicked: editor.selectCue(index, mouseX > width - 140)
                                onDoubleClicked: {
                                    editor.selectCue(index, false)
                                    editor.createOrEditCue()
                                }
                            }
                            SubButton {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 20
                                visible: !rowItem.timed || rowItem.status === "LOW_CONFIDENCE"
                                text: rowItem.timed ? qsTr("确认") : qsTr("在播放头创建")
                                onClicked: {
                                    if (rowItem.timed) editor.confirmCue(rowItem.index)
                                    else editor.locateCue(rowItem.index)
                                }
                            }
                        }
                    }
                }
            }
        }

        Item {
            SplitView.preferredHeight: verticalSplit.height * 0.38
            SplitView.minimumHeight: 230
            TimelineSceneItem {
                id: timelineScene
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: timelineZoomBar.top
                objectName: "timelineScene"
                followDirection: editor.playing ? editor.direction : 0
                viewportInteracting: timelineZoomBar.interacting
                durationUs: editor.durationUs > 0 ? editor.durationUs : 60000000
                playheadUs: editor.positionUs
                inPointUs: editor.inPointUs
                outPointUs: editor.outPointUs
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

    Popup {
        id: cueEditor
        objectName: "cueEditorPopup"
        anchors.centerIn: Overlay.overlay
        width: 480
        height: 150
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        onClosed: timelineScene.forceActiveFocus()
        background: Rectangle {
            color: Theme.panelRaised
            border.color: Theme.border
            radius: Theme.radiusPopup
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            Label { text: qsTr("编辑字幕"); color: Theme.text; font.bold: true }
            TextField { id: cueEditorRow; visible: false }
            TextArea {
                id: cueTextEditor
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.text
                selectionColor: Theme.selection
                selectedTextColor: Theme.text
                wrapMode: TextEdit.Wrap
                background: Rectangle {
                    color: Theme.input
                    border.width: 1
                    border.color: cueTextEditor.activeFocus ? Theme.focusBorder : Theme.border
                    radius: Theme.radiusInput
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape) {
                        cueEditor.close()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        editor.setCueText(Number(cueEditorRow.text), text)
                        cueEditor.close()
                        event.accepted = true
                    }
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                SubButton { text: qsTr("取消"); onClicked: cueEditor.close() }
                PrimaryButton {
                    text: qsTr("保存")
                    onClicked: {
                        editor.setCueText(Number(cueEditorRow.text), cueTextEditor.text)
                        cueEditor.close()
                    }
                }
            }
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
        height: editor.busy ? 66 : 0
        visible: editor.busy
        color: Theme.panelRaised
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 16
            Label { text: qsTr("正在自动打轴") + " · " + parent.parent.elapsedSeconds + "s"; color: Theme.text }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: editor.alignmentProgressText; color: Theme.secondaryText }
                ProgressBar {
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
        enabled: !mediaDialog.visible && !scriptDialog.visible && !cueEditor.visible
                 && !(settingsWindow && settingsWindow.visible)
        onEntered: function(drag) { drag.accepted = drag.hasUrls && editor.canImportFiles(drag.urls) }
        onDropped: function(drop) {
            if (drop.hasUrls && editor.canImportFiles(drop.urls)) {
                editor.importFiles(drop.urls)
                drop.acceptProposedAction()
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

    Shortcut { sequence: "Ctrl+O"; onActivated: window.openMediaDialog() }
    Shortcut { sequence: "Ctrl+I"; onActivated: window.openMediaDialog() }
    Shortcut { sequence: "Space"; enabled: !window.textEditing; onActivated: editor.togglePlay() }
    Shortcut { sequence: "Ctrl+Z"; enabled: !window.textEditing; onActivated: editor.undo() }
    Shortcut { sequence: "Ctrl+Y"; enabled: !window.textEditing; onActivated: editor.redo() }
    Shortcut { sequence: "Delete"; enabled: !window.textEditing; onActivated: editor.deleteCue() }
    Shortcut { sequence: "J"; enabled: !window.textEditing; onActivated: editor.playReverse() }
    Shortcut { sequence: "K"; enabled: !window.textEditing; onActivated: editor.stop() }
    Shortcut { sequence: "L"; enabled: !window.textEditing; onActivated: editor.playForward() }
    Shortcut { sequence: "Left"; enabled: !window.textEditing; onActivated: editor.stepFrames(-1) }
    Shortcut { sequence: "Right"; enabled: !window.textEditing; onActivated: editor.stepFrames(1) }
    Shortcut { sequence: "Shift+Left"; enabled: !window.textEditing; onActivated: editor.stepFrames(-5) }
    Shortcut { sequence: "Shift+Right"; enabled: !window.textEditing; onActivated: editor.stepFrames(5) }
    Shortcut { sequence: "3"; enabled: !window.textEditing; onActivated: editor.setFollowingStart() }
    Shortcut { sequence: "4"; enabled: !window.textEditing; onActivated: editor.setPreviousEnd() }
    Shortcut { sequence: "T"; enabled: !window.textEditing; onActivated: editor.joinAroundPlayhead() }
    Shortcut { sequence: "["; enabled: !window.textEditing; onActivated: editor.setCurrentStart() }
    Shortcut { sequence: "]"; enabled: !window.textEditing; onActivated: editor.setCurrentEnd() }
    Shortcut { sequence: "C"; enabled: !window.textEditing; onActivated: editor.splitCurrentCue() }
    Shortcut { sequences: ["Return", "Enter"]; enabled: !window.textEditing; onActivated: editor.createOrEditCue() }
    Shortcut { sequences: ["Shift+Return", "Shift+Enter"]; enabled: !window.textEditing; onActivated: editor.createNextScriptCue() }
    Shortcut { sequence: "Up"; enabled: !window.textEditing; onActivated: editor.navigateCue(-1) }
    Shortcut { sequence: "Down"; enabled: !window.textEditing; onActivated: editor.navigateCue(1) }
    Shortcut { sequence: "Tab"; enabled: !window.textEditing; onActivated: editor.navigateCue(1) }
    Shortcut { sequence: "I"; enabled: !window.textEditing; onActivated: editor.setInPoint() }
    Shortcut { sequence: "O"; enabled: !window.textEditing; onActivated: editor.setOutPoint() }
    Shortcut { sequence: "Alt+I"; enabled: !window.textEditing; onActivated: editor.clearInPoint() }
    Shortcut { sequence: "Alt+O"; enabled: !window.textEditing; onActivated: editor.clearOutPoint() }
    Shortcut { sequence: "S"; enabled: !window.textEditing; onActivated: editor.toggleSnap() }
    Shortcut { sequence: "="; enabled: !window.textEditing; onActivated: editor.adjustZoomPercent(10, timelineScene.width) }
    Shortcut { sequence: "-"; enabled: !window.textEditing; onActivated: editor.adjustZoomPercent(-10, timelineScene.width) }
    Shortcut { sequence: "\\"; enabled: !window.textEditing; onActivated: editor.fitTimeline(timelineScene.width) }
}

