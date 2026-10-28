import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel
    required property var cloud
    property string accountId: ""
    property string modelId: ""
    property int tab: 0
    property bool detail: false
    property bool adding: false
    property bool manualModel: false
    property bool replacing: false
    property bool reveal: false
    property string query: ""
    property var discoveredSelection: []
    readonly property bool compact: width < 680
    readonly property bool pending: secret.text.length > 0 || (adding && remoteModel.text.length > 0)
    readonly property var account: cloud.accounts.find(row => row.id === accountId) || ({})
    readonly property var selectedModel: cloud.models.find(row => row.id === modelId) || ({})
    readonly property var vendors: [{id: "bailian", name: "阿里云百炼"}, {id: "deepseek", name: "DeepSeek"}, {id: "moonshot", name: "Moonshot"}]
    readonly property var adapters: [{id: "compatible-text", name: "文本"}, {id: "dashscope-asr", name: "语音识别"}, {id: "qwen-omni", name: "音频理解"}, {id: "qwen-tts", name: "语音合成"}]
    readonly property var tasks: [{id: "speechRecognition", name: "语音识别", description: "字幕打轴与粗剪识别共用"}, {id: "subtitleWordMapping", name: "字幕时间映射复核", description: "手动触发，使用文本及词级证据"}, {id: "subtitleAudioReview", name: "字幕听音复核", description: "需要音频输入与结构化结果"}, {id: "roughcutAudioReview", name: "粗剪听音复核", description: "需要音频输入与结构化结果"}, {id: "speechSynthesis", name: "语音合成", description: "需要兼容的 Workspace 与音色"}]
    readonly property var filteredAccounts: cloud.accounts.filter(row => (row.name + " " + row.vendor).toLowerCase().includes(query))
    readonly property var accountModels: cloud.models.filter(row => (!accountFilter.currentValue || row.accountId === accountFilter.currentValue) && (!kindFilter.currentValue || row.adapter === kindFilter.currentValue) && (row.name + " " + row.remoteId + " " + ownerName(row.accountId)).toLowerCase().includes(query))
    spacing: 12
    component Field: SubTextField { implicitHeight: 32; font.pixelSize: 13; selectByMouse: true }
    component Command: SubButton { implicitHeight: 32; font.pixelSize: 13; leftPadding: 12; rightPadding: 12 }
    component Hint: Label { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; color: Theme.secondaryText; font.pixelSize: 12 }
    component Caption: Label { color: Theme.secondaryText; font.pixelSize: 12; textFormat: Text.PlainText }
    component Result: TextEdit {
        required property string targetId
        readonly property var result: panel.cloud.tests[targetId] || ({})
        Layout.fillWidth: true; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText; font.pixelSize: 12
        color: result.state === "failed" ? Theme.error : result.state === "passed" ? Theme.success : Theme.secondaryText
        text: result.message || qsTr("未验证 · 测试不会保存设置")
    }
    function ownerName(id) { return (cloud.accounts.find(row => row.id === id) || {}).name || qsTr("账号已移除") }
    function stageInput() {
        if (secret.text !== "" && !cloud.stageKey(accountId, secret.text)) return false
        secret.clear(); reveal = false; replacing = false
        return true
    }
    function commitNewModel() {
        if (!adding || remoteModel.text.trim() === "") return true
        const id = cloud.addModel(accountId, remoteModel.text, adapter.currentValue)
        if (!id) return false
        modelId = id; remoteModel.clear(); adding = false
        return true
    }
    function prepareSave() { return stageInput() && commitNewModel() }
    function chooseAccount(id) {
        if (!stageInput()) return
        cloud.cancelDiscovery(); accountId = id; detail = true
    }
    function chooseModel(id) {
        if (!commitNewModel()) return
        cloud.cancelDiscovery(); modelId = id; adding = false; detail = true
    }
    function targetName(task) {
        const binding = cloud.bindings.find(row => row.taskId === task)
        if (!binding || !binding.target) return qsTr("未配置")
        if (binding.target.startsWith("local:")) return binding.target === "local:qwen3" ? "本地 Qwen3-ASR" : "本地 Fun-ASR"
        const model = cloud.models.find(row => row.id === binding.target)
        return model ? ownerName(model.accountId) + " / " + model.name : qsTr("原模型已移除")
    }
    function showTask() { if (stageInput()) tab = 2 }
    Component.onCompleted: { cloud.begin(); accountId = cloud.accounts.length ? cloud.accounts[0].id : "" }
    Component.onDestruction: cloud.discard()
    Timer { interval: 180; id: searchDelay; onTriggered: panel.query = search.text.trim().toLowerCase() }
    Timer { interval: 10000; running: panel.reveal; onTriggered: panel.reveal = false }
    Connections {
        target: panel.Window.window
        function onActiveChanged() { if (!panel.Window.window.active) panel.reveal = false }
    }
    Label { text: qsTr("AI 服务"); color: Theme.text; font.pixelSize: 18; font.bold: true }
    Hint { text: qsTr("添加账号 → 添加模型 → 绑定用途。所有资料在底部保存后生效。") }
    RowLayout {
        Layout.fillWidth: true; spacing: 4
        Repeater {
            model: [qsTr("账号"), qsTr("模型"), qsTr("用途绑定")]
            Command {
                required property int index; required property string modelData
                objectName: "cloudTab" + index; text: modelData; highlighted: panel.tab === index
                background: Rectangle { color: parent.highlighted ? Theme.selection : "transparent"; radius: Theme.radiusButton; border.width: parent.visualFocus ? 1 : 0; border.color: Theme.focusBorder }
                onClicked: { if (!panel.stageInput()) return; panel.tab = index; panel.detail = false; search.clear() }
            }
        }
        Item { Layout.fillWidth: true }
    }
    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider }
    RowLayout {
        visible: panel.tab < 2; Layout.fillWidth: true; spacing: 8
        Field { id: search; Layout.fillWidth: true; placeholderText: panel.tab === 0 ? qsTr("搜索账号") : qsTr("搜索名称、模型 ID 或账号"); onTextEdited: searchDelay.restart(); onTextChanged: if (text === "") panel.query = "" }
        Command { text: panel.tab === 0 ? qsTr("新增账号") : qsTr("添加模型"); objectName: "addCloudEntryButton"; enabled: panel.tab === 0 || cloud.accounts.length > 0; onClicked: { if (!panel.stageInput()) return; if (panel.tab === 0) newAccount.open(); else { panel.adding = true; panel.detail = true } } }
    }
    RowLayout {
        visible: panel.tab === 1; Layout.fillWidth: true; spacing: 8
        SubComboBox { id: accountFilter; Layout.fillWidth: true; implicitHeight: 32; model: [{id: "", name: qsTr("全部账号")}].concat(panel.cloud.accounts); textRole: "name"; valueRole: "id" }
        SubComboBox { id: kindFilter; Layout.fillWidth: true; implicitHeight: 32; model: [{id: "", name: qsTr("全部能力")}].concat(panel.adapters); textRole: "name"; valueRole: "id" }
    }
    RowLayout {
        visible: panel.tab < 2; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16
        ColumnLayout {
            visible: !panel.compact || !panel.detail; Layout.fillHeight: true
            Layout.preferredWidth: panel.compact ? panel.width : Math.max(208, panel.width * 0.38)
            Layout.fillWidth: panel.compact; spacing: 8
            ListView {
                id: entries; objectName: "cloudEntryList"; Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; reuseItems: true; boundsBehavior: Flickable.StopAtBounds
                model: panel.tab === 0 ? panel.filteredAccounts : panel.accountModels
                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width; height: 64; leftPadding: 10; rightPadding: 12
                    highlighted: panel.tab === 0 ? panel.accountId === modelData.id : panel.modelId === modelData.id
                    background: Rectangle { color: parent.highlighted ? Theme.selection : parent.hovered ? Theme.listHover : "transparent"; radius: Theme.radiusButton; border.width: parent.visualFocus ? 1 : 0; border.color: Theme.focusBorder }
                    contentItem: Column {
                        spacing: 5
                        Label { textFormat: Text.PlainText; width: parent.width; color: Theme.text; font.pixelSize: 13; text: modelData.name + (modelData.enabled === false ? qsTr(" · 已停用") : ""); elide: Text.ElideRight }
                        Label { textFormat: Text.PlainText; width: parent.width; color: Theme.secondaryText; font.pixelSize: 12; elide: Text.ElideRight; text: panel.tab === 0 ? ((panel.vendors.find(row => row.id === modelData.vendor) || {}).name || modelData.vendor) + " · " + modelData.keyState : panel.ownerName(modelData.accountId) + " · " + (panel.adapters.find(row => row.id === modelData.adapter) || {}).name }
                    }
                    ToolTip.visible: hovered; ToolTip.text: modelData.name + (modelData.remoteId && modelData.remoteId !== modelData.name ? "\n" + modelData.remoteId : "")
                    onClicked: panel.tab === 0 ? panel.chooseAccount(modelData.id) : panel.chooseModel(modelData.id)
                }
                ScrollBar.vertical: SubScrollBar {}
                Label { anchors.centerIn: parent; width: parent.width - 16; text: panel.cloud.accounts.length === 0 ? qsTr("添加第一个账号，安全保存 API Key。") : qsTr("没有匹配条目，试试其他筛选或添加模型。"); visible: entries.count === 0; wrapMode: Text.Wrap; color: Theme.secondaryText; font.pixelSize: 13 }
            }
            RowLayout {
                visible: panel.tab === 0; Layout.fillWidth: true
                Command { text: qsTr("上移"); enabled: !!panel.account.id; onClicked: cloud.moveAccount(panel.accountId, -1) }
                Command { text: qsTr("下移"); enabled: !!panel.account.id; onClicked: cloud.moveAccount(panel.accountId, 1) }
                Item { Layout.fillWidth: true }
            }
        }
        Rectangle { visible: !panel.compact; Layout.fillHeight: true; implicitWidth: 1; color: Theme.divider }
        ColumnLayout {
            visible: !panel.compact || panel.detail; Layout.fillHeight: true; Layout.fillWidth: true; spacing: 8
            Command { visible: panel.compact; text: qsTr("返回列表"); onClicked: { if (panel.stageInput() && panel.commitNewModel()) panel.detail = false } }
            ScrollView {
                id: detailScroll; objectName: "cloudDetailScroll"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: detailScroll.availableWidth; spacing: 12
                    ColumnLayout {
                        visible: panel.tab === 0 && !!panel.account.id; Layout.fillWidth: true; spacing: 8
                        Label { text: qsTr("账号资料"); color: Theme.text; font.pixelSize: 16 }
                        Caption { text: qsTr("账号名称") }
                        Field { objectName: "cloudAccountName"; Layout.fillWidth: true; text: panel.account.name || ""; onTextEdited: cloud.editAccount(panel.accountId, {name: text}) }
                        RowLayout {
                            Layout.fillWidth: true
                            Caption { text: (panel.vendors.find(row => row.id === panel.account.vendor) || {}).name || ""; Layout.fillWidth: true }
                            SubComboBox { visible: panel.account.vendor === "bailian"; enabled: !panel.account.existing; model: ["beijing", "singapore"]; currentIndex: panel.account.region === "singapore" ? 1 : 0; onActivated: cloud.editAccount(panel.accountId, {region: currentText}); implicitHeight: 32 }
                            SubCheckBox { text: qsTr("启用"); checked: panel.account.enabled !== false; onToggled: cloud.editAccount(panel.accountId, {enabled: checked}) }
                        }
                        Caption { visible: panel.account.vendor === "bailian"; text: qsTr("业务空间（首次需要时填写）") }
                        Field { visible: panel.account.vendor === "bailian"; Layout.fillWidth: true; text: panel.account.workspace || ""; placeholderText: qsTr("从百炼控制台复制一次，同账号模型共用"); onTextEdited: cloud.editAccount(panel.accountId, {workspace: text}) }
                        Hint { visible: panel.account.vendor === "bailian"; text: qsTr("无法从 Key 可靠推断业务空间。北京原生接口需要此值，保存后无需为每个模型重复输入。"); }
                        Command { visible: panel.account.vendor === "bailian"; text: qsTr("打开官方配置指引"); onClicked: Qt.openUrlExternally("https://help.aliyun.com/zh/model-studio/first-api-call-to-qwen") }
                        Caption { text: qsTr("备注") }
                        Field { Layout.fillWidth: true; text: panel.account.notes || ""; placeholderText: qsTr("例如：工作账号"); onTextEdited: cloud.editAccount(panel.accountId, {notes: text}) }
                        Hint { text: qsTr("官方域名：") + (panel.account.vendor === "deepseek" ? "api.deepseek.com" : panel.account.vendor === "moonshot" ? "api.moonshot.cn" : panel.account.region === "singapore" ? "dashscope-intl.aliyuncs.com" : "dashscope.aliyuncs.com") }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider; Layout.topMargin: 4; Layout.bottomMargin: 4 }
                        Caption { text: "API Key" }
                        Label { objectName: "cloudKeyMask"; color: Theme.text; font.pixelSize: 13; text: (panel.account.keyState === "未配置" ? "" : "••••••••  ·  ") + (panel.account.keyState || qsTr("未配置")) }
                        RowLayout {
                            Layout.fillWidth: true
                            Command { objectName: "replaceCloudKeyButton"; text: panel.account.keyState === "未配置" ? qsTr("添加密钥") : qsTr("更换密钥"); onClicked: { panel.replacing = true; secret.forceActiveFocus() } }
                            Command { text: qsTr("删除密钥"); enabled: panel.account.keyState !== "未配置"; onClicked: deleteKey.open() }
                        }
                        ColumnLayout {
                            visible: panel.replacing; Layout.fillWidth: true; spacing: 8
                            Field { id: secret; objectName: "cloudNewKeyField"; Layout.fillWidth: true; maximumLength: 4096; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhLatinOnly; placeholderText: qsTr("仅输入新密钥"); echoMode: panel.reveal ? TextInput.Normal : TextInput.Password; onActiveFocusChanged: if (!activeFocus) panel.reveal = false }
                            RowLayout {
                                Command { text: panel.reveal ? qsTr("隐藏") : qsTr("临时显示"); onClicked: panel.reveal = !panel.reveal }
                                Command { text: qsTr("暂存"); onClicked: panel.stageInput() }
                                Command { text: qsTr("取消"); onClicked: { secret.clear(); panel.reveal = false; panel.replacing = false } }
                            }
                        }
                        Command { visible: panel.account.keyState === "待替换" || panel.account.keyState === "待删除"; text: qsTr("撤回密钥更改"); onClicked: cloud.keepKey(panel.accountId) }
                        Hint { text: qsTr("旧密钥不会回填。圆圈只遮挡显示，密钥由 Windows 凭据管理器保存。") }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider; Layout.topMargin: 4; Layout.bottomMargin: 4 }
                        RowLayout {
                            Command { objectName: "testCloudConnectionButton"; text: qsTr("连接并选择模型"); enabled: !cloud.busy; onClicked: if (panel.stageInput()) { panel.discoveredSelection = []; cloud.discoverModels(panel.accountId); discovery.open() } }
                            Command { text: qsTr("取消测试"); visible: cloud.busy; onClicked: cloud.cancelDiscovery() }
                        }
                        Result { targetId: panel.accountId }
                        Hint { text: qsTr("只读取模型目录，不生成内容；选择模型后在底部统一保存。") }
                        Command { text: qsTr("删除账号…"); onClicked: deleteAccount.open(); Layout.topMargin: 8 }
                    }
                    ColumnLayout {
                        visible: panel.tab === 1 && (panel.adding || !!panel.selectedModel.id); Layout.fillWidth: true; spacing: 8
                        Label { text: panel.adding ? qsTr("添加模型") : qsTr("模型详情"); color: Theme.text; font.pixelSize: 16 }
                        Caption { visible: panel.adding; text: qsTr("所属账号") }
                        SubComboBox { id: modelOwner; visible: panel.adding; Layout.fillWidth: true; model: cloud.accounts; textRole: "name"; valueRole: "id"; currentIndex: model.findIndex(row => row.id === panel.accountId); onActivated: panel.chooseAccount(currentValue) }
                        Hint { visible: !panel.adding; text: panel.ownerName(panel.selectedModel.accountId) }
                        Command { objectName: "discoverCloudModelsButton"; visible: panel.adding; text: qsTr("获取官方列表…"); enabled: !cloud.busy && !!panel.accountId; onClicked: { if (!panel.stageInput()) return; panel.discoveredSelection = []; discoverySearch.clear(); cloud.discoverModels(panel.accountId); discovery.open() } }
                        SubButton { visible: panel.adding; text: qsTr("高级：手动添加模型"); checkable: true; checked: panel.manualModel; onToggled: panel.manualModel = checked }
                        Caption { visible: panel.manualModel || !panel.adding; text: qsTr("实际模型 ID（高级详情）") }
                        Field { id: remoteModel; objectName: "cloudRemoteModelId"; visible: panel.adding && panel.manualModel; Layout.fillWidth: true; placeholderText: qsTr("填写厂商提供的模型 ID") }
                        Field { visible: !panel.adding; readOnly: true; selectByMouse: true; Layout.fillWidth: true; text: panel.selectedModel.remoteId || "" }
                        Caption { visible: panel.adding && panel.manualModel; text: qsTr("协议能力") }
                        SubComboBox { id: adapter; visible: panel.adding && panel.manualModel; Layout.fillWidth: true; model: panel.account.vendor === "bailian" ? panel.adapters : [panel.adapters[0]]; textRole: "name"; valueRole: "id" }
                        Command { objectName: "addCloudModelButton"; visible: panel.adding && panel.manualModel; text: qsTr("添加到模型列表"); enabled: remoteModel.text.trim() !== "" && !!panel.accountId; onClicked: panel.commitNewModel() }
                        ColumnLayout {
                            visible: !panel.adding; Layout.fillWidth: true; spacing: 8
                            Caption { text: qsTr("显示名称") }
                            Field { Layout.fillWidth: true; text: panel.selectedModel.name || ""; onTextEdited: cloud.editModel(panel.modelId, {name: text}) }
                            Hint { text: qsTr("协议：") + ((panel.adapters.find(row => row.id === panel.selectedModel.adapter) || {}).name || "") }
                            Caption { visible: panel.selectedModel.adapter === "qwen-tts" && !!panel.selectedModel.workspace; text: qsTr("旧模型独立业务空间（可清空改用账号值）") }
                            Field { visible: panel.selectedModel.adapter === "qwen-tts" && !!panel.selectedModel.workspace; Layout.fillWidth: true; text: panel.selectedModel.workspace || ""; onTextEdited: cloud.editModel(panel.modelId, {workspace: text}) }
                            SubCheckBox { text: qsTr("启用模型"); checked: panel.selectedModel.enabled !== false; onToggled: cloud.editModel(panel.modelId, {enabled: checked}) }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider; Layout.topMargin: 4; Layout.bottomMargin: 4 }
                            Field { id: testVoice; visible: panel.selectedModel.adapter === "qwen-tts"; Layout.fillWidth: true; placeholderText: qsTr("测试 Voice ID，不会关联工程") }
                            Command { objectName: "testCloudCapabilityButton"; text: qsTr("测试任务能力…"); enabled: !cloud.busy; onClicked: capability.open() }
                            Command { visible: cloud.busy; text: qsTr("取消测试"); onClicked: cloud.cancelDiscovery() }
                            Result { targetId: panel.modelId }
                            Hint { text: qsTr("手动添加的未知模型需通过样例协议测试。测试不修改工程，也不代表真实内容质量。") }
                            Command { text: qsTr("前往用途绑定"); onClicked: panel.tab = 2 }
                            Command { text: qsTr("移除模型…"); onClicked: deleteModel.open(); Layout.topMargin: 8 }
                        }
                    }
                    Hint { visible: (panel.tab === 0 && !panel.account.id) || (panel.tab === 1 && !panel.adding && !panel.selectedModel.id); text: qsTr("从左侧选择条目查看详情，或添加新条目。") }
                    Item { implicitHeight: 8 }
                }
            }
        }
    }
    ScrollView {
        id: bindingScroll; visible: panel.tab === 2; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: bindingScroll.availableWidth; spacing: 16
            Repeater {
                model: panel.tasks
                ColumnLayout {
                    required property var modelData
                    readonly property var binding: cloud.bindings.find(row => row.taskId === modelData.id) || ({})
                    readonly property var choices: [{id: "", name: qsTr("未配置")}].concat(cloud.taskChoices(modelData.id))
                    readonly property bool unavailable: !!binding.target && !choices.some(row => row.id === binding.target)
                    Layout.fillWidth: true; spacing: 8
                    Label { text: modelData.name; color: Theme.text; font.pixelSize: 16 }
                    Hint { text: modelData.description }
                    Hint { visible: parent.unavailable; text: qsTr("原选择：") + panel.targetName(parent.modelData.id); color: Theme.warning }
                    SubComboBox { Layout.fillWidth: true; implicitHeight: 32; model: parent.choices; textRole: "name"; valueRole: "id"; currentIndex: parent.unavailable ? -1 : model.findIndex(row => row.id === parent.binding.target); displayText: currentIndex < 0 ? qsTr("原选择不可用，请检查") : currentText; onActivated: cloud.bindModel(parent.modelData.id, currentValue) }
                    Hint { text: { cloud.accounts; cloud.models; cloud.bindings; cloud.tests; return cloud.taskStatus(parent.modelData.id) } }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.divider }
                }
            }
            Hint { text: qsTr("设置内的选择统一保存后生效；工作区快速切换立即保存，只影响之后启动的任务。") }
            RowLayout { Command { text: qsTr("管理账号"); onClicked: panel.tab = 0 } Command { text: qsTr("管理模型"); onClicked: panel.tab = 1 } }
        }
    }
    Hint { objectName: "cloudSettingsStatus"; text: panel.cloud.status; visible: text !== ""; color: Theme.warning }
    Dialog {
        id: newAccount; parent: Overlay.overlay; modal: true; title: qsTr("新增账号"); standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout { width: 280; Caption { text: qsTr("选择厂商，账号资料和密钥在详情中编辑。") } SubComboBox { id: vendor; Layout.fillWidth: true; model: panel.vendors; textRole: "name"; valueRole: "id" } }
        onAccepted: panel.chooseAccount(cloud.addAccount(vendor.currentValue))
    }
    Dialog {
        id: capability; width: Math.min(420, panel.Window.window.width - 40); parent: Overlay.overlay; modal: true; title: qsTr("发送任务能力测试？"); standardButtons: Dialog.Ok | Dialog.Cancel
        Hint { width: Math.min(380, panel.width - 40); text: panel.selectedModel.adapter === "qwen-tts" ? qsTr("向所选账号发送“连接测试。”及测试 Voice ID，生成并检查短音频后删除临时结果。可能计费（软预算 1 元，非账单上限），不会保存配置或修改工程。失败或结果未知不自动重发。") : qsTr("向所选账号发送内置最小样例：文本为固定协议探针；音频为 0.1 秒测试音。可能计费，不上传工程内容。检查响应协议与结构化结果，ASR 需返回词级信息；无识别内容不算通过。测试不会保存配置，失败不自动重发。") }
        onAccepted: cloud.testCapability(panel.modelId, testVoice.text)
    }
    Dialog {
        id: discovery; parent: Overlay.overlay; modal: true; title: qsTr("从官方模型列表添加")
        width: Math.min(560, panel.Window.window.width - 40); height: Math.min(480, panel.Window.window.height - 80)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: standardButton(Dialog.Ok).text = qsTr("添加所选模型")
        onRejected: cloud.cancelDiscovery()
        ColumnLayout {
            anchors.fill: parent; spacing: 8
            Result { targetId: panel.accountId }
            Field { id: discoverySearch; Layout.fillWidth: true; placeholderText: qsTr("搜索模型名称") }
            ListView {
                Layout.fillHeight: true; Layout.fillWidth: true; clip: true; reuseItems: true
                model: cloud.discovered.filter(row => (row.name || row.id).toLowerCase().includes(discoverySearch.text.toLowerCase()))
                delegate: SubCheckBox { required property var modelData; width: ListView.view.width; text: (modelData.name || modelData.id) + " · " + (modelData.source || qsTr("目录可见 / 权限未验证")); checked: panel.discoveredSelection.includes(modelData.id); onToggled: panel.discoveredSelection = checked ? panel.discoveredSelection.concat([modelData.id]) : panel.discoveredSelection.filter(id => id !== modelData.id) }
                ScrollBar.vertical: SubScrollBar {}
            }
            Hint { text: qsTr("已选 %1 个。添加时去重；可列出不代表所选协议受支持。").arg(panel.discoveredSelection.length) }
        }
        onAccepted: { for (const id of panel.discoveredSelection) panel.modelId = cloud.addModel(panel.accountId, id, (cloud.discovered.find(row => row.id === id) || {}).adapter || "compatible-text"); panel.adding = false; remoteModel.clear() }
    }
    Dialog {
        id: deleteAccount; parent: Overlay.overlay; modal: true; title: qsTr("删除账号？"); standardButtons: Dialog.Ok | Dialog.Cancel
        Hint { width: 300; text: qsTr("保存后删除该账号的凭据、模型与用途绑定。已有工程和音频保留。") }
        onAccepted: { secret.clear(); cloud.removeAccount(panel.accountId); panel.accountId = cloud.accounts.length ? cloud.accounts[0].id : ""; panel.modelId = "" }
    }
    Dialog {
        id: deleteKey; parent: Overlay.overlay; modal: true; title: qsTr("删除该账号密钥？"); standardButtons: Dialog.Ok | Dialog.Cancel
        Hint { width: 300; text: qsTr("保存后取消并等待该账号的任务退出，再删除本机密钥。不吊销云平台上的 Key。") }
        onAccepted: { secret.clear(); panel.replacing = false; cloud.removeKey(panel.accountId) }
    }
    Dialog {
        id: deleteModel; parent: Overlay.overlay; modal: true; title: qsTr("移除模型及其用途绑定？"); standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: { cloud.removeModel(panel.modelId); panel.modelId = "" }
    }
}
