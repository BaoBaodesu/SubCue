import QtQuick
import QtQuick.Controls

// 深色组合框：字段统一输入底色，Popup 深色、选中行用选区蓝。
ComboBox {
    id: control
    implicitHeight: 28
    leftPadding: 10
    rightPadding: 26
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.displayText
        font: control.font
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Theme.text : Theme.disabledText
    }

    indicator: Text {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: control.enabled ? Theme.muted : Theme.disabledText
    }

    background: Rectangle {
        radius: Theme.radiusInput
        color: control.enabled ? Theme.input : Theme.buttonDisabled
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.border
    }

    popup: Popup {
        y: control.height + 2
        width: control.width
        padding: 1
        implicitHeight: Math.min(contentItem.implicitHeight, 260)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollBar.vertical: SubScrollBar { }
        }

        background: Rectangle {
            radius: Theme.radiusPopup
            color: Theme.input
            border.color: Theme.border
        }
    }

    delegate: ItemDelegate {
        id: itemDelegate
        width: ListView.view.width
        implicitHeight: 26
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: control.textRole && modelData && modelData[control.textRole] !== undefined
                  ? modelData[control.textRole] : modelData
            font: control.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            color: itemDelegate.highlighted ? Theme.text : Theme.secondaryText
        }

        background: Rectangle {
            color: itemDelegate.highlighted ? Theme.selection
                 : itemDelegate.hovered ? Theme.listHover
                 : "transparent"
        }
    }
}
