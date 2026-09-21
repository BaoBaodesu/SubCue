import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Window {
    id: settingsHost
    objectName: "settingsWindow"
    width: 720
    height: 640
    minimumWidth: 580
    minimumHeight: 480
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
    property int modelRequestId: 0
    property int asrCredentialRequest: 0
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
                property var asrModelData: []
                property bool modelLoading: false
                readonly property int asrCredentialRequest: settingsHost.asrCredentialRequest
                readonly property int modelRequestId: settingsHost.modelRequestId
                property string preferredModel: ""
                property var asrVerification: ({})
                property int asrConfigRevision: 0
                property int asrCredentialRevision: 0
                property bool asrOperationBusy: false
                property bool aiOperationBusy: false
                property bool clearAiKeyPending: false

                function clone(value) { return JSON.parse(JSON.stringify(value)) }
                function indexById(items, id) {
                    for (let i = 0; i < items.length; ++i)
                        if (items[i].id === id) return i
                    return items.length > 0 ? 0 : -1
                }
                function invalidateAsr(configChanged) {
                    if (configChanged !== false) ++asrConfigRevision
                    asrVerification = ({})
                    asrStatus.text = "未验证"
                }
                function refreshAiKeyStatus() {
                    aiCredentialStatus.text = controller.aiKeySource(aiKey.text, settingsWindow.clearAiKeyPending)
                    if (settingsWindow.clearAiKeyPending && aiKey.text.length === 0)
                        aiStatus.text = "将在保存时清除应用保存的密钥"
                }
                function refreshAsrModels(preferred) {
                    preferredModel = preferred || ""
                    modelLoading = true
                    ++settingsHost.modelRequestId
                    controller.requestAsrModels(asrProvider.currentValue || "dashscope", "",
                                                settingsHost.modelRequestId)
                }
                function loadValues() {
                    saveStatus.text = ""
                    settingsWindow.clearAiKeyPending = false
                    asrProviderData = controller.asrProviders()
                    const savedAsr = controller.setting("asrProvider") || "dashscope"
                    asrProvider.currentIndex = indexById(asrProviderData, savedAsr)
                    refreshAsrModels(controller.setting("asrModel"))
                    region.currentIndex = controller.setting("region") === "singapore" ? 1 : 0
                    asrApiHost.text = controller.setting("asrApiHost") || ""
                    asrVerification = clone(controller.setting("asrVerification") || {})
                    asrConfigRevision = controller.setting("asrConfigRevision") || 0
                    asrCredentialRevision = controller.setting("asrCredentialRevision") || 0
                    asrCredentialStatus.text = savedAsr === "dashscope"
                        ? controller.requestCredentialStatus("SubCue/ASR/dashscope", ++settingsHost.asrCredentialRequest) : "尚未配置"
                    asrStatus.text = controller.verificationStatus("asr")

                    autoReview.checked = controller.setting("autoReviewEnabled") || false
                    reviewUseSame.checked = controller.setting("reviewUseSameAsr") !== false
                    reviewProvider.currentIndex = indexById(asrProviderData, controller.setting("reviewAsrProvider") || "funasr")
                    reviewModel.text = controller.setting("reviewAsrModel") || "Fun-ASR-Nano-2512"

                    aiKey.clear()
                    refreshAiKeyStatus()
                    aiStatus.text = "未验证"

                    fontFamily.text = controller.setting("fontFamily") || "Microsoft YaHei"
                    fontSize.value = controller.setting("fontSize1080p") || 52
                    alignment.currentIndex = controller.setting("alignment") === "bottom-left" ? 0
                        : (controller.setting("alignment") === "bottom-right" ? 2 : 1)
                    bottomMargin.value = controller.setting("bottomMargin1080p") || 90
                    outputSrt.checked = controller.setting("outputSrt")
                    outputAss.checked = controller.setting("outputAss")
                    outputDirectory.text = controller.setting("outputDirectory") || ""
                    asrKey.clear()
                }

                onVisibleChanged: if (visible) Qt.callLater(loadValues)
                Component.onCompleted: Qt.callLater(loadValues)

                Connections {
                    target: settingsWindow.controller
                    function onAsrModelsReady(requestId, models) {
                        if (requestId !== settingsWindow.modelRequestId) return
                        modelLoading = false
                        asrModelData = models
                        asrModel.currentIndex = indexById(models, settingsWindow.preferredModel)
                    }
                    function onCredentialStatusReady(id, status, requestId) {
                        if (!settingsWindow.visible) return
                        if (id === "SubCue/ASR/dashscope" && requestId === asrCredentialRequest && asrProvider.currentValue === "dashscope")
                            asrCredentialStatus.text = status
                    }
                    function onAsrConnectionTestFinished(result) {
                        settingsWindow.asrOperationBusy = false
                        if (result.success) {
                            settingsWindow.asrVerification = {
                                "providerId": asrProvider.currentValue,
                                "configRevision": settingsWindow.asrConfigRevision,
                                "credentialRevision": settingsWindow.asrCredentialRevision + (asrKey.text.length > 0 ? 1 : 0),
                                "selectedModel": asrModel.currentValue,
                                "verifiedAtUtc": result.verifiedAtUtc
                            }
                            asrStatus.text = "已验证"
                        } else {
                            settingsWindow.invalidateAsr(false)
                            asrStatus.text = result.error
                        }
                    }
                    function onAiConnectionTestFinished(result) {
                        settingsWindow.aiOperationBusy = false
                        if (result.success) {
                            let text = "连接成功 · " + (result.model || "qwen3.8-omni-flash")
                                + " · 延迟 " + result.latencyMs + " ms"
                            if (result.totalTokens >= 0)
                                text += " · Token " + result.totalTokens
                            aiStatus.text = text
                        } else {
                            aiStatus.text = result.error || "连接失败"
                        }
                    }
                }

                component FieldLabel: Label {
                    color: Theme.text
                    font.family: Theme.fontFamily
                    Layout.preferredWidth: 120
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
                    color: text.indexOf("已验证") === 0 ? Theme.success : Theme.muted
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.preferredWidth: 145
                    elide: Text.ElideRight
                }

                Rectangle {
                    anchors.fill: parent
                    color: Theme.background
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.vertical: SubScrollBar { }
                            contentWidth: availableWidth

                            GridLayout {
                                width: Math.max(0, parent.width - 36)
                                x: 18
                                columns: 3
                                columnSpacing: 10
                                rowSpacing: 10

                                SectionTitle { text: "语音识别" }
                                FieldLabel { text: "Provider" }
                                SubComboBox {
                                    id: asrProvider
                                    Layout.fillWidth: true
                                    enabled: !settingsWindow.asrOperationBusy
                                    model: asrProviderData
                                    textRole: "name"
                                    valueRole: "id"
                                    onActivated: {
                                        refreshAsrModels("")
                                        invalidateAsr()
                                    }
                                }
                                StatusLabel { id: asrStatus; text: "未验证" }

                                FieldLabel { text: "ASR 模型" }
                                SubComboBox {
                                    id: asrModel
                                    Layout.fillWidth: true
                                    enabled: !settingsWindow.modelLoading && !settingsWindow.asrOperationBusy
                                    model: asrModelData
                                    textRole: "name"
                                    valueRole: "id"
                                    onActivated: invalidateAsr()
                                }
                                StatusLabel {
                                    visible: asrProvider.currentValue === "qwen3" || asrProvider.currentValue === "funasr"
                                    text: asrModel.currentIndex >= 0 && asrModel.currentIndex < asrModelData.length
                                          ? (asrModelData[asrModel.currentIndex].ready ? "模型已安装" : "模型未安装或权重未完成")
                                          : "正在检查模型"
                                }

                                Item { visible: asrProvider.currentValue === "dashscope"; Layout.fillWidth: true }

                                FieldLabel { text: "服务地域"; visible: asrProvider.currentValue === "dashscope" }
                                SubComboBox {
                                    id: region
                                    visible: asrProvider.currentValue === "dashscope"
                                    enabled: !settingsWindow.asrOperationBusy
                                    model: ["beijing", "singapore"]
                                    Layout.fillWidth: true
                                    onActivated: invalidateAsr()
                                }
                                Item { visible: asrProvider.currentValue === "dashscope"; Layout.fillWidth: true }

                                FieldLabel { text: "API Host"; visible: asrProvider.currentValue === "dashscope" }
                                SubTextField {
                                    id: asrApiHost
                                    visible: asrProvider.currentValue === "dashscope"
                                    enabled: !settingsWindow.asrOperationBusy
                                    Layout.fillWidth: true
                                    placeholderText: "留空使用公共地址，或粘贴控制台 API Host"
                                    onTextEdited: invalidateAsr()
                                }
                                StatusLabel {
                                    visible: asrProvider.currentValue === "dashscope"
                                    text: "支持 https://{WorkspaceId}..."
                                }

                                FieldLabel { text: "API Key"; visible: asrProvider.currentValue === "dashscope" }
                                SubTextField {
                                    id: asrKey
                                    visible: asrProvider.currentValue === "dashscope"
                                    enabled: !settingsWindow.asrOperationBusy
                                    Layout.fillWidth: true
                                    echoMode: TextInput.Password
                                    placeholderText: "留空保持 Credential Manager 中的密钥"
                                    onTextEdited: invalidateAsr(false)
                                }
                                StatusLabel { id: asrCredentialStatus; visible: asrProvider.currentValue === "dashscope" }


                                Item { Layout.preferredWidth: 1 }
                                SubButton {
                                    text: "测试连接"
                                    enabled: !settingsWindow.asrOperationBusy
                                    Layout.alignment: Qt.AlignLeft
                                    onClicked: {
                                        const values = {
                                            "asrProvider": asrProvider.currentValue,
                                            "asrModel": asrProvider.currentValue === "dashscope" ? asrModel.currentValue : "",
                                            "region": region.currentText,
                                            "asrApiHost": asrApiHost.text
                                        }
                                        settingsWindow.asrOperationBusy = true
                                        asrStatus.text = "正在测试…"
                                        controller.testAsrConnection(values, asrKey.text)
                                    }
                                }
                                Item { Layout.fillWidth: true }

                                Rectangle { Layout.columnSpan: 3; Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                SectionTitle { objectName: "asrReviewSection"; text: "ASR 复核" }
                                FieldLabel { text: "自动开启" }
                                SubCheckBox { id: autoReview; text: "自动打轴后立即用 ASR 复核（默认关闭）"; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "ASR 模型" }
                                SubCheckBox { id: reviewUseSame; text: "使用与自动打轴相同的模型"; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }
                                FieldLabel { text: "复核 Provider"; visible: !reviewUseSame.checked }
                                SubComboBox { id: reviewProvider; visible: !reviewUseSame.checked; model: asrProviderData; textRole: "name"; valueRole: "id"; Layout.fillWidth: true }
                                Item { visible: !reviewUseSame.checked; Layout.fillWidth: true }
                                FieldLabel { text: "复核模型"; visible: !reviewUseSame.checked }
                                SubTextField { id: reviewModel; visible: !reviewUseSame.checked; Layout.fillWidth: true; placeholderText: "Fun-ASR-Nano-2512" }
                                StatusLabel { visible: !reviewUseSame.checked; text: "ASR 复核会重新识别并更新低置信度及时间位置" }

                                Rectangle { Layout.columnSpan: 3; Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                SectionTitle { objectName: "aiAssistSection"; text: "AI 辅助" }
                                FieldLabel { text: "说明" }
                                Label {
                                    objectName: "aiAssistHint"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: Theme.muted
                                    font.pixelSize: Theme.fontSizeSmall
                                    text: "统一使用阿里云百炼北京地域 · qwen3.8-omni-flash。使用百炼北京地域 API Key。打轴、字幕语义与粗剪复核均需手动触发。"
                                }
                                Item { Layout.fillWidth: true }

                                FieldLabel { text: "API Key" }
                                SubTextField {
                                    id: aiKey
                                    objectName: "aiApiKeyField"
                                    Layout.fillWidth: true
                                    echoMode: TextInput.Password
                                    enabled: !settingsWindow.aiOperationBusy
                                    placeholderText: "使用百炼北京地域 API Key；留空保留已保存密钥"
                                    onTextEdited: {
                                        settingsWindow.clearAiKeyPending = false
                                        refreshAiKeyStatus()
                                        aiStatus.text = "未验证"
                                    }
                                }
                                RowLayout {
                                    spacing: 4
                                    SubButton {
                                        objectName: "clearAiKeyButton"
                                        text: "清除密钥"
                                        enabled: !settingsWindow.aiOperationBusy
                                        onClicked: {
                                            aiKey.clear()
                                            settingsWindow.clearAiKeyPending = true
                                            refreshAiKeyStatus()
                                        }
                                    }
                                }

                                FieldLabel { text: "密钥来源" }
                                StatusLabel { id: aiCredentialStatus; Layout.fillWidth: true }
                                Item { Layout.fillWidth: true }

                                FieldLabel { text: "连接状态" }
                                StatusLabel { id: aiStatus; objectName: "aiConnectionStatus"; Layout.fillWidth: true }
                                SubButton {
                                    objectName: "testAiConnectionButton"
                                    text: "测试连接"
                                    enabled: !settingsWindow.aiOperationBusy
                                    onClicked: {
                                        settingsWindow.aiOperationBusy = true
                                        aiStatus.text = "正在测试…"
                                        controller.testAiConnection(aiKey.text)
                                    }
                                }

                                Rectangle { Layout.columnSpan: 3; Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                SectionTitle { text: "字幕与导出" }
                                FieldLabel { text: "字体" }
                                SubTextField { id: fontFamily; Layout.fillWidth: true }
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
                                Item { Layout.columnSpan: 3; Layout.preferredHeight: 8 }
                            }
                        }

                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 58
                            color: Theme.panelRaised
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                Label {
                                    id: saveStatus
                                    Layout.fillWidth: true
                                    color: Theme.error
                                    elide: Text.ElideRight
                                }
                                SubButton { text: "取消"; onClicked: settingsHost.hide() }
                                PrimaryButton {
                                    text: "保存"
                                    enabled: !settingsWindow.modelLoading && !settingsWindow.asrOperationBusy && !settingsWindow.aiOperationBusy
                                    onClicked: {
                                        const ok = controller.saveSettings({
                                            "asrProvider": asrProvider.currentValue,
                                            "asrModel": asrModel.currentValue,
                                            "region": region.currentText,
                                            "asrApiHost": asrApiHost.text,
                                            "asrVerification": asrVerification,
                                            "asrConfigRevision": asrConfigRevision,
                                            "asrCredentialRevision": asrCredentialRevision,
                                            "autoReviewEnabled": autoReview.checked,
                                            "reviewUseSameAsr": reviewUseSame.checked,
                                            "reviewAsrProvider": reviewProvider.currentValue || "funasr",
                                            "reviewAsrModel": reviewModel.text.trim() || "Fun-ASR-Nano-2512",
                                            "clearAiApiKey": settingsWindow.clearAiKeyPending,
                                            "fontFamily": fontFamily.text,
                                            "fontSize1080p": fontSize.value,
                                            "alignment": ["bottom-left", "bottom-center", "bottom-right"][alignment.currentIndex],
                                            "bottomMargin1080p": bottomMargin.value,
                                            "outputSrt": outputSrt.checked,
                                            "outputAss": outputAss.checked,
                                            "outputDirectory": outputDirectory.text
                                        }, asrKey.text, aiKey.text)
                                        if (ok) {
                                            saveStatus.text = ""
                                            settingsHost.hide()
                                        } else {
                                            saveStatus.text = controller.statusText
                                        }
                                    }
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
            }
        }
    }
}
