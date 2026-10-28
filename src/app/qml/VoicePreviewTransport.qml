import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    property var player
    property string sourceTitle: qsTr("音频试听")
    property bool preparing: false
    signal stopRequested()
    Layout.fillWidth: true
    function clock(us) { return Math.floor(us / 60000000) + ":" + String(Math.floor(us / 1000000) % 60).padStart(2, "0") }
    Label { Layout.fillWidth: true; color: Theme.secondaryText; elide: Text.ElideMiddle; text: preparing || (player && player.loading) ? qsTr("正在准备试听…") : player && player.displaySource ? sourceTitle : qsTr("选择音频试听") }
    RowLayout {
        Layout.fillWidth: true
        SubButton { text: player && player.playing ? qsTr("暂停") : player && player.paused ? qsTr("继续") : qsTr("播放"); enabled: player && !!player.displaySource; onClicked: { if (player.playing) player.pause(); else if (player.paused) player.resume(); else player.replay() } }
        SubButton { text: qsTr("停止"); enabled: preparing || (player && (player.playing || player.paused || player.loading)); onClicked: { stopRequested(); player.stop() } }
        Slider { id: playbackPosition; Layout.fillWidth: true; from: 0; to: Math.max(1, player ? player.durationUs : 0); enabled: player && (player.playing || player.paused); value: player ? player.positionUs : 0; onPressedChanged: if (!pressed && enabled) player.seek(value) }
        Label { color: Theme.text; text: clock(playbackPosition.pressed ? playbackPosition.value : player ? player.positionUs : 0) + " / " + clock(player ? player.durationUs : 0) }
        SubButton { text: player && player.muted ? qsTr("取消静音") : qsTr("静音"); enabled: !!player; onClicked: player.muted = !player.muted }
        Slider { Layout.preferredWidth: 80; from: 0; to: 1; value: player ? player.volume : 1; onMoved: if (player) player.volume = value; ToolTip.visible: hovered; ToolTip.text: qsTr("试听音量 %1%").arg(Math.round(value * 100)) }
    }
}
