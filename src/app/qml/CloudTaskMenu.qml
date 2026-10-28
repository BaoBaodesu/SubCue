import QtQuick
import QtQuick.Controls

MenuButton {
    id: control
    required property var cloud
    required property var tasks
    text: qsTr("AI 服务 ▾")
    objectName: "cloudQuickSwitch"
    // 菜单由 Instantiator 持有，命令入口供菜单和键盘使用。
    function activateChoice(taskId, target) {
        for (let i = 0; i < menu.count; ++i) {
            const group = menu.menuAt(i)
            if (!group || group.modelData.id !== taskId) continue
            for (let j = 0; j < group.count; ++j) {
                const entry = group.itemAt(j)
                if (entry && entry.objectName === "cloudTaskChoice_" + taskId + "_" + target) { entry.triggered(); return true }
            }
        }
        return false
    }
    onClicked: menu.popup(control, 0, height)
    SubMenu {
        id: menu
        Instantiator {
            model: control.tasks
            onObjectAdded: (index, object) => menu.insertMenu(index, object)
            onObjectRemoved: (index, object) => menu.removeMenu(object)
            SubMenu {
                id: taskMenu
                objectName: "cloudTaskMenu_" + modelData.id
                required property var modelData
                readonly property var choices: { control.cloud.models; control.cloud.accounts; control.cloud.tests; return control.cloud.taskChoices(modelData.id) }
                readonly property var binding: control.cloud.bindings.find(row => row.taskId === modelData.id) || ({})
                title: modelData.name
                MenuItem { enabled: false; text: { control.cloud.accounts; control.cloud.models; control.cloud.bindings; control.cloud.tests; return control.cloud.taskStatus(taskMenu.modelData.id) } }
                Instantiator {
                    model: taskMenu.choices
                    onObjectAdded: (index, object) => taskMenu.insertItem(index + 1, object)
                    onObjectRemoved: (index, object) => taskMenu.removeItem(object)
                    MenuItem {
                        required property var modelData
                        objectName: "cloudTaskChoice_" + taskMenu.modelData.id + "_" + modelData.id
                        property string taskId: taskMenu.modelData.id
                        text: modelData.name; checkable: true; checked: taskMenu.binding.target === modelData.id
                        onTriggered: if (!control.cloud.switchTask(taskId, modelData.id)) failure.open()
                    }
                }
            }
        }
        MenuSeparator {}
        MenuItem { text: qsTr("管理 AI 服务…"); onTriggered: control.Window.window.openSettings("cloud", "") }
    }
    Dialog {
        id: failure; parent: Overlay.overlay; modal: true; title: qsTr("未切换模型"); standardButtons: Dialog.Ok
        Label { width: Math.min(360, control.Window.window.width - 64); wrapMode: Text.Wrap; color: Theme.text; text: control.cloud.status; textFormat: Text.PlainText }
    }
}
