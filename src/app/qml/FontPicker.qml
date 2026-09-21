import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 系统字体选择：下拉列表 + 搜索，默认微软雅黑。
Item {
    id: control
    implicitHeight: 28
    implicitWidth: 220

    property string text: "Microsoft YaHei"

    readonly property var fontAliases: ({
        "Microsoft YaHei": ["微软雅黑", "雅黑"],
        "Microsoft YaHei UI": ["微软雅黑 UI"],
        "SimHei": ["黑体"],
        "SimSun": ["宋体"],
        "NSimSun": ["新宋体"],
        "KaiTi": ["楷体"],
        "FangSong": ["仿宋"],
        "Microsoft JhengHei": ["微软正黑体"],
        "DengXian": ["等线"]
    })

    property var allFamilies: []
    property var filteredFamilies: []

    function ensureFamilies() {
        if (!allFamilies || allFamilies.length === 0)
            allFamilies = Qt.fontFamilies()
        return allFamilies
    }

    function aliasNames(family) {
        const mapped = fontAliases[family]
        if (mapped)
            return mapped
        for (const key in fontAliases) {
            if (key.toLowerCase() === String(family).toLowerCase())
                return fontAliases[key]
        }
        return []
    }

    function displayLabel(family) {
        const aliases = aliasNames(family)
        if (aliases.length > 0 && aliases[0] !== family)
            return family + "（" + aliases[0] + "）"
        return family
    }

    function resolveFamily(name) {
        const families = ensureFamilies()
        const wanted = String(name || "").trim()
        if (!wanted)
            return ""
        const lower = wanted.toLowerCase()
        for (let i = 0; i < families.length; ++i) {
            if (families[i].toLowerCase() === lower)
                return families[i]
        }
        for (const family in fontAliases) {
            const names = [family].concat(fontAliases[family])
            let hit = false
            for (let n = 0; n < names.length; ++n) {
                if (names[n].toLowerCase() === lower) {
                    hit = true
                    break
                }
            }
            if (!hit)
                continue
            for (let i = 0; i < families.length; ++i) {
                if (families[i].toLowerCase() === family.toLowerCase())
                    return families[i]
            }
            for (let i = 0; i < families.length; ++i) {
                const aliases = fontAliases[family]
                for (let a = 0; a < aliases.length; ++a) {
                    if (families[i] === aliases[a])
                        return families[i]
                }
            }
        }
        return ""
    }

    function matchesQuery(family, query) {
        if (!query)
            return true
        const q = query.toLowerCase()
        if (family.toLowerCase().indexOf(q) >= 0)
            return true
        if (displayLabel(family).toLowerCase().indexOf(q) >= 0)
            return true
        const aliases = aliasNames(family)
        for (let i = 0; i < aliases.length; ++i) {
            if (aliases[i].toLowerCase().indexOf(q) >= 0)
                return true
        }
        return false
    }

    function refreshFilter() {
        const families = ensureFamilies()
        const query = searchField.text.trim()
        const matched = []
        for (let i = 0; i < families.length; ++i) {
            if (matchesQuery(families[i], query))
                matched.push(families[i])
        }
        matched.sort(function(a, b) { return a.localeCompare(b, "zh-CN") })
        const pins = []
        function pin(name) {
            const index = matched.indexOf(name)
            if (index >= 0) {
                matched.splice(index, 1)
                pins.push(name)
            }
        }
        pin(control.text)
        pin(resolveFamily("Microsoft YaHei"))
        pin(resolveFamily("微软雅黑"))
        filteredFamilies = pins.concat(matched)
    }

    function loadValue(value) {
        ensureFamilies()
        const preferred = String(value || "").trim() || "Microsoft YaHei"
        text = resolveFamily(preferred) || resolveFamily("Microsoft YaHei")
            || resolveFamily("微软雅黑") || preferred
    }

    function selectFamily(family) {
        if (!family)
            return
        text = family
        popup.close()
    }

    function openPicker() {
        if (!control.enabled)
            return
        searchField.text = ""
        refreshFilter()
        popup.open()
        const current = filteredFamilies.indexOf(control.text)
        fontList.currentIndex = current >= 0 ? current : 0
        if (fontList.currentIndex >= 0)
            fontList.positionViewAtIndex(fontList.currentIndex, ListView.Contain)
        searchField.forceActiveFocus()
    }

    Component.onCompleted: loadValue(text)
    activeFocusOnTab: true

    Rectangle {
        id: field
        anchors.fill: parent
        radius: Theme.radiusInput
        color: control.enabled ? Theme.input : Theme.buttonDisabled
        border.width: 1
        border.color: !control.enabled ? Theme.border
                     : (control.activeFocus || popup.visible) ? Theme.focusBorder
                     : Theme.border

        Text {
            anchors.left: parent.left
            anchors.right: indicator.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            text: control.displayLabel(control.text)
            font.family: control.text || Theme.fontFamily
            font.pixelSize: Theme.fontSize
            elide: Text.ElideRight
            color: control.enabled ? Theme.text : Theme.disabledText
        }

        Text {
            id: indicator
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            color: control.enabled ? Theme.muted : Theme.disabledText
        }

        MouseArea {
            anchors.fill: parent
            enabled: control.enabled
            onClicked: {
                control.forceActiveFocus()
                control.openPicker()
            }
        }
    }

    Keys.onPressed: function(event) {
        if (!control.enabled)
            return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space || event.key === Qt.Key_Down) {
            openPicker()
            event.accepted = true
        }
    }

    Popup {
        id: popup
        parent: Overlay.overlay ? Overlay.overlay : control
        padding: 8
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onAboutToShow: {
            if (!parent)
                return
            const pos = control.mapToItem(parent, 0, control.height)
            x = pos.x
            y = pos.y + 2
            width = Math.max(control.width, 280)
        }

        background: Rectangle {
            radius: Theme.radiusPopup
            color: Theme.input
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 8
            SubTextField {
                id: searchField
                objectName: "fontFamilySearch"
                Layout.fillWidth: true
                placeholderText: "搜索系统字体"
                onTextChanged: {
                    control.refreshFilter()
                    fontList.currentIndex = filteredFamilies.length > 0 ? 0 : -1
                }
                onAccepted: control.selectFamily(filteredFamilies[Math.max(0, fontList.currentIndex)])
                Keys.onDownPressed: {
                    if (filteredFamilies.length > 0) {
                        fontList.forceActiveFocus()
                        fontList.currentIndex = Math.max(0, fontList.currentIndex)
                    }
                }
            }
            ListView {
                id: fontList
                objectName: "fontFamilyList"
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: filteredFamilies
                currentIndex: 0
                ScrollBar.vertical: SubScrollBar { }
                Keys.onReturnPressed: control.selectFamily(filteredFamilies[currentIndex])
                Keys.onEnterPressed: control.selectFamily(filteredFamilies[currentIndex])
                delegate: ItemDelegate {
                    id: fontRow
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: 28
                    highlighted: fontList.currentIndex === index
                    contentItem: Text {
                        text: control.displayLabel(fontRow.modelData)
                        font.family: fontRow.modelData
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        color: fontRow.highlighted ? Theme.text : Theme.secondaryText
                    }
                    background: Rectangle {
                        color: fontRow.highlighted ? Theme.selection
                             : fontRow.hovered ? Theme.listHover
                             : "transparent"
                    }
                    onClicked: control.selectFamily(fontRow.modelData)
                    onHoveredChanged: if (hovered) fontList.currentIndex = fontRow.index
                }

                Label {
                    anchors.centerIn: parent
                    visible: fontList.count === 0
                    text: "没有匹配的字体"
                    color: Theme.muted
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }
}
