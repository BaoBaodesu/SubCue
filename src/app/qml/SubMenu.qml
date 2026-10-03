import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Menu {
    id: control
    property bool countedPopup: false
    function updatePopupCount(opened) {
        const win = control.parent ? control.parent.Window.window : null
        if (!win || typeof win.popupDepth === "undefined" || countedPopup === opened) return
        win.popupDepth = Math.max(0, win.popupDepth + (opened ? 1 : -1))
        countedPopup = opened
    }
    Component.onDestruction: updatePopupCount(false)

    onVisibleChanged: updatePopupCount(visible)
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

        contentItem: RowLayout {
            spacing: 16
            Text {
                Layout.fillWidth: true
                text: menuItem.text; font: menuItem.font; elide: Text.ElideRight
                color: menuItem.enabled ? Theme.text : Theme.disabledText
            }
            Text {
                text: menuItem.action ? (menuItem.action.keySequence || String(menuItem.action.shortcut || "")) : ""
                font.family: "Consolas"; font.pixelSize: 12
                color: menuItem.enabled ? Theme.secondaryText : Theme.disabledText
            }
        }

        background: Rectangle {
            color: menuItem.highlighted ? Theme.selection : "transparent"
        }
    }
}
