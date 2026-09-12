import QtQuick

Rectangle {
    id: bar
    required property var timeline
    height: 18
    color: Theme.scrollTrack
    border.color: Theme.border
    readonly property bool interacting: mouse.pressed
    readonly property real totalMs: Math.max(1, timeline.durationUs / 1000)
    readonly property real startRatio: Math.max(0, timeline.scrollOffset / timeline.pixelsPerMs / totalMs)
    readonly property real endRatio: Math.min(1, (timeline.scrollOffset + timeline.width) / timeline.pixelsPerMs / totalMs)
    readonly property real minimumRatio: Math.min(1, timeline.width / (10 * totalMs))
    readonly property real rangeWidth: Math.min(width, Math.max(40, width * (endRatio - startRatio)))
    readonly property real rangeX: Math.min(width - rangeWidth, width * startRatio)

    Rectangle {
        x: bar.rangeX
        width: bar.rangeWidth
        height: parent.height
        color: mouse.pressed ? Theme.scrollThumbPressed : mouse.containsMouse ? Theme.scrollThumbHover : Theme.scrollThumb
        border.color: Theme.border
        Rectangle { x: 1; y: 2; width: 2; height: parent.height - 4; color: Theme.secondaryText }
        Rectangle { x: parent.width - 3; y: 2; width: 2; height: parent.height - 4; color: Theme.secondaryText }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        // 仅保存本次手势起点；显示范围始终读取现有 viewport。
        property real pressX: 0
        property real initialStart: 0
        property real initialEnd: 1
        property int mode: 0
        function hitMode(x) {
            const left = bar.rangeX
            const right = left + bar.rangeWidth
            if (Math.abs(x - left) <= 8 && Math.abs(x - left) <= Math.abs(x - right)) return 1
            if (Math.abs(x - right) <= 8) return 2
            return x > left && x < right ? 3 : 0
        }
        cursorShape: (pressed ? mode : hitMode(mouseX)) === 3
                     ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                     : (pressed ? mode : hitMode(mouseX)) > 0 ? Qt.SizeHorCursor : Qt.ArrowCursor
        onPressed: function(event) {
            mode = hitMode(event.x)
            if (!mode) { event.accepted = false; return }
            pressX = event.x
            initialStart = bar.startRatio
            initialEnd = bar.endRatio
        }
        onPositionChanged: function(event) {
            if (!pressed || bar.width <= 0) return
            const delta = (event.x - pressX) / bar.width
            if (mode === 1) {
                bar.timeline.setVisibleRange(Math.max(0, Math.min(initialEnd - bar.minimumRatio, initialStart + delta)), initialEnd)
            } else if (mode === 2) {
                bar.timeline.setVisibleRange(initialStart, Math.min(1, Math.max(initialStart + bar.minimumRatio, initialEnd + delta)))
            } else if (mode === 3) {
                const start = Math.max(0, Math.min(1 - (initialEnd - initialStart), initialStart + delta))
                bar.timeline.scrollOffset = start * bar.totalMs * bar.timeline.pixelsPerMs
            }
        }
    }
}
