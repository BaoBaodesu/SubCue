import QtQuick
import QtQuick.Controls

Menu {
    id: control
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    background: Rectangle {
        implicitWidth: 180
        implicitHeight: 32
        radius: Theme.radiusPopup
        color: Theme.panelRaised
        border.color: Theme.border
        border.width: 1
    }

    delegate: MenuItem {
        id: menuItem
        implicitHeight: 28
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize

        contentItem: Text {
            text: menuItem.text
            font: menuItem.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            color: !menuItem.enabled ? Theme.disabledText
                 : menuItem.highlighted ? Theme.text
                 : Theme.menuButtonText
        }

        background: Rectangle {
            color: menuItem.highlighted ? Theme.selection : "transparent"
        }
    }
}
