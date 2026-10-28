import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Window {
    id: settingsHost
    objectName: "settingsWindow"
    width: Math.min(900, Screen.desktopAvailableWidth - 32)
    height: Math.min(680, Screen.desktopAvailableHeight - 64)
    minimumWidth: Math.min(760, Screen.desktopAvailableWidth - 32)
    minimumHeight: Math.min(520, Screen.desktopAvailableHeight - 64)
    title: "SubCue 设置"
    color: Theme.background
    modality: Qt.ApplicationModal
    flags: Qt.Dialog
    palette {
        window: Theme.background; windowText: Theme.text; base: Theme.input
        alternateBase: Theme.panelSecondary; text: Theme.text; button: Theme.button
        buttonText: Theme.text; highlight: Theme.selection; highlightedText: Theme.text
        placeholderText: Theme.placeholder; mid: Theme.divider; midlight: Theme.panelRaised
        dark: Theme.border; light: Theme.panelSecondary; shadow: Theme.divider
        toolTipBase: Theme.panelRaised; toolTipText: Theme.text
    }

    property var controller
    property string requestedCategory: "asr"
    property string requestedField: ""
    function route(categoryId, fieldId) {
        requestedCategory = categoryId
        requestedField = fieldId
        if (settingsLoader.item) settingsLoader.item.applyRoute()
    }
    function requestClose() {
        if (settingsLoader.item) settingsLoader.item.requestClose()
        else hide()
    }
    onClosing: function(close) {
        close.accepted = false
        requestClose()
    }
    Component.onCompleted: if (typeof nativeTheme !== "undefined" && nativeTheme)
        nativeTheme.applyDarkTitleBar(settingsHost, Theme.titleBar, Theme.text, Theme.border)
    Label {
        anchors.centerIn: parent
        visible: settingsLoader.status !== Loader.Ready
        text: qsTr("正在加载设置…")
        color: Theme.text
    }
    Loader {
        id: settingsLoader
        objectName: "settingsContentLoader"
        anchors.fill: parent
        active: settingsHost.visible
        asynchronous: true
        sourceComponent: Component {
            Item {
                id: settingsWindow
                property var controller: settingsHost.controller
                property var asrProviderData: []
                property bool storageBusy: false
                property var storageTargets: []
                property var localModelStatus: []
                property bool storageExpanded: true
                property bool storageLoaded: false
                property int selectedCategory: 0
                function applyRoute() {
                    const categories = ["asr", "ai", "voice", "defaults", "storage", "cloud"]
                    const index = categories.indexOf(settingsHost.requestedCategory)
                    if (settingsHost.requestedCategory === "ai" || ["asrKey", "aiApiKeyField", "voiceCloudWorkspace", "voiceCloudKey", "asrApiHost"].includes(settingsHost.requestedField)) selectedCategory = 5
                    else if (index >= 0) selectedCategory = index
                    if (settingsHost.requestedCategory === "ai") cloudPanel.showTask()
                    Qt.callLater(function() {
                        function focusField(item) {
                            if (item.objectName === settingsHost.requestedField) { item.forceActiveFocus(); return true }
                            for (const child of item.children || []) if (focusField(child)) return true
                            return false
                        }
                        if (settingsHost.requestedField !== "") focusField(settingsWindow)
                    })
                }

                property bool loadingValues: true
                property string savedValues: ""
                property bool savedFeedback: false
                property var ttsEngines: ({})
                function loadVoiceEngine() {
                    const config = ttsEngines[voiceEngine.currentValue] || {}
                    voicePython.text = config.python || ""
                    voiceModels.text = config.directory || ""
                }
                function updateVoiceEngine() {
                    const configs = clone(ttsEngines)
                    configs[voiceEngine.currentValue] = { python: voicePython.text, directory: voiceModels.text }
                    ttsEngines = configs
                }
                readonly property bool dirty: !loadingValues && (snapshot() !== savedValues || cloudPanel.pending || cloudPanel.cloud.dirty)



                function settingValues() {
                    return {
                        "defaultsOnly": true,
                        "ttsEngines": ttsEngines,
                        "asrProvider": controller.defaultSetting("asrProvider"),
                        "asrModel": controller.defaultSetting("asrModel"),
                        "modelsRoot": modelsRoot.text,
                        "autoReviewEnabled": autoReview.checked,
                        "reviewUseSameAsr": reviewUseSame.checked,
                        "reviewAsrProvider": reviewProvider.currentValue || "funasr",
                        "reviewAsrModel": reviewModel.text.trim() || "Fun-ASR-Nano-2512",
                        "fontFamily": fontFamily.text,
                        "fontSize1080p": fontSize.value,
                        "alignment": ["bottom-left", "bottom-center", "bottom-right"][alignment.currentIndex],
                        "bottomMargin1080p": bottomMargin.value,
                        "outputSrt": outputSrt.checked,
                        "outputAss": outputAss.checked,
                        "outputDirectory": outputDirectory.text
                    }
                }
                function snapshot() {
                    return JSON.stringify(settingValues())
                }
                function save() {
                    if (!cloudPanel.prepareSave()) return false
                    if (controller.saveSettings(settingValues(), "", "")) {
                        savedValues = snapshot()
                        if (cloudPanel.cloud.dirty && !cloudPanel.cloud.save()) { saveStatus.text = cloudPanel.cloud.status; return false }
                        saveStatus.text = ""
                        savedFeedback = true
                        return true
                    }
                    saveStatus.text = controller.statusText
                    return false
                }
                function requestClose() {
                    if (dirty) unsavedSettings.open()
                    else settingsHost.hide()
                }
                function refreshStorage() {
                    storageBusy = true
                    storageStatus.text = "正在扫描可清理项目…"
                    controller.requestStorageTargets()
                }
                onSelectedCategoryChanged: {
                    settingsFlick.contentY = 0
                    if (selectedCategory === 4 && !storageLoaded && !storageBusy) refreshStorage()
                }

                function clone(value) { return JSON.parse(JSON.stringify(value)) }
                function formatBytes(bytes) {
                    if (bytes >= 1073741824)
                        return (bytes / 1073741824).toFixed(2) + " GiB"
                    if (bytes >= 1048576)
                        return (bytes / 1048576).toFixed(1) + " MiB"
                    if (bytes >= 1024)
                        return (bytes / 1024).toFixed(1) + " KiB"
                    return bytes + " B"
                }
                function selectedStorageIds() {
                    const ids = []
                    for (let i = 0; i < storageTargets.length; ++i)
                        if (storageTargets[i].selected) ids.push(storageTargets[i].id)
                    return ids
                }
                function selectedStorageSummary() {
                    let total = 0
                    const lines = []
                    for (let i = 0; i < storageTargets.length; ++i) {
                        if (!storageTargets[i].selected) continue
                        total += storageTargets[i].bytes
                        lines.push(storageTargets[i].label + "\n" + storageTargets[i].path)
                    }
                    return { "total": total, "text": lines.join("\n\n") }
                }
                function keepBuildText() {
                    for (let i = 0; i < storageTargets.length; ++i) {
                        if (storageTargets[i].group === "build")
                            return storageTargets[i].label
                    }
                    return ""
                }
                readonly property bool allStorageSelected: {
                    if (storageTargets.length === 0)
                        return false
                    for (let i = 0; i < storageTargets.length; ++i)
                        if (!storageTargets[i].selected)
                            return false
                    return true
                }
                function setAllStorageSelected(selected) {
                    const next = []
                    for (let i = 0; i < storageTargets.length; ++i)
                        next.push(Object.assign({}, storageTargets[i], { "selected": selected }))
                    storageTargets = next
                }
                function indexById(items, id) {
                    for (let i = 0; i < items.length; ++i)
                        if (items[i].id === id) return i
                    return items.length > 0 ? 0 : -1
                }



                function loadValues() {
                    loadingValues = true
                    ttsEngines = clone(controller.defaultSetting("ttsEngines") || {})
                    loadVoiceEngine()
                    saveStatus.text = ""
                    asrProviderData = controller.asrProviders().filter(row => row.id !== "dashscope")
                    modelsRoot.text = controller.defaultSetting("modelsRoot") || ""
                    localModelStatus = controller.modelStatus(modelsRoot.text)

                    autoReview.checked = controller.defaultSetting("autoReviewEnabled") || false
                    reviewUseSame.checked = controller.defaultSetting("reviewUseSameAsr") !== false
                    reviewProvider.currentIndex = indexById(asrProviderData, controller.defaultSetting("reviewAsrProvider") || "funasr")
                    reviewModel.text = controller.defaultSetting("reviewAsrModel") || "Fun-ASR-Nano-2512"

                    fontFamily.loadValue(controller.defaultSetting("fontFamily") || "Microsoft YaHei")
                    fontSize.value = controller.defaultSetting("fontSize1080p") || 52
                    alignment.currentIndex = controller.defaultSetting("alignment") === "bottom-left" ? 0
                        : (controller.defaultSetting("alignment") === "bottom-right" ? 2 : 1)
                    bottomMargin.value = controller.defaultSetting("bottomMargin1080p")
                    outputSrt.checked = controller.defaultSetting("outputSrt")
                    outputAss.checked = controller.defaultSetting("outputAss")
                    outputDirectory.text = controller.defaultSetting("outputDirectory") || ""
                    savedValues = snapshot()
                    loadingValues = false
                }

                Component.onCompleted: { applyRoute(); settingsInitialization.start() }
                Timer { id: settingsInitialization; interval: 0; onTriggered: settingsWindow.loadValues() }
                Timer {
                    id: modelStatusDelay
                    interval: 350
                    onTriggered: localModelStatus = controller.modelStatus(modelsRoot.text)
                }

                Connections {
                    target: settingsWindow.controller




                    function onStorageTargetsReady(targets) {
                        settingsWindow.storageBusy = false
                        settingsWindow.storageLoaded = true
                        const next = []
                        for (let i = 0; i < targets.length; ++i) {
                            const item = targets[i]
                            next.push({
                                "id": item.id,
                                "group": item.group,
                                "label": item.label,
                                "path": item.path,
                                "bytes": item.bytes,
                                "selected": item.group === "cache"
                            })
                        }
                        settingsWindow.storageTargets = next
                        if (storageStatus.text === "正在扫描可清理项目…")
                            storageStatus.text = targets.length === 0 ? "没有可清理的项目" : "请选择要清理的项目"
                    }
                    function onStorageCleanupFinished(result) {
                        settingsWindow.storageBusy = false
                        if (result.success) {
                            storageStatus.text = "已清理 " + settingsWindow.formatBytes(result.bytesRemoved || 0)
                            settingsWindow.storageBusy = true
                            controller.requestStorageTargets()
                            localModelStatus = controller.modelStatus(modelsRoot.text)
                        } else {
                            storageStatus.failed = true
                            storageStatus.text = result.error || "清理失败"
                        }
                    }
                }

                component FieldLabel: Label {
                    color: Theme.text
                    font.family: Theme.fontFamily
                    Layout.preferredWidth: 96
                    Layout.alignment: Qt.AlignVCenter
                }
                component SectionTitle: Label {
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: 15
                    topPadding: 4
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                }
                component StatusLabel: Label {
                    property bool failed: false
                    color: failed ? Theme.error : text.indexOf("已验证") === 0 || text.indexOf("连接成功") === 0 || text.indexOf("已清理") === 0 ? Theme.success : Theme.secondaryText
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.preferredWidth: 145
                    wrapMode: Text.Wrap
                }

                Rectangle {
                    anchors.fill: parent
                    color: Theme.background
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 0
                            Rectangle {
                                Layout.preferredWidth: 148
                                Layout.fillHeight: true
                                color: Theme.panel
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 6
                                    Label { text: "设置"; color: Theme.text; font.bold: true; font.pixelSize: 18; Layout.bottomMargin: 12 }
                                    Repeater {
                                        model: [{category: 5, name: "AI 服务"}, {category: 0, name: "语音识别与复核"}, {category: 2, name: "AI 配音"}, {category: 3, name: "新工程默认值"}, {category: 4, name: "存储与恢复"}]
                                        delegate: SubButton {
                                            required property int index
                                            required property var modelData
                                            objectName: "settingsCategory" + modelData.category
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            highlighted: settingsWindow.selectedCategory === modelData.category
                                            background: Rectangle {
                                                radius: Theme.radiusButton
                                                color: settingsWindow.selectedCategory === modelData.category ? Theme.selection
                                                     : parent.hovered ? Theme.buttonHover : "transparent"
                                                border.width: parent.visualFocus ? 1 : 0
                                                border.color: Theme.focusBorder
                                            }
                                            onClicked: settingsWindow.selectedCategory = modelData.category
                                        }
                                    }
                                    Item { Layout.fillHeight: true }
                                    Label {
                                        Layout.fillWidth: true
                                        text: "切换分类保留更改\n点击保存后统一生效"
                                        wrapMode: Text.Wrap
                                        color: Theme.secondaryText
                                        font.pixelSize: Theme.fontSizeSmall
                                    }
                                }
                            }
                        Item {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            CloudSettingsPanel {
                                id: cloudPanel; objectName: "cloudSettingsPanel"
                                anchors.fill: parent; anchors.margins: 16
                                cloud: controller.cloudSettings
                                visible: settingsWindow.selectedCategory === 5
                            }
                        Flickable {
                            id: settingsFlick
                            objectName: "settingsScroll"
                            anchors.fill: parent
                            visible: settingsWindow.selectedCategory !== 5
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            flickableDirection: Flickable.VerticalFlick
                            contentWidth: width
                            contentHeight: settingsGrid.implicitHeight + 24
                            ScrollBar.vertical: SubScrollBar {
                                parent: settingsFlick
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                            }

                            ColumnLayout {
                                id: settingsGrid
                                width: Math.max(0, settingsFlick.width - 18 - 22)
                                x: 18
                                y: 12
                                spacing: 16

                            GridLayout {
                                Layout.fillWidth: true
                                visible: settingsWindow.selectedCategory === 0
                                columns: 3
                                columnSpacing: 10
                                rowSpacing: 10

                                SectionTitle { text: "语音识别" }
                                Label { Layout.columnSpan: 3; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("当前用途：") + cloudPanel.targetName("speechRecognition") }
                                SubComboBox {
                                    objectName: "unifiedAsrSelector"; Layout.columnSpan: 3; Layout.fillWidth: true
                                    textRole: "name"; valueRole: "id"
                                    model: [{id: "local:funasr", name: "本地 Fun-ASR"}, {id: "local:qwen3", name: "本地 Qwen3-ASR"}].concat(cloudPanel.cloud.models.filter(function(row) { return row.adapter === "dashscope-asr" && row.enabled !== false }).map(function(row) { return {id: row.id, name: row.name} }))
                                    currentIndex: model.findIndex(function(row) { const binding = cloudPanel.cloud.bindings.find(function(value) { return value.taskId === "speechRecognition" }); return binding && row.id === binding.target })
                                    onActivated: cloudPanel.cloud.bindModel("speechRecognition", currentValue)
                                }
                                SubButton { Layout.columnSpan: 3; text: qsTr("管理账号与识别模型"); onClicked: settingsWindow.selectedCategory = 5 }

                                FieldLabel { text: "模型根目录" }
                                SubTextField {
                                    id: modelsRoot
                                    objectName: "modelsRootField"
                                    Layout.fillWidth: true
                                    enabled: true
                                    placeholderText: "例如 E:\\\\AIModels\\\\ASR\\\\models"
                                    onTextEdited: {
                                        modelStatusDelay.restart()
                                    }
                                    onEditingFinished: {
                                        modelStatusDelay.stop()
                                        localModelStatus = controller.modelStatus(modelsRoot.text)
                                    }
                                }
                                SubButton {
                                    text: "浏览"
                                    enabled: true
                                    onClicked: modelsRootDialog.open()
                                }
                                Repeater {
                                    model: localModelStatus
                                    delegate: Label {
                                        Layout.columnSpan: 3
                                        Layout.fillWidth: true
                                        color: modelData.ready ? Theme.success : Theme.secondaryText
                                        font.pixelSize: Theme.fontSizeSmall
                                        wrapMode: Text.WrapAnywhere
                                        text: modelData.name + " · "
                                              + (modelData.ready ? "已安装" : "未安装（运行 tools\\setup-models.ps1）")
                                              + " · " + modelData.directory
                                    }
                                }

                            }
                            GridLayout {
                                Layout.fillWidth: true
                                visible: settingsWindow.selectedCategory === 0
                                columns: 3
                                columnSpacing: 10
                                rowSpacing: 10
                                SectionTitle { objectName: "asrReviewSection"; text: "ASR 复核" }
                                FieldLabel { text: "自动开启" }
                                SubCheckBox { id: autoReview; text: "自动打轴后立即用 ASR 复核（默认关闭）"; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "ASR 模型" }
                                SubCheckBox { id: reviewUseSame; text: "使用与自动打轴相同的模型"; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "复核服务"; visible: !reviewUseSame.checked }
                                SubComboBox { id: reviewProvider; visible: !reviewUseSame.checked; model: asrProviderData; textRole: "name"; valueRole: "id"; Layout.fillWidth: true }
                                Item { visible: !reviewUseSame.checked; Layout.fillWidth: true }
                                FieldLabel { text: "复核模型"; visible: !reviewUseSame.checked }
                                SubTextField { id: reviewModel; visible: !reviewUseSame.checked; Layout.fillWidth: true; placeholderText: "Fun-ASR-Nano-2512" }
                                StatusLabel { visible: !reviewUseSame.checked; text: "重新识别并标记低置信度内容" }

                            }


                            GridLayout {
                                Layout.fillWidth: true
                                visible: settingsWindow.selectedCategory === 2
                                columns: 2; columnSpacing: 12; rowSpacing: 10
                                SectionTitle { text: qsTr("AI 配音引擎"); Layout.columnSpan: 2 }
                                Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("各引擎独立记忆路径。检查使用当前输入，保存后工作区才会使用新配置。"); color: Theme.secondaryText }
                                FieldLabel { text: qsTr("引擎") }
                                SubComboBox {
                                    id: voiceEngine; objectName: "settingsVoiceEngine"; Layout.fillWidth: true
                                    textRole: "name"; valueRole: "id"
                                    model: [{id: "qwen", name: "Qwen Base"}, {id: "index", name: "IndexTTS 2.5"}, {id: "gpt", name: "GPT-SoVITS"}, {id: "mock", name: "Mock 测试音"}]
                                    onActivated: settingsWindow.loadVoiceEngine()
                                }
                                FieldLabel { text: "Python" }
                                SubTextField { id: voicePython; objectName: "settingsVoicePython"; Layout.fillWidth: true; selectByMouse: true; onTextEdited: settingsWindow.updateVoiceEngine() }
                                FieldLabel { text: qsTr("模型／源码目录") }
                                SubTextField { id: voiceModels; objectName: "settingsVoiceModels"; Layout.fillWidth: true; selectByMouse: true; onTextEdited: settingsWindow.updateVoiceEngine() }
                                SubButton { text: qsTr("检查当前环境"); enabled: typeof voiceStudio !== "undefined" && voiceStudio && !voiceStudio.busy && voiceEngine.currentValue !== "mock"; onClicked: voiceStudio.checkLocal(voiceEngine.currentValue, voicePython.text, voiceModels.text) }
                                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: typeof voiceStudio !== "undefined" && voiceStudio ? voiceStudio.statusText : ""; color: Theme.secondaryText }
                                SectionTitle { text: qsTr("云端配音"); Layout.columnSpan: 2 }
                                Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("当前用途：") + cloudPanel.targetName("speechSynthesis") }
                                SubButton { Layout.columnSpan: 2; text: qsTr("管理配音账号、模型与 Workspace"); onClicked: settingsWindow.selectedCategory = 5 }

                            }
                            GridLayout {
                                Layout.fillWidth: true
                                visible: settingsWindow.selectedCategory === 3
                                columns: 3
                                columnSpacing: 10
                                rowSpacing: 10
                                SectionTitle { text: "新工程默认值" }
                                FieldLabel { text: "字体" }
                                FontPicker { id: fontFamily; objectName: "fontFamilyField"; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "1080p 字号" }
                                RowLayout {
                                    Layout.fillWidth: true
                                    SubSpinBox { id: fontSize; from: 12; to: 200 }
                                    Label { text: "底部上抬"; color: Theme.text }
                                    SubSpinBox { id: bottomMargin; from: 0; to: 500 }
                                }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "水平位置" }
                                SubComboBox { id: alignment; model: ["左对齐", "居中", "右对齐"]; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "输出格式" }
                                RowLayout {
                                    SubCheckBox { id: outputSrt; text: "SRT" }
                                    SubCheckBox { id: outputAss; text: "ASS" }
                                }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "输出目录" }
                                SubTextField { id: outputDirectory; Layout.fillWidth: true; placeholderText: "留空使用媒体目录" }
                                SubButton { text: "浏览"; onClicked: outputDialog.open() }

                            }
                            GridLayout {
                                Layout.fillWidth: true
                                visible: settingsWindow.selectedCategory === 4
                                columns: 3
                                columnSpacing: 10
                                rowSpacing: 10
                                RowLayout {
                                    Layout.columnSpan: 3
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        objectName: "storageSection"
                                        text: "存储与清理"
                                        color: Theme.text
                                        font.bold: true
                                        font.pixelSize: 15
                                        topPadding: 4
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    SubButton {
                                        objectName: "refreshStorageButton"
                                        text: "重新扫描"
                                        enabled: !settingsWindow.storageBusy
                                        onClicked: settingsWindow.refreshStorage()
                                    }
                                    SubButton {
                                        objectName: "expandStorageButton"
                                        text: "展开"
                                        enabled: !settingsWindow.storageExpanded
                                        onClicked: settingsWindow.storageExpanded = true
                                    }
                                    SubButton {
                                        objectName: "collapseStorageButton"
                                        text: "折叠"
                                        enabled: settingsWindow.storageExpanded
                                        onClicked: settingsWindow.storageExpanded = false
                                    }
                                }
                                ColumnLayout {
                                    Layout.columnSpan: 3
                                    Layout.fillWidth: true
                                    spacing: 8
                                    visible: settingsWindow.storageExpanded

                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.muted
                                        font.pixelSize: Theme.fontSizeSmall
                                        text: keepBuildText() !== ""
                                              ? keepBuildText()
                                              : "清理应用缓存、测试残留和过时构建。不会删除模型根目录、源码或正式测试夹具。"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: "清理会立即删除所选文件，无法撤回；取消或关闭设置不会撤销已执行的清理。"
                                        color: Theme.warning
                                        wrapMode: Text.Wrap
                                        font.pixelSize: Theme.fontSizeSmall
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        SubCheckBox {
                                            id: storageSelectAll
                                            objectName: "selectAllStorage"
                                            text: "全选"
                                            enabled: !settingsWindow.storageBusy && storageTargets.length > 0
                                            onClicked: settingsWindow.setAllStorageSelected(checked)
                                        }
                                        StatusLabel {
                                            id: storageStatus
                                            Layout.fillWidth: true
                                            text: storageTargets.length === 0 ? "没有可清理的项目" : ""
                                        }
                                        SubButton {
                                            objectName: "cleanupSelectedButton"
                                            text: "清理所选"
                                            contentItem: Text {
                                                text: parent.text
                                                font: parent.font
                                                color: parent.enabled ? Theme.danger : Theme.disabledText
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            Layout.alignment: Qt.AlignRight
                                            enabled: !settingsWindow.storageBusy && selectedStorageIds().length > 0
                                            onClicked: {
                                                const summary = selectedStorageSummary()
                                                confirmCleanup.summaryText = summary.text
                                                confirmCleanup.bytesText = settingsWindow.formatBytes(summary.total)
                                                confirmCleanup.open()
                                            }
                                        }
                                    }
                                    Binding {
                                        target: storageSelectAll
                                        property: "checked"
                                        value: settingsWindow.allStorageSelected
                                        restoreMode: Binding.RestoreBinding
                                    }
                                    Repeater {
                                        model: storageTargets
                                        delegate: Item {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 44
                                            RowLayout {
                                                anchors.fill: parent
                                                spacing: 8
                                                SubCheckBox {
                                                    checked: modelData.selected
                                                    enabled: !settingsWindow.storageBusy
                                                    onToggled: {
                                                        const next = storageTargets.slice()
                                                        next[index] = Object.assign({}, next[index], { "selected": checked })
                                                        storageTargets = next
                                                    }
                                                }
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 0
                                                    Label {
                                                        Layout.fillWidth: true
                                                        color: Theme.text
                                                        elide: Text.ElideRight
                                                        text: modelData.label + " · " + settingsWindow.formatBytes(modelData.bytes)
                                                    }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        color: Theme.muted
                                                        font.pixelSize: Theme.fontSizeSmall
                                                        elide: Text.ElideMiddle
                                                        text: modelData.path
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Item { Layout.columnSpan: 3; Layout.preferredHeight: 8 }
                            }
                            }
                        }
                        }

                        }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.max(64, settingsFooter.implicitHeight + 24)
                            color: Theme.panelRaised
                            RowLayout {
                                id: settingsFooter
                                anchors.fill: parent
                                anchors.margins: 14
                                Label {
                                    id: saveStatus
                                    Layout.fillWidth: true
                                    visible: text !== ""
                                    color: Theme.error
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: saveStatus.text === ""
                                    text: settingsWindow.dirty ? "未保存" : settingsWindow.savedFeedback ? "已保存" : "没有未保存更改"
                                    color: settingsWindow.dirty ? Theme.warning : Theme.secondaryText
                                }
                                SubButton { objectName: "cancelSettingsButton"; text: "关闭"; onClicked: settingsWindow.requestClose() }
                                PrimaryButton {
                                    objectName: "saveSettingsButton"
                                    text: "保存更改"
                                    enabled: settingsWindow.dirty
                                    onClicked: settingsWindow.save()
                                }
                            }
                        }
                    }
                }

                FolderDialog {
                    id: outputDialog
                    title: "选择输出目录"
                    parentWindow: settingsHost
                    onAccepted: outputDirectory.text = controller.localPath(selectedFolder)
                }
                FolderDialog {
                    id: modelsRootDialog
                    title: "选择模型根目录"
                    parentWindow: settingsHost
                    onAccepted: {
                        modelsRoot.text = controller.localPath(selectedFolder)
                        localModelStatus = controller.modelStatus(modelsRoot.text)
                        modelStatusDelay.stop()
                    }
                }
                Dialog {
                    id: unsavedSettings
                    objectName: "unsavedSettingsDialog"
                    title: "保存设置更改？"
                    modal: true
                    parent: settingsHost.contentItem
                    anchors.centerIn: parent
                    width: Math.min(460, settingsHost.width - 40)
                    standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
                    onOpened: {
                        standardButton(Dialog.Save).text = "保存并关闭"
                        standardButton(Dialog.Save).enabled = true
                        standardButton(Dialog.Discard).text = "放弃更改"
                        standardButton(Dialog.Cancel).text = "继续编辑"
                    }
                    contentItem: Label {
                        text: "当前分类和其他分类中有未保存的更改。"
                        wrapMode: Text.Wrap
                        color: Theme.text
                    }
                    onAccepted: if (settingsWindow.save()) settingsHost.hide()
                    onDiscarded: settingsHost.hide()
                }
                Shortcut {
                    sequence: "Ctrl+S"
                    enabled: settingsHost.active && settingsWindow.dirty && !unsavedSettings.visible && !confirmCleanup.visible

                    onActivated: settingsWindow.save()
                }
                Dialog {
                    id: confirmCleanup
                    title: "确认清理"
                    modal: true
                    parent: settingsHost.contentItem
                    anchors.centerIn: parent
                    property string summaryText: ""
                    property string bytesText: ""
                    standardButtons: Dialog.Ok | Dialog.Cancel
                    width: Math.min(520, settingsHost.width - 40)
                    onOpened: {
                        standardButton(Dialog.Ok).text = "清理"
                        standardButton(Dialog.Cancel).text = "取消"
                    }
                    contentItem: ColumnLayout {
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.warning
                            text: "将立即删除所选项目，合计 " + confirmCleanup.bytesText + "。此操作不可恢复。"
                        }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(240, cleanupSummary.implicitHeight + 16)
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                            TextArea {
                                id: cleanupSummary
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.WrapAnywhere
                                color: Theme.text
                                text: confirmCleanup.summaryText
                                background: Rectangle { color: Theme.panel }
                            }
                        }
                    }
                    onAccepted: {
                        settingsWindow.storageBusy = true
                        storageStatus.failed = false
                        storageStatus.text = "正在清理…"
                        controller.cleanupStorage(selectedStorageIds())
                    }
                }
            }
        }
    }
}
