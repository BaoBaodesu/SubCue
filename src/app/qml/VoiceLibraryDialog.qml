import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SubCue

Dialog {
    id: dialog


    property var activePage: null
    property bool advancedDetails: false

    property var controller
    property bool transcriptRequired: true
    onTranscriptRequiredChanged: if (controller) controller.setCreationTranscriptRequired(transcriptRequired)
    property var preview
    signal useVoice(string versionId, string displayName)
    onClosed: { if (preview) preview.clear(); if (controller) { controller.cancelCreation(); controller.cancelPreview(); if (controller.busy) controller.cancel(); } activePage = null }
    title: qsTr("声音库")
    modal: true
    width: Math.min(1120, parent ? parent.width - 32 : 1120)
    height: Math.min(820, parent ? parent.height - 32 : 820)
    standardButtons: Dialog.Close
    footer: DialogButtonBox { visible: !dialog.activePage; standardButtons: Dialog.Close; onRejected: dialog.close() }
    onOpened: if (controller) controller.refresh(controller.selectedProfile)
    Connections {
        target: controller
        function onAuditionRequested(path, sha256) { if (dialog.visible && preview) preview.play(path, sha256) }
        function onVoiceSelected(versionId, name) { if (dialog.visible) { typeTabs.currentIndex = 1; dialog.activePage = null } }
    }
    function openAdd() { open(); addVoicePage.open() }
    Shortcut {
        sequence: "Space"
        enabled: dialog.visible && preview && (preview.playing || preview.paused)
            && !(dialog.parent && dialog.parent.Window.window && dialog.parent.Window.window.activeFocusItem
                && typeof dialog.parent.Window.window.activeFocusItem.inputMethodComposing !== "undefined")
        onActivated: { if (preview.playing) preview.pause(); else preview.resume() }
    }
    function load(profileOffset, versionOffset) {
        if (controller) controller.refresh(controller.selectedProfile, controller.includeArchived, profileOffset, versionOffset)
    }
    FileDialog {
        id: referenceDialog
        title: qsTr("选择同一说话人的参考音频")
        nameFilters: [qsTr("音频 (*.wav *.flac *.mp3 *.m4a *.ogg)"), qsTr("所有文件 (*)")]
        onAccepted: referenceField.text = selectedFile.toString()
    }
    FileDialog {
        id: batchFiles
        title: qsTr("选择同一说话人的素材（最多 100 个）")
        fileMode: FileDialog.OpenFiles
        nameFilters: referenceDialog.nameFilters
        onAccepted: { if (preview) preview.stop(); batchDialog.open(); controller.prepareSamples(selectedFiles) }
    }
    contentItem: Item {
      ColumnLayout {
        anchors.fill: parent
        visible: !dialog.activePage
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("我的音色"); font.bold: true; color: Theme.text; Layout.fillWidth: true }
            PrimaryButton { objectName: "addVoiceButton"; text: qsTr("+ 添加音色"); enabled: controller && !controller.busy; onClicked: addVoicePage.open() }
            SubButton { text: qsTr("详情"); checkable: true; checked: dialog.advancedDetails; onToggled: dialog.advancedDetails = checked }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("选择音色，试听并确认后用于文稿。参考素材和历史版本在详情中管理。") }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true; Layout.minimumHeight: 80
            ColumnLayout {
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                TextField { id: nameField; objectName: "voiceProfileName"; Layout.fillWidth: true; visible: dialog.advancedDetails; placeholderText: qsTr("音色名称"); selectByMouse: true }
                RowLayout {
                    SubButton { visible: dialog.advancedDetails; text: qsTr("重命名"); enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: controller.renameProfile(nameField.text) }
                }
                SubCheckBox { text: qsTr("显示停用音色"); checked: controller ? controller.includeArchived : false; enabled: controller && !controller.busy; onToggled: controller.refresh(controller.selectedProfile, checked, 0, 0) }
                ListView {
                    id: profilesList
                    objectName: "voiceProfilesList"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true
                    model: controller ? controller.profiles : []
                    ScrollBar.vertical: SubScrollBar {}
                    delegate: ItemDelegate {
                        required property var modelData
                        width: profilesList.width
                        text: modelData.name + (modelData.archived ? qsTr("（已停用）") : "")
                        highlighted: controller && controller.selectedProfile === modelData.id
                        background: Rectangle { color: parent.highlighted ? Theme.selection : parent.hovered ? Theme.listHover : Theme.panel; border.color: parent.activeFocus ? Theme.accent : "transparent" }
                        enabled: controller && !controller.busy
                        onClicked: { nameField.text = modelData.name; controller.refresh(modelData.id, controller.includeArchived, controller.profileOffset, 0) }
                    }
                    Label { anchors.centerIn: parent; visible: profilesList.count === 0; color: Theme.secondaryText; text: qsTr("点击“+ 添加音色”开始") }
                }
                RowLayout {
                    SubButton { text: qsTr("上一页"); enabled: controller && !controller.busy && controller.profileOffset > 0; onClicked: dialog.load(Math.max(0, controller.profileOffset - 50), controller.versionOffset) }
                    SubButton { text: qsTr("下一页"); enabled: controller && !controller.busy && profilesList.count === 50; onClicked: dialog.load(controller.profileOffset + 50, controller.versionOffset) }
                }
                SubButton { text: qsTr("停用音色"); enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: archiveDialog.open() }
                SubButton { text: qsTr("恢复音色"); visible: controller && controller.includeArchived; enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: controller.restoreProfile() }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: Theme.divider }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                TabBar {
                    id: typeTabs
                    Layout.fillWidth: true
                    TabButton { text: qsTr("本地参考"); background: Rectangle { color: parent.checked ? Theme.selection : Theme.panel; border.color: parent.checked ? Theme.accent : Theme.border } }
                    TabButton { text: qsTr("通义云音色"); background: Rectangle { color: parent.checked ? Theme.selection : Theme.panel; border.color: parent.checked ? Theme.accent : Theme.border } }
                }
                ColumnLayout {
                    visible: typeTabs.currentIndex === 0 && dialog.advancedDetails
                    Layout.fillWidth: true
                    RowLayout {
                        TextField { id: referenceField; Layout.fillWidth: true; readOnly: true; placeholderText: qsTr("参考文件原件不覆盖") }
                        SubButton { text: qsTr("选择文件"); enabled: controller && !controller.busy; onClicked: referenceDialog.open() }
                    }
                    TextArea { id: transcriptField; visible: dialog.transcriptRequired; Layout.fillWidth: true; Layout.preferredHeight: 70; wrapMode: TextEdit.Wrap; placeholderText: qsTr("填写与参考音频一致的转写"); selectByMouse: true }
                    SubButton { text: qsTr("保存新的参考版本"); enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: controller.addLocalVersion(referenceField.text, transcriptField.text, dialog.transcriptRequired) }
                    SubButton { objectName: "prepareReferenceBatch"; text: qsTr("批量整理同一说话人素材…"); enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: batchFiles.open() }
                }
                TextField { id: libraryCloudSearch; visible: typeTabs.currentIndex === 1; Layout.fillWidth: true; placeholderText: qsTr("搜索云端音色名称"); selectByMouse: true }
                RowLayout {
                    visible: typeTabs.currentIndex === 1; Layout.fillWidth: true
                    SubButton { text: qsTr("刷新账号音色"); enabled: controller && !controller.busy; onClicked: controller.discoverVoices(true) }
                    SubComboBox { id: customVoice; Layout.fillWidth: true; textRole: "name"; model: controller ? (controller.voiceCatalog.rows || []).filter(function(row) { return row.compatible && row.name.toLowerCase().indexOf(libraryCloudSearch.text.toLowerCase()) >= 0 }) : [] }
                    SubButton { text: qsTr("添加到声音库"); enabled: controller && !controller.busy && customVoice.currentIndex >= 0; onClicked: controller.useDiscoveredVoice(customVoice.model[customVoice.currentIndex].id) }
                }
                Label { visible: typeTabs.currentIndex === 1 && controller && !!controller.voiceCatalog.message; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.voiceCatalog.message || "" : "" }
                RowLayout {
                    visible: typeTabs.currentIndex === 1; Layout.fillWidth: true
                    SubComboBox { id: systemVoice; Layout.fillWidth: true; textRole: "name"; model: controller ? controller.builtinVoices().filter(function(row) { return row.name.toLowerCase().indexOf(libraryCloudSearch.text.toLowerCase()) >= 0 }) : [] }
                    SubButton { text: qsTr("添加到声音库"); enabled: controller && !controller.busy && systemVoice.currentIndex >= 0; onClicked: controller.useBuiltinVoice(systemVoice.model[systemVoice.currentIndex].id) }
                    SubButton { text: qsTr("试听样本"); enabled: controller && !controller.busy && systemVoice.currentIndex >= 0; onClicked: controller.previewCloudVoice(systemVoice.model[systemVoice.currentIndex].id) }
                }
                Label { visible: typeTabs.currentIndex === 1; Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("官方中文目录 v2 · 2026-10-17 · 北京 Flash；权限未验证。试听使用官方现成音频，不调用合成。"); color: Theme.secondaryText }
                SubButton { objectName: "openCloudClone"; visible: typeTabs.currentIndex === 1; text: qsTr("创建云端音色／查看提交记录"); enabled: controller && !controller.busy; onClicked: { controller.reloadCloudClone(); cloudCloneDialog.open() } }
                RowLayout {
                    visible: typeTabs.currentIndex === 1
                    SubButton { visible: dialog.advancedDetails; objectName: "openCloudUpdate"; text: qsTr("更新云端音色…"); enabled: controller && !controller.busy; onClicked: { controller.reloadCloudClone(); cloudDeletionDialog.updating = true; deletionVoice.text = customVoice.currentIndex >= 0 ? customVoice.model[customVoice.currentIndex].id : controller.cloudClone.voiceId || ""; cloudDeletionDialog.open() } }
                    SubButton { visible: dialog.advancedDetails; objectName: "openCloudDeletion"; text: qsTr("删除云端自定义音色…"); enabled: controller && !controller.busy; onClicked: { controller.reloadCloudClone(); cloudDeletionDialog.updating = false; deletionVoice.text = customVoice.currentIndex >= 0 ? customVoice.model[customVoice.currentIndex].id : controller.cloudClone.voiceId || ""; cloudDeletionDialog.open() } }
                }
                RowLayout {
                    visible: typeTabs.currentIndex === 1
                    Layout.fillWidth: true
                    SubButton { visible: dialog.advancedDetails; text: qsTr("高级：关联已有 Voice ID"); checkable: true; id: manualCloudVoice }
                    TextField { visible: manualCloudVoice.checked; id: cloudVoiceField; Layout.fillWidth: true; placeholderText: qsTr("Flash 系统音色／已有 Voice ID，以官方列表为准"); selectByMouse: true }
                    SubButton { visible: manualCloudVoice.checked; text: qsTr("关联当前云账号"); enabled: controller && !controller.busy && controller.selectedProfile !== ""; onClicked: controller.addCloudVersion(cloudVoiceField.text) }
                }
                Label { visible: typeTabs.currentIndex === 1; text: qsTr("使用 AI 服务中语音合成用途的账号、模型与 Workspace；已有外部音色会查询兼容性。保存为新版本，旧版本和已有音频保留，不自动合成或上传。"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: dialog.advancedDetails ? qsTr("历史版本") : qsTr("可用参考"); font.bold: true; color: Theme.text }
                ListView {
                    id: versionsList
                    objectName: "voiceVersionsList"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    model: controller ? controller.versions.filter(function(row) { return typeTabs.currentIndex === 1 ? !!row.metadata.providerVoiceId : !row.metadata.providerVoiceId }) : []
                    clip: true; spacing: 6
                    ScrollBar.vertical: SubScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        width: versionsList.width
                        height: versionContent.implicitHeight + 16
                        color: Theme.panelSecondary
                        ColumnLayout {
                            id: versionContent
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            Label { Layout.fillWidth: true; color: Theme.text; text: modelData.metadata.providerVoiceId ? qsTr("云音色：%1").arg(modelData.metadata.providerVoiceId) : (modelData.metadata.qualityState === "approved" ? qsTr("本地参考 · 已审核") : qsTr("本地参考 · 待质检")); elide: Text.ElideRight }
                            Label { Layout.fillWidth: true; color: Theme.secondaryText; text: modelData.metadata.referenceText || modelData.metadata.model || ""; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight }
                            RowLayout {
                                Label { Layout.fillWidth: true; color: Theme.secondaryText; text: modelData.id.substring(0, 8) + (modelData.isDefault ? qsTr(" · 默认") : "") }
                                SubButton { text: qsTr("检查并审核"); visible: modelData.referencePath !== ""; enabled: controller && !controller.busy; onClicked: { reviewDialog.open(); controller.inspectVersion(modelData.id) } }
                                SubButton {
                                    text: qsTr("使用此音色"); enabled: controller && !controller.busy && (modelData.metadata.providerVoiceId || modelData.metadata.qualityState === "approved")
                                    onClicked: {
                                        let name = qsTr("项目音色")
                                        for (const profile of controller.profiles) if (profile.id === controller.selectedProfile) { name = profile.name; break }
                                        dialog.useVoice(modelData.id, name); dialog.close()
                                    }
                                }
                            }
                            SubButton { text: modelData.isDefault ? qsTr("当前默认版本") : qsTr("设为默认版本"); enabled: controller && !controller.busy && !modelData.isDefault && (!!modelData.metadata.providerVoiceId || modelData.metadata.qualityState === "approved"); onClicked: controller.setDefaultVersion(modelData.id) }
                            SubButton { text: modelData.metadata.preset ? qsTr("预设：%1（另存）").arg(modelData.metadata.preset.name) : qsTr("保存声音预设／使用范围…"); enabled: controller && !controller.busy; onClicked: { presetDialog.versionId = modelData.id; presetName.text = modelData.metadata.preset ? modelData.metadata.preset.name : ""; usageScope.text = modelData.metadata.usageScope || ""; presetDialog.open() } }
                        }
                    }
                }
                RowLayout {
                    SubButton { text: qsTr("较新版本"); enabled: controller && !controller.busy && controller.versionOffset > 0; onClicked: dialog.load(controller.profileOffset, Math.max(0, controller.versionOffset - 50)) }
                    SubButton { text: qsTr("较旧版本"); enabled: controller && !controller.busy && versionsList.count === 50; onClicked: dialog.load(controller.profileOffset, controller.versionOffset + 50) }
                }
            }
        }
        VoicePreviewTransport { sourceTitle: qsTr("声音库 · 当前样本"); player: preview; preparing: controller && controller.previewLoading; onStopRequested: if (controller) controller.cancelPreview() }
        RowLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.statusText : "" }
            SubButton { visible: controller && controller.busy; text: qsTr("取消"); onClicked: controller.cancel() }
            BusyIndicator { running: controller && controller.busy; visible: running; implicitWidth: 24; implicitHeight: 24 }
        }
    }
    }

    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: reviewDialog
        objectName: "voiceReferenceReviewDialog"
        title: qsTr("参考样本审核")
        modal: true
        standardButtons: Dialog.Close
        Connections {
            target: controller
            function onChanged() {
                if (controller.review.id && controller.review.id !== reviewDialog.versionId) {
                    reviewDialog.versionId = controller.review.id
                    reviewTranscript.text = controller.review.metadata.referenceText || ""
                    referenceLanguage.currentIndex = Math.max(0, referenceLanguage.values.indexOf(controller.review.metadata.referenceLanguage || "Chinese"))
                    referenceEmotion.currentIndex = Math.max(0, referenceEmotion.values.indexOf(controller.review.metadata.manualEmotion || "unclassified"))
                    derivedEmotion.currentIndex = 0
                    emotionReviewed.checked = false
                    acceptQuality.checked = false
                    cropStart.text = "0"
                    cropEnd.text = String(Math.min(30, Number(controller.review.quality.sampleCount) / controller.review.quality.sampleRate))
                    derivedReviewed.checked = false
                }
            }
        }
        property string versionId: ""
        onOpened: { versionId = ""; reviewTranscript.text = ""; acceptQuality.checked = false }
        onClosed: { if (preview) preview.stop(); if (controller && controller.busy) controller.cancel() }
        contentItem: ScrollView {
            id: reviewScroll
            objectName: "voiceEmotionReviewScroll"
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: reviewScroll.availableWidth
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.statusText : "" }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text; text: controller && controller.review.quality ? qsTr("采样率 %1 Hz · 峰值 %2 · RMS %3").arg(controller.review.quality.sampleRate).arg(controller.review.quality.peak.toFixed(3)).arg(controller.review.quality.rms.toFixed(3)) : "" }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller && controller.review.quality ? controller.review.quality.warnings.join(" ") : "" }
                RowLayout {
                    SubButton { objectName: "referencePreviewPlay"; text: qsTr("试听参考"); enabled: preview && controller && !!controller.review.id && !preview.loading; onClicked: preview.play(controller.review.referencePath, controller.review.referenceSha256) }
                    SubButton { text: qsTr("停止"); enabled: preview && (preview.playing || preview.loading); onClicked: preview.stop() }
                    Label { text: preview ? preview.statusText : ""; color: Theme.secondaryText; Layout.fillWidth: true; wrapMode: Text.Wrap }
                }
                TextArea { id: reviewTranscript; objectName: "referenceReviewTranscript"; Layout.fillWidth: true; Layout.preferredHeight: 100; selectByMouse: true; wrapMode: TextEdit.Wrap; placeholderText: dialog.transcriptRequired ? qsTr("试听后审核与参考一致的转写") : qsTr("当前引擎转写可选，仍需试听审核"); enabled: controller && !controller.busy && !!controller.review.id }
                SubCheckBox { id: acceptQuality; text: qsTr("已试听，并确认接受上述质量疑点"); enabled: controller && !controller.busy && !!controller.review.id }
                Label { text: qsTr("参考语言 · 旧样本默认中文，请核对"); color: Theme.secondaryText }
                SubComboBox { id: referenceLanguage; objectName: "voiceReferenceLanguage"; Layout.fillWidth: true; readonly property var values: ["Chinese", "English", "Japanese"]; model: [qsTr("中文"), qsTr("英语"), qsTr("日语")]; onActivated: derivedReviewed.checked = false }
                RowLayout {
                    Label { text: qsTr("人工情绪"); color: Theme.secondaryText }
                    SubComboBox { id: referenceEmotion; objectName: "voiceReferenceEmotion"; Layout.fillWidth: true; readonly property var values: ["unclassified", "angry", "disgusted", "fearful", "happy", "neutral", "other", "sad", "surprised", "unknown"]; model: [qsTr("未分类"), qsTr("愤怒"), qsTr("厌恶"), qsTr("恐惧"), qsTr("高兴"), qsTr("中性"), qsTr("其它"), qsTr("悲伤"), qsTr("惊讶"), qsTr("无法判断")]; enabled: controller && !controller.busy && !!controller.review.id }
                }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("人工标签仅描述参考音频，不自动设置合成情绪。模型建议不会覆盖人工标签。") }
                SubCheckBox { id: emotionEnabled; objectName: "voiceEmotionEnabled"; text: qsTr("启用本地音频情绪识别（实验，手动执行）"); enabled: controller && !controller.busy }
                TextField { id: emotionPython; objectName: "voiceEmotionPython"; visible: emotionEnabled.checked; Layout.fillWidth: true; placeholderText: qsTr("独立情绪识别环境 Python 可执行文件完整路径"); selectByMouse: true; enabled: controller && !controller.busy }
                TextField { id: emotionModel; objectName: "voiceEmotionModel"; visible: emotionEnabled.checked; Layout.fillWidth: true; placeholderText: qsTr("本地 emotion2vec+ Base 模型目录"); selectByMouse: true; enabled: controller && !controller.busy }
                SubButton { objectName: "recognizeReferenceEmotion"; visible: emotionEnabled.checked; text: qsTr("识别参考情绪"); enabled: controller && !controller.busy && !!controller.review.id; onClicked: { emotionReviewed.checked = false; controller.recognizeEmotion(emotionPython.text, emotionModel.text) } }
                Label {
                    objectName: "voiceEmotionPrediction"
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText
                    readonly property var prediction: controller && controller.review.metadata ? controller.review.metadata.emotionPrediction : null
                    text: prediction ? qsTr("模型建议：%1 · 分数 %2 · 差距 %3 · %4\n分数未经准确率校准；暂定审核阈值 0.60／差距 0.15。")
                        .arg(referenceEmotion.model[referenceEmotion.values.indexOf(prediction.label)])
                        .arg(Number(prediction.score).toFixed(3)).arg(Number(prediction.margin).toFixed(3))
                        .arg(prediction.requiresReview ? qsTr("低分／不明确，待人工审核") : qsTr("高分仍需人工审核")) : ""
                }
                SubCheckBox { id: emotionReviewed; objectName: "voiceEmotionReviewed"; text: qsTr("已试听并审核模型建议，人工标签以我的选择为准"); visible: controller && !!controller.review.metadata && !!controller.review.metadata.emotionPrediction; enabled: controller && !controller.busy }
                SubButton { text: qsTr("忽略当前模型建议，保留人工标签"); visible: emotionReviewed.visible; enabled: controller && !controller.busy; onClicked: { emotionReviewed.checked = false; controller.discardEmotionPrediction() } }
                SubButton { text: qsTr("保存新的审核版本"); enabled: controller && !controller.busy && !!controller.review.id; onClicked: controller.approveReview(reviewTranscript.text, acceptQuality.checked, referenceLanguage.values[referenceLanguage.currentIndex], referenceEmotion.values[referenceEmotion.currentIndex], emotionReviewed.checked, dialog.transcriptRequired) }
                Label { text: qsTr("派生处理 · 原件不覆盖"); font.bold: true; color: Theme.text }
                RowLayout {
                    Label { text: qsTr("起止秒"); color: Theme.secondaryText }
                    TextField { id: cropStart; text: "0"; Layout.preferredWidth: 90; validator: DoubleValidator { bottom: 0; locale: "C" } }
                    TextField { id: cropEnd; Layout.preferredWidth: 90; placeholderText: qsTr("最多 30 秒"); validator: DoubleValidator { bottom: 0; locale: "C" } }
                    SubCheckBox { id: dc; text: qsTr("去直流") }
                    SubCheckBox { id: denoise; text: qsTr("轻度降噪") }
                }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("派生样本为 24kHz 单声道。轻度降噪只保守衰减低电平区；请比较气口和尾音。裁切后重新核对上方转写。") }
                RowLayout {
                    SubButton { objectName: "deriveReferenceSample"; text: qsTr("准备派生样本"); enabled: controller && !controller.busy && !!controller.review.id; onClicked: { if (preview) preview.stop(); derivedReviewed.checked = false; derivedEmotion.currentIndex = 0; controller.deriveReview(Number(cropStart.text), Number(cropEnd.text), dc.checked, denoise.checked) } }
                    SubButton { text: qsTr("A 原样本"); enabled: preview && controller && !!controller.review.id && !preview.loading; onClicked: preview.play(controller.review.referencePath, controller.review.referenceSha256) }
                    SubButton { objectName: "previewDerivedReference"; text: qsTr("B 派生样本"); enabled: preview && controller && !!controller.derived.referencePath && !preview.loading; onClicked: preview.play(controller.derived.referencePath, controller.derived.referenceSha256) }
                }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller && controller.derived.quality ? controller.derived.quality.warnings.join(" ") : "" }
                RowLayout {
                    Label { text: qsTr("派生人工情绪"); color: Theme.secondaryText }
                    SubComboBox { id: derivedEmotion; objectName: "voiceDerivedEmotion"; Layout.fillWidth: true; model: referenceEmotion.model; enabled: controller && !controller.busy && !!controller.derived.referencePath; onActivated: derivedReviewed.checked = false }
                }
                SubCheckBox { id: derivedReviewed; text: qsTr("已 A/B 试听并重新核对裁切后的转写"); enabled: controller && !controller.busy && !!controller.derived.referencePath }
                SubButton { text: qsTr("保存新的派生审核版本"); enabled: controller && !controller.busy && !!controller.derived.referencePath && derivedReviewed.checked; onClicked: controller.approveDerived(reviewTranscript.text, acceptQuality.checked, derivedReviewed.checked, referenceLanguage.values[referenceLanguage.currentIndex], referenceEmotion.values[derivedEmotion.currentIndex], dialog.transcriptRequired) }
                SubButton { text: qsTr("取消检查"); visible: controller && controller.busy; onClicked: controller.cancel() }
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: batchDialog
        objectName: "voiceSampleBatchDialog"
        title: qsTr("同一说话人 · 样本整理预览")
        modal: true
        standardButtons: Dialog.Close
        onClosed: { if (preview) preview.stop(); if (controller && controller.busy) controller.cancel() }
        contentItem: ColumnLayout {
            id: batchScroll
            objectName: "voiceBatchReviewScroll"
            clip: true
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("优先在静音处分段，连续讲话最长 12 秒。请复核断句和短片段；内容去重只跳过相同解码内容，不推断说话人。原件一并留存，保存后样本仍待审核。") }
                ListView {
                    visible: !batchEmotionEnabled.checked
                    id: sampleList
                    objectName: "voicePreparedSamples"
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredHeight: 340; Layout.minimumHeight: 80
                    clip: true; spacing: 8
                    model: controller ? controller.samples : []
                    ScrollBar.vertical: SubScrollBar {}
                    delegate: ColumnLayout {
                        required property var modelData
                        required property int index
                        width: sampleList.width - Theme.scrollBarSize - 8
                        RowLayout {
                            SubCheckBox { id: chosen; checked: !!modelData.selected; enabled: controller && !controller.busy && !modelData.error && !modelData.savedVersionId; onToggled: controller.editSample(index, sampleText.text, checked) }
                            Label { Layout.fillWidth: true; elide: Text.ElideMiddle; color: Theme.text; text: modelData.sourcePath.split(/[\\/]/).pop() + (modelData.error ? "" : " · " + modelData.startSeconds.toFixed(2) + "–" + modelData.endSeconds.toFixed(2) + qsTr(" 秒")) }
                            SubButton { text: qsTr("试听"); enabled: preview && !!modelData.referencePath && !preview.loading; onClicked: preview.play(modelData.referencePath, modelData.referenceSha256) }
                        }
                        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: modelData.error || (modelData.savedVersionId ? qsTr("已保存 · 仍需在版本列表审核") : ((modelData.duplicateText ? qsTr("相同转写，请比较不同表演。 ") : "") + (modelData.quality ? modelData.quality.warnings.join(" ") : ""))) }
                        TextArea { id: sampleText; Layout.fillWidth: true; Layout.preferredHeight: 60; text: modelData.transcript || ""; placeholderText: qsTr("手工转写；本地识别不会覆盖已有内容"); selectByMouse: true; wrapMode: TextEdit.Wrap; enabled: controller && !controller.busy && !modelData.error && !modelData.savedVersionId; onTextChanged: if (controller) controller.editSample(index, text, chosen.checked) }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText
                            visible: !!modelData.emotionPrediction
                            text: modelData.emotionPrediction ? qsTr("情绪建议：%1 · 分数 %2 · 待逐个试听审核（不代表准确率）")
                                .arg(referenceEmotion.model[referenceEmotion.values.indexOf(modelData.emotionPrediction.label)])
                                .arg(Number(modelData.emotionPrediction.score).toFixed(3)) : ""
                        }
                    }
                }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.statusText : "" }
                SubCheckBox { id: batchEmotionEnabled; objectName: "voiceBatchEmotionEnabled"; text: qsTr("启用本地情绪建议（实验，手动执行）"); enabled: controller && !controller.busy }
                TextField { id: batchEmotionPython; objectName: "voiceBatchEmotionPython"; visible: batchEmotionEnabled.checked; Layout.fillWidth: true; placeholderText: emotionPython.placeholderText; selectByMouse: true; enabled: controller && !controller.busy }
                TextField { id: batchEmotionModel; objectName: "voiceBatchEmotionModel"; visible: batchEmotionEnabled.checked; Layout.fillWidth: true; placeholderText: emotionModel.placeholderText; selectByMouse: true; enabled: controller && !controller.busy }
                SubButton { objectName: "recognizeSampleEmotions"; visible: batchEmotionEnabled.checked; text: qsTr("识别勾选样本情绪（最多 100 段）"); enabled: controller && !controller.busy && sampleList.count > 0 && batchEmotionPython.text.trim().length > 0 && batchEmotionModel.text.trim().length > 0; onClicked: controller.recognizeEmotion(batchEmotionPython.text, batchEmotionModel.text, true) }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    SubButton { text: qsTr("本地中文转写"); enabled: controller && !controller.busy && sampleList.count > 0; onClicked: controller.transcribeSamples() }
                    SubButton { objectName: "savePreparedSamples"; text: qsTr("保存勾选样本（待审核）"); enabled: controller && !controller.busy && sampleList.count > 0; onClicked: controller.saveSamples(dialog.transcriptRequired) }
                    SubButton { text: qsTr("停止试听"); enabled: preview && (preview.playing || preview.loading); onClicked: preview.stop() }
                    SubButton { text: qsTr("取消处理"); visible: controller && controller.busy; onClicked: controller.cancel() }
                }
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: cloudCloneDialog
        objectName: "cloudCloneDialog"
        title: qsTr("创建通义云端音色")
        modal: true
        standardButtons: Dialog.Close
        readonly property var operation: controller ? controller.cloudClone : ({})
        onOpened: { cloneRights.checked = false; clonePrivacy.checked = false; cloneCost.checked = false; cloneChecked.checked = false }
        onClosed: {
            cloneUrl.text = ""
            if (controller) { if (controller.busy) controller.cancel(); else controller.discardCloudClone() }
        }
        footer: ColumnLayout {
            VoicePreviewTransport { sourceTitle: qsTr("云端音色 · 试听"); player: preview; preparing: controller && controller.previewLoading; onStopRequested: if (controller) controller.cancelPreview() }
            Flow {
                Layout.fillWidth: true; spacing: 8
                SubButton { text: qsTr("取消"); onClicked: cloudCloneDialog.close() }
                SubButton { objectName: "previewCloudClone"; text: qsTr("预览本次提交"); enabled: cloneUrl.enabled && cloneUrl.text.trim().length > 0 && clonePrefix.text.length > 0 && clonePrice.text.length > 0 && clonePrice.acceptableInput && cloudCloneDialog.operation.state !== "unknown"; onClicked: { cloneRights.checked = false; clonePrivacy.checked = false; cloneCost.checked = false; controller.previewCloudClone(cloneUrl.text.trim(), clonePrefix.text, Number(clonePrice.text), clonePriceSource.text.trim(), clonePriceDate.text) } }
                SubButton { objectName: "confirmCloudClone"; text: qsTr("确认并创建 1 个云端音色"); enabled: cloneRights.enabled && cloneRights.checked && clonePrivacy.checked && cloneCost.checked; onClicked: controller.confirmCloudClone(cloneRights.checked, clonePrivacy.checked, cloneCost.checked) }
                SubButton { objectName: "useCreatedCloudClone"; text: qsTr("查询审核状态并关联音色"); visible: cloudCloneDialog.operation.state === "created" || cloudCloneDialog.operation.state === "updated"; enabled: controller && !controller.busy; onClicked: controller.useCreatedCloudClone() }
            }
        }
        contentItem: ScrollView {
            id: cloneScroll
            objectName: "cloudCloneScroll"
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: cloneScroll.availableWidth
                spacing: 10
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("使用语音合成用途绑定的北京 Flash 账号。仅支持您已托管、已获使用权的 HTTPS 参考链接；应用不上传本地文件，也不下载或质检远端音频。") }
                RowLayout {
                    SubButton { text: qsTr("官方复刻说明"); onClicked: Qt.openUrlExternally("https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide") }
                    SubButton { text: qsTr("官方价格说明"); onClicked: Qt.openUrlExternally("https://help.aliyun.com/zh/model-studio/model-pricing") }
                }
                Label { text: qsTr("参考音频链接"); color: Theme.secondaryText }
                TextField { id: cloneUrl; objectName: "cloudCloneUrl"; Layout.fillWidth: true; placeholderText: qsTr("公网可读的 HTTPS 参考音频 URL"); selectByMouse: true; enabled: controller && !controller.busy && cloudCloneDialog.operation.state !== "prepared" }
                Label { text: qsTr("音色前缀"); color: Theme.secondaryText }
                TextField { id: clonePrefix; objectName: "cloudClonePrefix"; Layout.fillWidth: true; placeholderText: qsTr("音色前缀：1–10 位英文字母或数字"); maximumLength: 10; selectByMouse: true; enabled: cloneUrl.enabled }
                Label { text: qsTr("单次预计价格（人民币元，人工核对）"); color: Theme.secondaryText }
                TextField { id: clonePrice; objectName: "cloudClonePrice"; Layout.fillWidth: true; placeholderText: qsTr("您已核对的单个音色预计价格（人民币元，可为 0）"); validator: DoubleValidator { bottom: 0; top: 10000; decimals: 6; locale: "C" } enabled: cloneUrl.enabled }
                Label { text: qsTr("价格依据链接"); color: Theme.secondaryText }
                TextField { id: clonePriceSource; objectName: "cloudClonePriceSource"; Layout.fillWidth: true; placeholderText: qsTr("HTTPS 价格依据链接（不含密钥或签名）"); selectByMouse: true; enabled: cloneUrl.enabled }
                Label { text: qsTr("价格核对日期"); color: Theme.secondaryText }
                TextField { id: clonePriceDate; objectName: "cloudClonePriceDate"; Layout.fillWidth: true; placeholderText: qsTr("价格核对日期 YYYY-MM-DD"); text: Qt.formatDate(new Date(), "yyyy-MM-dd"); enabled: cloneUrl.enabled }
                Label {
                    objectName: "cloudCloneSummary"
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text
                    text: cloudCloneDialog.operation.operation ? qsTr("云端音色操作记录\nVoice ID：%1\n%2").arg(cloudCloneDialog.operation.voiceId || "").arg(cloudCloneDialog.operation.message || "") : cloudCloneDialog.operation.state ? qsTr("状态：%1\n账号：%2 · 型号：%3\n北京 · Workspace：%8\n参考域名：%4 · 前缀：%5\n创建 1 个音色 · 人工预计 ¥%6（非服务端费用上限）\n%7")
                        .arg(({prepared: qsTr("待确认"), created: qsTr("已创建，待查询"), unknown: qsTr("结果不明"), acknowledged: qsTr("已人工核对")})[cloudCloneDialog.operation.state] || qsTr("未准备")).arg(cloudCloneDialog.operation.accountName || (cloudCloneDialog.operation.binding ? cloudCloneDialog.operation.binding.cloudAccountId : ""))
                        .arg(cloudCloneDialog.operation.binding ? cloudCloneDialog.operation.binding.model : "").arg(cloudCloneDialog.operation.referenceHost || "")
                        .arg(cloudCloneDialog.operation.prefix || "").arg(cloudCloneDialog.operation.priceCny === undefined ? "?" : cloudCloneDialog.operation.priceCny)
                        .arg(cloudCloneDialog.operation.message || "").arg(cloudCloneDialog.operation.binding ? cloudCloneDialog.operation.binding.businessSpace : "") : ""
                }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("参考 URL 仅随本次请求提交，记录只保存其域名和 URL 哈希（不是音频哈希）。服务商可读取完整托管文件，最多取 10 秒克隆；不开启降噪或音量归一化。素材留存、训练用途和删除规则请核对服务商政策；托管源文件须由您自行管理。") }
                SubCheckBox { id: cloneRights; objectName: "cloudCloneRights"; text: qsTr("我拥有该声音素材的使用权，并已核对参考内容"); enabled: controller && !controller.busy && cloudCloneDialog.operation.state === "prepared" }
                SubCheckBox { id: clonePrivacy; objectName: "cloudClonePrivacy"; text: qsTr("同意服务商读取链接，并已核对其素材处理与留存政策"); enabled: cloneRights.enabled }
                SubCheckBox { id: cloneCost; objectName: "cloudCloneCost"; text: qsTr("已核对收费规则并同意本次费用，以实际账单为准"); enabled: cloneRights.enabled }
                SubButton { text: qsTr("撤回预览，重新填写"); visible: cloudCloneDialog.operation.state === "prepared"; enabled: controller && !controller.busy; onClicked: controller.discardCloudClone() }
                TextField { objectName: "createdCloudVoiceId"; Layout.fillWidth: true; visible: !!cloudCloneDialog.operation.voiceId; readOnly: true; selectByMouse: true; text: cloudCloneDialog.operation.voiceId || "" }
                SubButton { text: qsTr("重读提交记录"); enabled: controller && !controller.busy; onClicked: controller.reloadCloudClone() }
                SubCheckBox { id: cloneChecked; objectName: "cloudCloneChecked"; visible: cloudCloneDialog.operation.state === "unknown"; text: qsTr("我已到服务商控制台核对结果并记录 Voice ID／费用"); enabled: controller && !controller.busy }
                SubButton { text: qsTr("记录人工核对，允许重新准备"); visible: cloneChecked.visible; enabled: cloneChecked.enabled && cloneChecked.checked; onClicked: controller.acknowledgeCloudClone(cloneChecked.checked) }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.statusText : "" }
                SubButton { text: qsTr("取消等待（远端结果可能不明）"); visible: controller && controller.busy; onClicked: controller.cancel() }
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: cloudDeletionDialog
        objectName: "cloudDeletionDialog"
        property bool updating: false
        title: updating ? qsTr("更新云端自定义音色") : qsTr("删除云端自定义音色")
        modal: true
        standardButtons: Dialog.Close
        readonly property var operation: controller ? controller.cloudClone : ({})
        onOpened: { deletionTyped.text = ""; deletionAccepted.checked = false; deletionChecked.checked = false; updatePrivacy.checked = false; updateCost.checked = false }
        onClosed: { updateUrl.text = ""; if (controller) { if (controller.busy) controller.cancel(); else controller.discardCloudClone() } }
        contentItem: ScrollView {
            id: deletionScroll
            objectName: "cloudDeletionScroll"
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: deletionScroll.availableWidth
                spacing: 10
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text; text: cloudDeletionDialog.updating ? qsTr("更新会替换同一 Voice ID 的远端声音，影响其它应用。确认后停止本账号云任务；旧音频保留，旧声音版本不能再次生成。更新后须查询审核状态并选用新版本。参考链接由您托管，服务商可读取整个文件，应用不下载或质检。") : qsTr("删除无法撤销，可能影响所有使用此 Voice ID 的工程和其它应用。请先停止相关生成。已有音频和项目声音快照保留；删除音色不等于删除托管源文件或服务商其它素材副本。") }
                Label { text: qsTr("目标自定义 Voice ID"); color: Theme.secondaryText }
                TextField { id: deletionVoice; objectName: "cloudDeletionVoice"; Layout.fillWidth: true; selectByMouse: true; maximumLength: 256; placeholderText: qsTr("北京 Flash 自定义 Voice ID，系统音色不可变更"); enabled: controller && !controller.busy && cloudDeletionDialog.operation.state !== "delete_prepared" && cloudDeletionDialog.operation.state !== "update_prepared" }
                Label { visible: cloudDeletionDialog.updating; text: qsTr("新的 HTTPS 参考音频链接"); color: Theme.secondaryText }
                TextField { id: updateUrl; objectName: "cloudUpdateUrl"; visible: cloudDeletionDialog.updating; Layout.fillWidth: true; selectByMouse: true; enabled: deletionVoice.enabled; placeholderText: qsTr("已获使用权、公网可读的 HTTPS 链接") }
                SubButton { objectName: "previewCloudDeletion"; text: cloudDeletionDialog.updating ? qsTr("预览更新目标") : qsTr("预览删除目标"); enabled: deletionVoice.enabled && deletionVoice.text.trim().length > 0 && (!cloudDeletionDialog.updating || updateUrl.text.trim().length > 0) && cloudDeletionDialog.operation.state !== "unknown"; onClicked: { deletionTyped.text = ""; deletionAccepted.checked = false; updatePrivacy.checked = false; updateCost.checked = false; if (cloudDeletionDialog.updating) controller.previewCloudUpdate(deletionVoice.text.trim(), updateUrl.text.trim()); else controller.previewCloudDeletion(deletionVoice.text.trim()) } }
                Label {
                    objectName: "cloudDeletionSummary"
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text
                    text: cloudDeletionDialog.operation.operation ? qsTr("账号：%1\n型号：%2 · 北京 Workspace：%3\nVoice ID：%4\n%5%6")
                        .arg(cloudDeletionDialog.operation.accountName || (cloudDeletionDialog.operation.binding ? cloudDeletionDialog.operation.binding.cloudAccountId : ""))
                        .arg(cloudDeletionDialog.operation.binding ? cloudDeletionDialog.operation.binding.model : "")
                        .arg(cloudDeletionDialog.operation.binding ? cloudDeletionDialog.operation.binding.businessSpace : "")
                        .arg(cloudDeletionDialog.operation.voiceId || "").arg(cloudDeletionDialog.operation.message || "").arg(cloudDeletionDialog.operation.referenceHost ? qsTr("\n参考域名：%1（仅保留 URL 哈希，不是音频指纹）").arg(cloudDeletionDialog.operation.referenceHost) : "") : ""
                }
                Label { visible: deletionTyped.visible; text: qsTr("再次完整输入上方 Voice ID"); color: Theme.secondaryText }
                TextField { id: deletionTyped; objectName: "cloudDeletionTyped"; Layout.fillWidth: true; selectByMouse: true; maximumLength: 256; visible: cloudDeletionDialog.operation.state === "delete_prepared" || cloudDeletionDialog.operation.state === "update_prepared"; enabled: controller && !controller.busy }
                SubCheckBox { id: deletionAccepted; objectName: "cloudDeletionAccepted"; visible: deletionTyped.visible; enabled: deletionTyped.enabled; text: cloudDeletionDialog.updating ? qsTr("我有权使用新素材并更新此音色，已确认替换影响") : qsTr("我有权删除此音色，已确认影响并同意发送一次删除请求") }
                SubCheckBox { id: updatePrivacy; objectName: "cloudUpdatePrivacy"; visible: cloudDeletionDialog.updating && deletionTyped.visible; enabled: deletionTyped.enabled; text: qsTr("同意服务商读取链接，已核对素材处理与留存政策") }
                SubCheckBox { id: updateCost; objectName: "cloudUpdateCost"; visible: updatePrivacy.visible; enabled: deletionTyped.enabled; text: qsTr("已核对收费规则并同意本次更新费用，以实际账单为准") }
                SubButton { objectName: "confirmCloudDeletion"; text: cloudDeletionDialog.updating ? qsTr("确认更新此云端音色") : qsTr("确认删除此云端音色"); visible: deletionTyped.visible; enabled: deletionAccepted.enabled && deletionAccepted.checked && deletionTyped.text === cloudDeletionDialog.operation.voiceId && (!cloudDeletionDialog.updating || (updatePrivacy.checked && updateCost.checked)); onClicked: { if (cloudDeletionDialog.updating) controller.confirmCloudUpdate(deletionTyped.text, deletionAccepted.checked, updatePrivacy.checked, updateCost.checked); else controller.confirmCloudDeletion(deletionTyped.text, deletionAccepted.checked) } }
                SubButton { text: qsTr("撤回操作预览"); visible: deletionTyped.visible; enabled: controller && !controller.busy; onClicked: controller.discardCloudClone() }
                SubButton { text: qsTr("查询审核状态并关联新版本"); visible: cloudDeletionDialog.operation.state === "updated"; enabled: controller && !controller.busy; onClicked: controller.useCreatedCloudClone() }
                SubButton { text: qsTr("重读操作记录"); enabled: controller && !controller.busy; onClicked: controller.reloadCloudClone() }
                SubCheckBox { id: deletionChecked; visible: cloudDeletionDialog.operation.state === "unknown"; enabled: controller && !controller.busy; text: qsTr("我已在原账号控制台核对操作结果并保存必要记录") }
                SubButton { text: qsTr("记录人工核对，允许重新准备"); visible: deletionChecked.visible; enabled: deletionChecked.enabled && deletionChecked.checked; onClicked: controller.acknowledgeCloudClone(deletionChecked.checked) }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: controller ? controller.statusText : "" }
                SubButton { text: qsTr("取消等待（不能保证远端停止）"); visible: controller && controller.busy; onClicked: controller.cancel() }
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: presetDialog
        property string versionId: ""
        title: qsTr("保存新的声音预设")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ColumnLayout {
            TextField { id: presetName; Layout.fillWidth: true; placeholderText: qsTr("预设名称，如中文旁白"); maximumLength: 128; selectByMouse: true }
            TextArea { id: usageScope; Layout.fillWidth: true; Layout.preferredHeight: 100; placeholderText: qsTr("素材来源与允许的使用范围（用户声明）"); selectByMouse: true; wrapMode: TextEdit.Wrap }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("首版预设使用中文。Base 不支持自由表演指令；此操作不生成、不上传，不改变旧版本。") }
        }
        onAccepted: controller.savePreset(versionId, presetName.text, usageScope.text)
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: addVoicePage
        objectName: "voiceAddPage"
        title: qsTr("添加音色")
        contentItem: ScrollView {
            id: addVoiceScroll
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: addVoiceScroll.availableWidth; spacing: 16
                Label { text: qsTr("选择创建方式"); color: Theme.text; font.bold: true }
                PrimaryButton { objectName: "addReferenceVoice"; enabled: controller && !controller.busy; text: qsTr("参考音频创建"); onClicked: { controller.beginCreation(); controller.setCreationTranscriptRequired(dialog.transcriptRequired); creationCollectPage.open() } }
                Label { text: qsTr("使用已有录音创建可复用的参考音色，不训练新模型。"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
                SubButton { objectName: "addTrainedVoice"; text: qsTr("训练专属模型"); onClicked: { unavailablePage.training = true; unavailablePage.open() } }
                Label { text: qsTr("使用数据集微调专属模型 · 训练后端尚未接入"); color: Theme.secondaryText }
                SubButton { objectName: "importTrainedVoice"; text: qsTr("导入已有模型"); onClicked: { unavailablePage.training = false; unavailablePage.open() } }
                Label { text: qsTr("导入已经训练的专属权重 · 模型导入后端尚未接入"); color: Theme.secondaryText }
                SubButton { text: qsTr("添加云端音色"); onClicked: { typeTabs.currentIndex = 1; dialog.activePage = null } }
                Label { text: qsTr("选择系统或账号已有音色，也可提交参考链接创建通义云端音色。"); color: Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: unavailablePage
        objectName: "unavailableVoicePage"
        property bool training: true
        title: training ? qsTr("训练专属模型") : qsTr("导入已有模型")
        contentItem: ColumnLayout {
            Label { text: unavailablePage.training ? qsTr("训练后端尚未接入") : qsTr("专属模型导入尚未接入"); color: Theme.warning; font.bold: true }
            Label { text: qsTr("参考音频创建可以直接使用录音生成配音。专属模型训练和权重导入是独立能力，当前不会建立任务或生成模型。"); wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.secondaryText }
            SubButton { text: qsTr("添加素材"); enabled: false }
            SubButton { text: unavailablePage.training ? qsTr("检查并开始训练") : qsTr("导入模型"); enabled: false }
            SubButton { visible: unavailablePage.training; text: qsTr("终止训练"); enabled: false }
            Item { Layout.fillHeight: true }
        }
    }
    FileDialog {
        id: creationFiles
        fileMode: FileDialog.OpenFiles
        nameFilters: referenceDialog.nameFilters
        onAccepted: if (controller) controller.prepareSamples(selectedFiles, true)
    }
    Connections {
        target: controller
        function onOperationFinished(success) {
            // 创建成功后关闭创建流程；分析完成时 creating 仍为 true，不会误关。
            if (!success || !controller || controller.creating || controller.samples.length > 0) return;
            if (dialog.activePage === creationCollectPage || dialog.activePage === creationAnalyzePage
                || dialog.activePage === creationProfilePage) dialog.activePage = null;
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: creationCollectPage
        objectName: "creationCollectPage"
        title: qsTr("创建音色 · 收集素材")
        onClosed: { if (preview) preview.stop(); if (controller) controller.cancelCreation() }
        contentItem: ColumnLayout {
            spacing: 10
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("收集同一说话人的短音频。一次可以加入很多条；总时长达到 10 秒即可分析，30 秒够用，90 秒更好。") }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                PrimaryButton { objectName: "creationUploadButton"; text: qsTr("上传音频"); enabled: controller && !controller.busy; onClicked: creationFiles.open() }
                SubButton { objectName: "creationRecordButton"; text: qsTr("录制音频"); enabled: false; ToolTip.text: qsTr("录制暂未接入，先用上传音频"); ToolTip.visible: recordButtonHover.hovered; HoverHandler { id: recordButtonHover } }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("已上传 %1 秒").arg(controller ? controller.creationDraft.totalSeconds.toFixed(1) : "0.0")
                    color: controller && controller.creationDraft.usableSeconds >= 30 ? Theme.success : Theme.secondaryText
                }
            }
            ListView {
                id: collectSamples
                objectName: "creationSamples"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8
                model: controller ? controller.samples : []
                ScrollBar.vertical: SubScrollBar {}
                delegate: ColumnLayout {
                    required property var modelData
                    required property int index
                    readonly property real sampleDuration: (modelData.endSeconds || 0) - (modelData.startSeconds || 0)
                    width: collectSamples.width - Theme.scrollBarSize - 8; spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: modelData.sourcePath.split(/[\\/]/).pop(); elide: Text.ElideMiddle; Layout.fillWidth: true; color: Theme.text }
                        Label { text: sampleDuration.toFixed(2) + qsTr(" 秒"); color: Theme.secondaryText }
                        SubButton { text: qsTr("删除"); enabled: controller && !controller.busy; onClicked: controller.removeCreationSample(index) }
                    }
                    Label { visible: !!modelData.error; text: modelData.error || ""; color: Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
            Rectangle {
                Layout.fillWidth: true; height: 8; radius: 4; color: Theme.input
                Rectangle {
                    width: parent.width * Math.min(1, (controller ? controller.creationDraft.totalSeconds : 0) / 90)
                    height: parent.height; radius: 4; color: Theme.accent
                }
            }
            Label { text: controller ? controller.statusText : ""; color: controller && controller.statusIsWarning ? Theme.warning : Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
        }
        footer: ColumnLayout {
            VoicePreviewTransport { sourceTitle: qsTr("声音库 · 当前样本"); player: preview; preparing: controller && controller.previewLoading; onStopRequested: if (controller) controller.cancelPreview() }
            Flow {
                Layout.fillWidth: true; spacing: 8
                SubButton { text: qsTr("取消"); onClicked: { if (controller) controller.cancelCreation(); dialog.activePage = null } }
                PrimaryButton {
                    objectName: "creationAnalyzeButton"
                    text: qsTr("分析音频")
                    enabled: controller && !controller.busy && controller.creationDraft.canAnalyze === true
                    onClicked: { controller.analyzeCreationSamples(); creationAnalyzePage.open() }
                }
            }
            Label {
                objectName: "creationAnalyzeBlockReason"
                visible: !!controller && controller.creationDraft.canAnalyze !== true
                text: controller ? (controller.creationDraft.analyzeBlockReason || "") : ""
                color: Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: creationAnalyzePage
        objectName: "creationAnalyzePage"
        title: qsTr("创建音色 · 分析音频")
        contentItem: ColumnLayout {
            spacing: 8
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: controller && controller.statusIsWarning ? Theme.warning : Theme.secondaryText; text: controller ? controller.statusText : "" }
            ListView {
                id: analyzedSamples
                objectName: "creationAnalyzedSamples"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8
                model: controller ? controller.samples : []
                ScrollBar.vertical: SubScrollBar {}
                delegate: ColumnLayout {
                    required property var modelData
                    readonly property string analyzeState: modelData.analysisState || "queued"
                    width: analyzedSamples.width - Theme.scrollBarSize - 8; spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: modelData.sourcePath.split(/[\\/]/).pop(); elide: Text.ElideMiddle; Layout.fillWidth: true; color: Theme.text }
                        Label {
                            text: analyzeState === "queued" ? qsTr("排队") : analyzeState === "analyzing" ? qsTr("分析中…") : analyzeState === "passed" ? qsTr("通过") : analyzeState === "warning" ? qsTr("警告") : qsTr("失败")
                            color: analyzeState === "passed" ? Theme.success : analyzeState === "failed" ? Theme.error : analyzeState === "warning" ? Theme.warning : Theme.secondaryText
                        }
                        Label {
                            visible: !!modelData.detectedLanguage
                            text: modelData.detectedLanguage === "Chinese" ? qsTr("中文") : modelData.detectedLanguage === "English" ? qsTr("英语") : modelData.detectedLanguage === "Japanese" ? qsTr("日语") : ""
                            color: Theme.secondaryText
                        }
                    }
                    Label { visible: !!modelData.analysisMessage; text: modelData.analysisMessage || ""; color: Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
        }
        footer: Flow {
            Layout.fillWidth: true; spacing: 8
            SubButton { text: qsTr("上一步"); onClicked: creationCollectPage.open() }
            PrimaryButton {
                objectName: "creationContinueButton"
                text: qsTr("继续")
                enabled: controller && !controller.busy && controller.creationDraft.analyzed === true
                onClicked: creationProfilePage.open()
            }
        }
    }
    VoiceLibraryPage {
        host: dialog; controller: dialog.controller; player: dialog.preview
        id: creationProfilePage
        objectName: "creationProfilePage"
        // QML 对 var 属性 + Q_PROPERTY 的依赖追踪在逐字符编辑下不可靠，改为显式刷新创建条件。
        property var draftState: controller ? controller.creationDraft : ({})
        function refreshDraftState() { draftState = controller ? controller.creationDraft : ({}) }
        Connections {
            target: controller
            function onChanged() { creationProfilePage.refreshDraftState() }
        }
        title: qsTr("创建音色 · 音色资料")
        onOpened: {
            refreshDraftState()
            if (controller && creationTranscript) {
                const index = controller.creationDraft.promptIndex
                const rows = controller.samples
                creationTranscript.text = index >= 0 && index < rows.length ? (rows[index].transcript || "") : ""
            }
        }
        contentItem: ColumnLayout {
            spacing: 10
            TextField { id: profileName; objectName: "creationVoiceName"; Layout.fillWidth: true; placeholderText: qsTr("音色名称（必填）"); maximumLength: 256; selectByMouse: true; enabled: controller && !controller.busy }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                Label { text: qsTr("参考语言"); color: Theme.text }
                SubComboBox {
                    id: profileLanguage
                    objectName: "creationReferenceLanguage"
                    Layout.fillWidth: true
                    model: [qsTr("中文"), qsTr("英语"), qsTr("日语")]
                    currentIndex: creationProfilePage.draftState.promptLanguage === "Japanese" ? 2 : creationProfilePage.draftState.promptLanguage === "English" ? 1 : 0
                    enabled: controller && !controller.busy
                    onActivated: function(activatedIndex) {
                        if (!controller) return
                        const index = creationProfilePage.draftState.promptIndex
                        if (index >= 0) controller.confirmCreationSample(index, true, activatedIndex >= 0 ? ["Chinese", "English", "Japanese"][activatedIndex] : "")
                        creationProfilePage.refreshDraftState()
                    }
                }
            }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("参考语言是素材本身的语言；配音目标语言在配音工作区选择。") }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text
                text: {
                    const draft = creationProfilePage.draftState
                    const rows = controller ? controller.samples : []
                    if (draft.promptIndex === undefined || draft.promptIndex < 0 || draft.promptIndex >= rows.length) return qsTr("当前 prompt：未选择")
                    const row = rows[draft.promptIndex]
                    const duration = (row.endSeconds || 0) - (row.startSeconds || 0)
                    return qsTr("当前 prompt：第 %1 条 · %2 · %3 秒").arg(draft.promptIndex + 1).arg(row.sourcePath.split(/[\\/]/).pop()).arg(duration.toFixed(2))
                }
            }
            ListView {
                id: promptChoices
                objectName: "creationPromptChoices"
                Layout.fillWidth: true; Layout.preferredHeight: 120; clip: true; spacing: 4
                model: controller ? controller.samples : []
                ScrollBar.vertical: SubScrollBar {}
                delegate: RowLayout {
                    required property var modelData
                    required property int index
                    readonly property bool usable: !modelData.error && modelData.analysisState !== "failed"
                    visible: usable
                    height: usable ? implicitHeight : 0
                    width: promptChoices.width - Theme.scrollBarSize - 8
                    Label { text: qsTr("第 %1 条 · %2").arg(index + 1).arg(modelData.sourcePath.split(/[\\/]/).pop()); elide: Text.ElideMiddle; Layout.fillWidth: true; color: Theme.text }
                    SubButton { text: qsTr("试听"); enabled: !!modelData.referencePath && !controller.busy; onClicked: preview.play(modelData.referencePath, modelData.referenceSha256) }
                    SubButton {
                        objectName: "creationPromptButton"
                        text: controller && controller.creationDraft.promptIndex === index ? qsTr("当前 prompt") : qsTr("设为 prompt")
                        enabled: controller && !controller.busy
                        onClicked: {
                            if (!controller) return
                            controller.setCreationPrompt(index)
                            creationProfilePage.refreshDraftState()
                            if (creationTranscript) creationTranscript.text = modelData.transcript || ""
                        }
                    }
                }
            }
            TextArea {
                id: creationTranscript
                objectName: "creationTranscript"
                Layout.fillWidth: true; Layout.preferredHeight: 80
                wrapMode: TextEdit.Wrap; selectByMouse: true
                placeholderText: qsTr("参考音频里的原话（识别草稿需人工确认）")
                enabled: controller && !controller.busy
                onTextChanged: {
                    if (!controller) return
                    const index = controller.creationDraft.promptIndex
                    if (index >= 0) controller.editCreationTranscript(index, text)
                    creationProfilePage.refreshDraftState()
                }
            }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondaryText; text: qsTr("只编辑当前 prompt 这条的转写；这是参考音频里的原话，日语参考就保持日语。") }
            ColumnLayout {
                objectName: "creationIssues"
                visible: !!controller && (creationProfilePage.draftState.issues || []).length > 0
                Layout.fillWidth: true; spacing: 2
                Repeater {
                    model: controller ? (creationProfilePage.draftState.issues || []) : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.warning
                        text: qsTr("第 %1 条 · %2：%3").arg(modelData.index).arg(modelData.name).arg(modelData.reason)
                    }
                }
            }
            Label { text: controller ? controller.statusText : ""; color: controller && controller.statusIsWarning ? Theme.warning : Theme.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Item { Layout.fillHeight: true }
        }
        footer: ColumnLayout {
            VoicePreviewTransport { sourceTitle: qsTr("声音库 · 当前 prompt"); player: preview; preparing: controller && controller.previewLoading; onStopRequested: if (controller) controller.cancelPreview() }
            Flow {
                Layout.fillWidth: true; spacing: 8
                SubButton { text: qsTr("取消"); onClicked: { if (controller) controller.cancelCreation(); dialog.activePage = null } }
                SubButton { text: qsTr("上一步"); onClicked: creationAnalyzePage.open() }
                PrimaryButton {
                    id: createVoiceButton
                    objectName: "createReferenceVoice"
                    text: qsTr("创建音色")
                    enabled: controller && !controller.busy && profileName.text.trim().length > 0 && creationProfilePage.draftState.ready === true
                    onClicked: controller.createFromSamples(profileName.text, dialog.transcriptRequired)
                }
            }
            Label {
                objectName: "creationBlockReason"
                visible: !createVoiceButton.enabled && !!controller
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.warning
                text: profileName.text.trim().length === 0 ? qsTr("请填写音色名称") : (creationProfilePage.draftState.blockReason || "")
            }
        }
    }
    Dialog {
        id: archiveDialog
        anchors.centerIn: parent
        title: qsTr("停用音色")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("停用后从当前列表隐藏。已保存的版本与项目副本保留。"); color: Theme.text }
        onAccepted: if (controller) controller.archiveProfile()
    }
}
