import QtQuick
import QtQuick.Controls

// 深色输入框：统一背景/边框/焦点/占位/选区颜色。
TextField {
    id: control
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    color: enabled ? Theme.text : Theme.disabledText
    placeholderTextColor: Theme.placeholder
    selectionColor: Theme.selection
    selectedTextColor: Theme.text

    background: Rectangle {
        radius: Theme.radiusInput
        color: control.enabled ? Theme.input : Theme.buttonDisabled
        border.width: 1
        border.color: !control.enabled ? Theme.border
                     : control.activeFocus ? Theme.focusBorder
                     : Theme.border
    }
}
