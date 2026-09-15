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
    property int aiCredentialRequest: 0
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
                property var providers: []
                property var asrProviderData: []
                property var asrModelData: []
                property bool modelLoading: false
                readonly property int asrCredentialRequest: settingsHost.asrCredentialRequest
                readonly property int aiCredentialRequest: settingsHost.aiCredentialRequest
                readonly property int modelRequestId: settingsHost.modelRequestId
                property string preferredModel: ""
                property var asrVerification: ({})
                property int asrConfigRevision: 0
                property int asrCredentialRevision: 0
                property int editingProviderIndex: -1
                property string editingProviderKind: "openai"
                property bool asrOperationBusy: false
                property bool aiOperationBusy: false

                function clone(value) { return JSON.parse(JSON.stringify(value)) }
                function indexById(items, id) {
                    for (let i = 0; i < items.length; ++i)
                        if (items[i].id === id) return i
                    return items.length > 0 ? 0 : -1
                }
                function selectedProvider() {
                    return aiProvider.currentIndex >= 0 && aiProvider.currentIndex < providers.length
                           ? providers[aiProvider.currentIndex] : null
                }
                function invalidateAsr(configChanged) {
                    if (configChanged !== false) ++asrConfigRevision
                    asrVerification = ({})
                    asrStatus.text = "未验证"
                }
                function invalidateAi(configChanged) {
                    const provider = selectedProvider()
                    if (!provider) return
                    if (configChanged !== false)
                        provider.configRevision = (provider.configRevision || 0) + 1
                    provider.verification = ({})
                    providers = providers.slice()
                    aiStatus.text = "未验证"
                }
                function normalizeBaseUrl(value) {
                    let result = value.trim().replace(/\/+$/, "")
                    if (!result.endsWith("/v1")) result += "/v1"
                    return result
                }
                function refreshAsrModels(preferred) {
                    preferredModel = preferred || ""
                    modelLoading = true
                    ++settingsHost.modelRequestId
                    controller.requestAsrModels(asrProvider.currentValue || "dashscope", "",
                                                settingsHost.modelRequestId)
                }
                function refreshAiModels(preferred) {
                    const provider = selectedProvider()
                    const models = provider && provider.modelIds ? provider.modelIds : []
                    aiModel.model = models
                    aiModel.currentIndex = Math.max(0, models.indexOf(preferred || (provider ? provider.selectedModel : "")))
                    aiCredentialStatus.text = provider
                        ? controller.requestCredentialStatus("SubCue/AI/" + provider.id, ++settingsHost.aiCredentialRequest) : "尚未配置"
                    aiStatus.text = provider && provider.verification && provider.verification.verifiedAtUtc
                        ? controller.verificationStatus("ai", provider.id) : "未验证"
                }
                function loadValues() {
                    saveStatus.text = ""
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

                    aiAssist.checked = controller.setting("aiAssistEnabled")
                    providers = clone(controller.aiProviders())
                    aiProvider.currentIndex = indexById(providers, controller.setting("aiProviderId") || "")
                    refreshAiModels("")

                    fontFamily.text = controller.setting("fontFamily") || "Microsoft YaHei"
                    fontSize.value = controller.setting("fontSize1080p") || 52
                    alignment.currentIndex = controller.setting("alignment") === "bottom-left" ? 0
                        : (controller.setting("alignment") === "bottom-right" ? 2 : 1)
                    bottomMargin.value = controller.setting("bottomMargin1080p") || 90
                    outputSrt.checked = controller.setting("outputSrt")
                    outputAss.checked = controller.setting("outputAss")
                    outputDirectory.text = controller.setting("outputDirectory") || ""
                    asrKey.clear(); aiKey.clear()
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
                        const provider = settingsWindow.selectedProvider()
                        if (provider && requestId === aiCredentialRequest && id === "SubCue/AI/" + provider.id) aiCredentialStatus.text = status
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
                    function onAiConnectionTestFinished(providerId, result) {
                        settingsWindow.aiOperationBusy = false
                        const provider = settingsWindow.selectedProvider()
                        if (!provider || provider.id !== providerId) return
                        if (result.success) {
                            provider.modelIds = result.models
                            if (!provider.selectedModel || result.models.indexOf(provider.selectedModel) < 0)
                                provider.selectedModel = result.models[0]
                            provider.verification = {
                                "providerId": provider.id,
                                "configRevision": provider.configRevision || 0,
                                "credentialRevision": (provider.credentialRevision || 0) + (aiKey.text.length > 0 ? 1 : 0),
                                "selectedModel": provider.selectedModel,
                                "verifiedAtUtc": result.verifiedAtUtc
                            }
                            settingsWindow.providers = settingsWindow.providers.slice()
                            settingsWindow.refreshAiModels(provider.selectedModel)
                            aiStatus.text = "已验证"
                        } else {
                            settingsWindow.invalidateAi(false)
                            aiStatus.text = result.error
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
                                        refreshAsrModels("fun-asr-flash-2026-06-15")
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
                                SectionTitle { text: "AI 辅助" }
                                FieldLabel { text: "启用 AI" }
                                SubCheckBox { id: aiAssist; text: "复核低置信度字幕"; Layout.fillWidth: true; enabled: !settingsWindow.aiOperationBusy }
                                Item { Layout.fillWidth: true }

                                FieldLabel { text: "Provider"; visible: aiAssist.checked }
                                SubComboBox {
                                    id: aiProvider
                                    visible: aiAssist.checked
                                    enabled: !settingsWindow.aiOperationBusy
                                    Layout.fillWidth: true
                                    model: providers
                                    textRole: "name"
                                    valueRole: "id"
                                    onActivated: {
                                        aiKey.clear()
                                        refreshAiModels("")
                                    }
                                }
                                RowLayout {
                                    visible: aiAssist.checked
                                    spacing: 4
                                    SubButton {
                                        text: "新增"
                                        enabled: !settingsWindow.aiOperationBusy
                                        onClicked: {
                                            editingProviderIndex = -1
                                            editingProviderKind = "openai"
                                            providerName.text = ""
                                            providerUrl.text = ""
                                            providerAuth.currentIndex = 0
                                            providerDialog.open()
                                        }
                                    }
                                    SubButton {
                                        text: "添加 Qwen"
                                        enabled: !settingsWindow.aiOperationBusy
                                        onClicked: {
                                            editingProviderIndex = -1
                                            editingProviderKind = "qwen"
                                            providerName.text = "阿里云百炼 Qwen"
                                            providerUrl.text = "https://dashscope.aliyuncs.com/compatible-mode/v1"
                                            providerAuth.currentIndex = 0
                                            providerDialog.open()
                                        }
                                    }
                                    SubButton {
                                        text: "编辑"
                                        enabled: !settingsWindow.aiOperationBusy && selectedProvider() !== null
                                        onClicked: {
                                            editingProviderIndex = aiProvider.currentIndex
                                            const provider = selectedProvider()
                                            editingProviderKind = provider.kind || "openai"
                                            providerName.text = provider.name
                                            providerUrl.text = provider.baseUrl
                                            providerAuth.currentIndex = provider.authMode === "none" ? 1 : 0
                                            providerDialog.open()
                                        }
                                    }
                                    SubButton { text: "删除"; enabled: !settingsWindow.aiOperationBusy && selectedProvider() !== null; onClicked: deleteDialog.open() }
                                }

                                FieldLabel { text: "模型"; visible: aiAssist.checked }
                                SubComboBox {
                                    id: aiModel
                                    visible: aiAssist.checked
                                    Layout.fillWidth: true
                                    enabled: !settingsWindow.aiOperationBusy && !!model && model.length > 0
                                    onActivated: {
                                        const provider = selectedProvider()
                                        if (provider) { provider.selectedModel = currentText; invalidateAi() }
                                    }
                                }
                                SubButton {
                                    text: "测试连接"
                                    visible: aiAssist.checked
                                    enabled: !settingsWindow.aiOperationBusy && selectedProvider() !== null
                                    onClicked: {
                                        const provider = selectedProvider()
                                        settingsWindow.aiOperationBusy = true
                                        aiStatus.text = "正在测试…"
                                        controller.testAiConnection(provider, aiKey.text)
                                    }
                                }

                                FieldLabel { text: "API Key"; visible: aiAssist.checked && selectedProvider() !== null && selectedProvider().authMode !== "none" }
                                SubTextField {
                                    id: aiKey
                                    visible: aiAssist.checked && selectedProvider() !== null && selectedProvider().authMode !== "none"
                                    enabled: !settingsWindow.aiOperationBusy
                                    Layout.fillWidth: true
                                    echoMode: TextInput.Password
                                    placeholderText: selectedProvider() && selectedProvider().kind === "qwen"
                                        ? "支持 sk-ws-… / sk-…；留空保留已保存密钥"
                                        : "留空保持当前 Provider 的密钥"
                                    onTextEdited: invalidateAi(false)
                                }
                                StatusLabel { id: aiCredentialStatus; visible: aiKey.visible }

                                FieldLabel { text: "连接状态"; visible: aiAssist.checked }
                                StatusLabel { id: aiStatus; visible: aiAssist.checked; Layout.fillWidth: true }
                                Item { visible: aiAssist.checked; Layout.fillWidth: true }

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
                                        const provider = selectedProvider()
                                        if (provider && aiModel.currentIndex >= 0)
                                            provider.selectedModel = aiModel.currentText
                                        const ok = controller.saveSettings({
                                            "asrProvider": asrProvider.currentValue,
                                            "asrModel": asrProvider.currentValue === "dashscope" ? asrModel.currentValue : controller.setting("asrModel"),
                                            "region": region.currentText,
                                            "asrApiHost": asrApiHost.text,
                                            "asrVerification": asrVerification,
                                            "asrConfigRevision": asrConfigRevision,
                                            "asrCredentialRevision": asrCredentialRevision,
                                            "aiAssistEnabled": aiAssist.checked,
                                            "aiProviderId": provider ? provider.id : "",
                                            "aiProviders": providers,
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

                Dialog {
                    id: providerDialog
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    modal: true
                    title: editingProviderIndex < 0 ? "新增 AI Provider" : "编辑 AI Provider"
                    width: Math.min(520, settingsWindow.width - 48)
                    standardButtons: Dialog.Ok | Dialog.Cancel
                    background: Rectangle { color: Theme.panelRaised; border.color: Theme.border }
                    contentItem: GridLayout {
                        columns: 2
                        rowSpacing: 10
                        columnSpacing: 10
                        Label { text: "名称"; color: Theme.text }
                        SubTextField { id: providerName; Layout.fillWidth: true }
                        Label { text: "Base URL"; color: Theme.text }
                        SubTextField { id: providerUrl; Layout.fillWidth: true; placeholderText: "https://example.com/v1" }
                        Item { visible: editingProviderKind === "qwen" }
                        Label {
                            visible: editingProviderKind === "qwen"
                            Layout.fillWidth: true
                            color: Theme.muted
                            wrapMode: Text.WordWrap
                            text: "如控制台提供业务空间专属 API Host，请在此替换默认地址。"
                        }
                        Label { text: "认证方式"; color: Theme.text }
                        SubComboBox { id: providerAuth; model: ["Bearer API Key", "无需认证"]; Layout.fillWidth: true }
                    }
                    onAccepted: {
                        const item = {
                            "id": editingProviderIndex < 0 ? controller.newProviderId() : providers[editingProviderIndex].id,
                            "kind": editingProviderKind,
                            "name": providerName.text.trim(),
                            "baseUrl": normalizeBaseUrl(providerUrl.text),
                            "authMode": providerAuth.currentIndex === 1 ? "none" : "bearer",
                            "configRevision": editingProviderIndex < 0 ? 0 : (providers[editingProviderIndex].configRevision || 0) + 1,
                            "credentialRevision": editingProviderIndex < 0 ? 0 : (providers[editingProviderIndex].credentialRevision || 0),
                            "modelIds": editingProviderIndex < 0 ? [] : (providers[editingProviderIndex].modelIds || []),
                            "selectedModel": editingProviderIndex < 0 ? "" : (providers[editingProviderIndex].selectedModel || ""),
                            "verification": {}
                        }
                        if (!item.name || !item.baseUrl) return
                        if (editingProviderIndex < 0) {
                            providers.push(item)
                            providers = providers.slice()
                            aiProvider.currentIndex = providers.length - 1
                        } else {
                            providers[editingProviderIndex] = item
                            providers = providers.slice()
                            aiProvider.currentIndex = editingProviderIndex
                        }
                        aiKey.clear()
                        refreshAiModels("")
                    }
                }

                Dialog {
                    id: deleteDialog
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    modal: true
                    title: "删除 AI Provider"
                    standardButtons: Dialog.Yes | Dialog.No
                    Label { text: "删除后保存设置时，将同时移除该 Provider 的 API Key。"; color: Theme.text }
                    onAccepted: {
                        providers.splice(aiProvider.currentIndex, 1)
                        providers = providers.slice()
                        aiProvider.currentIndex = providers.length > 0 ? 0 : -1
                        aiKey.clear()
                        refreshAiModels("")
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
