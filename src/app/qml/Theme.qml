pragma Singleton
import QtQuick

QtObject {
    // ── 基础背景 ──────────────────────────────────────────────
    readonly property color background: "#15171C"       // AppBackground
    readonly property color titleBar: "#1B1E24"         // 原生标题栏 Caption Color
    readonly property color menuBar: "#1D2026"          // 顶部命令栏

    // ── 面板 ─────────────────────────────────────────────────
    readonly property color panel: "#1D2027"            // 媒体/文稿、字幕列表
    readonly property color panelSecondary: "#22252D"   // 斑马行、次级面板
    readonly property color panelHeader: "#20232A"      // Panel Header
    readonly property color panelRaised: "#252932"      // 弹窗、工具栏
    readonly property color previewBackground: "#090A0D" // 视频预览底
    readonly property color previewEmptyText: "#8A919D"  // 预览空状态文字

    // ── 时间轴 ───────────────────────────────────────────────
    readonly property color timelineBackground: "#181A20"
    readonly property color trackHeader: "#1D2026"      // 轨道头
    readonly property color ruler: "#1C1F25"            // 尺标
    readonly property color timelineGrid: "#292D35"     // 网格
    readonly property color timelineMajorGrid: "#343944" // 主网格/尺标刻度
    readonly property color trackDivider: "#30343D"     // 轨道分割
    readonly property color timeText: "#929AA6"         // 尺标时间文字

    // ── 边框 / 分割 ──────────────────────────────────────────
    readonly property color border: "#343944"
    readonly property color divider: "#2A2E36"

    // ── 文本层级 ─────────────────────────────────────────────
    readonly property color text: "#E7EAF0"             // PrimaryText（沿用旧名）
    readonly property color secondaryText: "#A6ADB9"
    readonly property color muted: "#7D8593"            // MutedText（沿用旧名）
    readonly property color disabledText: "#5F6672"
    readonly property color accentText: "#F0F3F7"       // 主按钮文字
    readonly property color listText: "#E2E5EB"         // 字幕列表正文
    readonly property color listTime: "#8C94A1"         // 字幕列表时间信息

    // ── 强调色 ───────────────────────────────────────────────
    readonly property color accent: "#4C8DFF"
    readonly property color accentHover: "#68A1FF"
    readonly property color accentPressed: "#3678E5"
    readonly property color selection: "#294D7A"
    readonly property color focusBorder: "#4C8DFF"

    // ── 危险色 ───────────────────────────────────────────────
    readonly property color danger: "#D9534F"
    readonly property color closeHover: "#C42B1C"

    // ── 按钮（普通） ─────────────────────────────────────────
    readonly property color button: "#272B33"
    readonly property color buttonBorder: "#3A404B"
    readonly property color buttonHover: "#313640"
    readonly property color buttonHoverBorder: "#4A5260"
    readonly property color buttonPressed: "#20242B"
    readonly property color buttonDisabled: "#202329"

    // ── 输入框 ───────────────────────────────────────────────
    readonly property color input: "#252932"
    readonly property color placeholder: "#737B89"

    // ── 顶栏菜单按钮 ─────────────────────────────────────────
    readonly property color menuButtonText: "#C9CED7"
    readonly property color menuButtonTextHover: "#F1F3F6"
    readonly property color menuButtonHover: "#292D35"
    readonly property color menuButtonPressed: "#323741"

    // ── 字幕列表 ─────────────────────────────────────────────
    readonly property color listHover: "#282D36"

    // ── 音频轨 ───────────────────────────────────────────────
    readonly property color audioTrack: "#1B1D23"
    readonly property color audioClip: "#3B2945"
    readonly property color audioClipHover: "#473151"
    readonly property color audioClipSelected: "#553763"
    readonly property color waveform: "#B565D9"         // 波形是唯一的紫色主角

    // ── 字幕轨 / 字幕块（蓝色系，与音频紫色区分） ─────────────
    readonly property color cue: "#294F6D"              // Subtitle Clip（沿用旧名）
    readonly property color cueHover: "#315E81"
    readonly property color cueSelected: "#376F9B"
    readonly property color cueText: "#F0F3F7"
    readonly property color trimHandle: "#68A1FF"

    // ── 播放头（保持红色语义） ───────────────────────────────
    readonly property color playhead: "#FF4D57"

    // ── 滚动条 ───────────────────────────────────────────────
    readonly property color scrollTrack: "#1A1D22"       // 时间轴缩放条底轨
    readonly property color scrollBarTrack: "#2F3540"    // 列表/设置滑动条轨道
    readonly property color scrollThumb: "#8B95A5"
    readonly property color scrollThumbHover: "#A4ADBB"
    readonly property color scrollThumbPressed: "#B8C0CC"

    // ── 状态色（沿用） ───────────────────────────────────────
    readonly property color success: "#66B77B"
    readonly property color warning: "#E7B34E"
    readonly property color error: "#E26767"

    readonly property string fontFamily: "Microsoft YaHei UI"
    readonly property int fontSize: 13
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeTiny: 11
    readonly property int radiusButton: 3
    readonly property int radiusInput: 3
    readonly property int radiusPopup: 4
    readonly property int radiusPanel: 0
    readonly property int scrollThumbSize: 10
    readonly property int scrollBarSize: 14
}
