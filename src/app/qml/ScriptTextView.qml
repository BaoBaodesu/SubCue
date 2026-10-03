import QtQuick
import QtQuick.Controls

// 文档放在外层 Flickable 里滚动，滚动条由外层 SubScrollBar 绘制。
// 框选还在可见文本框内时不滚动；指针拖出文本框后才滚动并延伸选区。
TextArea {
    id: editor

    property Flickable scrollView
    selectByMouse: false
    wrapMode: TextEdit.Wrap

    property bool dragSelecting: false
    property real dragLocalX: 0
    property real dragLocalY: 0

    function extendSelectionAt(localX, localY) {
        const x = Math.max(1, Math.min(width - 1, localX))
        const y = Math.max(1, Math.min(height - 1, localY))
        moveCursorSelection(positionAt(x, y))
    }

    MouseArea {
        anchors.fill: parent
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) {
            editor.forceActiveFocus()
            editor.dragLocalX = mouse.x
            editor.dragLocalY = mouse.y
            editor.dragSelecting = true
            const pos = editor.positionAt(mouse.x, mouse.y)
            if (mouse.modifiers & Qt.ShiftModifier)
                editor.moveCursorSelection(pos)
            else
                editor.cursorPosition = pos
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return
            editor.dragLocalX = mouse.x
            editor.dragLocalY = mouse.y
            editor.extendSelectionAt(mouse.x, mouse.y)
        }
        onReleased: editor.dragSelecting = false
        onDoubleClicked: function(mouse) {
            editor.cursorPosition = editor.positionAt(mouse.x, mouse.y)
            editor.selectWord()
        }
    }

    Timer {
        interval: 16
        repeat: true
        running: editor.dragSelecting && editor.scrollView !== null
        onTriggered: {
            const flick = editor.scrollView
            const yInView = editor.dragLocalY - flick.contentY
            let delta = 0
            if (yInView < 0)
                delta = -Math.max(3, -yInView * 0.35)
            else if (yInView > flick.height)
                delta = Math.max(3, (yInView - flick.height) * 0.35)
            if (delta === 0) return
            const maxY = Math.max(0, flick.contentHeight - flick.height)
            const next = Math.max(0, Math.min(maxY, flick.contentY + delta))
            if (next === flick.contentY) return
            flick.contentY = next
            editor.extendSelectionAt(editor.dragLocalX, delta < 0 ? flick.contentY + 2 : flick.contentY + flick.height - 2)
        }
    }
}
