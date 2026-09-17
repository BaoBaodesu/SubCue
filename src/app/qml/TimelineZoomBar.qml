import QtQuick

Rectangle {
    id: bar
    required property var timeline
    height: 24
    color: Theme.scrollTrack
    border.color: Theme.border
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    readonly property bool interacting: mouse.pressed
    readonly property real totalMs: Math.max(1, timeline.durationUs / 1000)
    readonly property real startRatio: Math.max(0, Math.min(1, timeline.scrollOffset / timeline.pixelsPerMs / totalMs))
    readonly property real endRatio: Math.max(startRatio, Math.min(1, (timeline.scrollOffset + timeline.width) / timeline.pixelsPerMs / totalMs))
    readonly property real spanRatio: Math.max(0, Math.min(1, endRatio - startRatio))
    readonly property real minimumRatio: Math.min(1, timeline.width / (10 * totalMs))
    readonly property real rangeWidth: Math.min(width, Math.max(40, width * spanRatio))
    readonly property real rangeX: spanRatio >= 1 ? 0 : Math.max(0, Math.min(width - rangeWidth,
        (width - rangeWidth) * startRatio / Math.max(0.000001, 1 - spanRatio)))

    Rectangle {
        x: bar.rangeX
        width: bar.rangeWidth
        height: parent.height
        color: mouse.pressed ? Theme.scrollThumbPressed : mouse.containsMouse ? Theme.scrollThumbHover : Theme.scrollThumb
        border.color: Theme.border
        Rectangle { x: 2; y: 4; width: 4; height: parent.height - 8; color: Theme.secondaryText }
        Rectangle { x: parent.width - 6; y: 4; width: 4; height: parent.height - 8; color: Theme.secondaryText }
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
            if (!mode) {
                const span = bar.endRatio - bar.startRatio
                const travel = Math.max(1, bar.width - bar.rangeWidth)
                const start = Math.max(0, Math.min(1 - span,
                    (event.x - bar.rangeWidth / 2) / travel * (1 - span)))
                bar.timeline.scrollOffset = start * bar.totalMs * bar.timeline.pixelsPerMs
                mode = 3
            }
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
                const span = initialEnd - initialStart
                const travel = Math.max(1, bar.width - bar.rangeWidth)
                const start = Math.max(0, Math.min(1 - span,
                    initialStart + (event.x - pressX) / travel * (1 - span)))
                bar.timeline.scrollOffset = start * bar.totalMs * bar.timeline.pixelsPerMs
            }
        }
    }
}
