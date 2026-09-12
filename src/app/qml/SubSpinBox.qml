import QtQuick
import QtQuick.Controls

// 深色数字输入框：统一输入底色与上下箭头状态。
SpinBox {
    id: control
    implicitHeight: 28
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: TextInput {
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: control.enabled ? Theme.text : Theme.disabledText
        selectionColor: Theme.selection
        selectedTextColor: Theme.text
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 0 : control.width - width
        y: 0
        implicitWidth: 20
        implicitHeight: 14
        color: control.up.pressed ? Theme.buttonPressed
             : control.up.hovered ? Theme.buttonHover
             : Theme.button
        border.color: Theme.buttonBorder
        Text {
            anchors.centerIn: parent
            text: "▲"
            font.pixelSize: 8
            color: control.enabled ? Theme.muted : Theme.disabledText
        }
    }

    down.indicator: Rectangle {
        x: control.mirrored ? 0 : control.width - width
        y: control.height - implicitHeight
        implicitWidth: 20
        implicitHeight: 14
        color: control.down.pressed ? Theme.buttonPressed
             : control.down.hovered ? Theme.buttonHover
             : Theme.button
        border.color: Theme.buttonBorder
        Text {
            anchors.centerIn: parent
            text: "▼"
            font.pixelSize: 8
            color: control.enabled ? Theme.muted : Theme.disabledText
        }
    }

    background: Rectangle {
        radius: Theme.radiusInput
        color: control.enabled ? Theme.input : Theme.buttonDisabled
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.border
    }
}
