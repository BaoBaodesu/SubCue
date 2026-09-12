import QtQuick
import QtQuick.Controls

ToolTip {
    id: control
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeTiny
    padding: 6
    delay: 500
    timeout: 4000

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.text
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        radius: Theme.radiusPopup
        color: Theme.panelRaised
        border.color: Theme.border
        border.width: 1
    }
}
