import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1120
    height: 740
    minimumWidth: 780
    minimumHeight: 540
    visible: true
    title: qsTr("GDrive")
    color: theme.windowBg

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property real lastPointerX: width / 2
    property real lastPointerY: height / 2
    property bool pointerOverWorkspace: false

    function categoryBadgeText(cat) {
        if (cat === "spreadsheet") return "XLS"
        if (cat === "presentation") return "DECK"
        if (cat === "image") return "IMG"
        if (cat === "video") return "VID"
        if (cat === "audio") return "WAV"
        if (cat === "archive") return "ARC"
        if (cat === "code") return "CODE"
        return "DOC"
    }

    function stateBadgeLabel(state, isFrequent) {
        if (state === "pinned") return qsTr("PINNED OFFLINE")
        if (state === "cached" && isFrequent) return qsTr("FREQUENT · CACHED")
        if (state === "cached") return qsTr("CACHED ON DISK")
        return qsTr("CLOUD ONLY · 0 B")
    }

    function stateBadgeColor(state, isFrequent) {
        if (state === "pinned") return theme.accent
        if (state === "cached" && isFrequent) return theme.green
        if (state === "cached") return theme.cyan
        return theme.borderStrong
    }

    function stateBadgeFg(state, isFrequent) {
        if (state === "pinned") return theme.accentForeground
        if (state === "cached") return "black"
        return theme.textSecondary
    }

    Shortcut {
        sequences: ["?", "F1"]
        context: Qt.ApplicationShortcut
        onActivated: shortcutOverlay.visible ? shortcutOverlay.close() : shortcutOverlay.open()
    }
    Shortcut {
        sequences: ["Q", "Ctrl+Q"]
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: Qt.quit()
    }
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.primaryAction()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus && !budgetDialog.visible && !connectDialog.visible
        onActivated: backend.openSelectedInDesktop()
    }
    Shortcut {
        sequence: "P"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.togglePinSelected()
    }
    Shortcut {
        sequence: "E"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.evictSelected()
    }
    Shortcut {
        sequence: "Shift+E"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.evictColdFiles()
    }
    Shortcut {
        sequence: "M"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.toggleMount()
    }
    Shortcut {
        sequences: ["R", "F5"]
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.syncNow()
    }
    Shortcut {
        sequence: "B"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: budgetDialog.visible ? budgetDialog.close() : budgetDialog.open()
    }
    Shortcut {
        sequence: "C"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: connectDialog.visible ? connectDialog.close() : connectDialog.open()
    }
    Shortcut {
        sequence: "O"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.openMountFolderInDesktop()
    }
    Shortcut {
        sequence: "/"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (searchField.activeFocus) {
                searchField.text = ""
                fileListView.forceActiveFocus()
            }
        }
    }
    Shortcut {
        sequence: "1"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.filterMode = "all"
    }
    Shortcut {
        sequence: "2"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.filterMode = "frequent"
    }
    Shortcut {
        sequence: "3"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.filterMode = "pinned"
    }
    Shortcut {
        sequence: "4"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.filterMode = "cached"
    }
    Shortcut {
        sequence: "5"
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.filterMode = "cloud"
    }
    Shortcut {
        sequences: ["Down", "J"]
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.selectNext()
    }
    Shortcut {
        sequences: ["Up", "K"]
        context: Qt.ApplicationShortcut
        enabled: !searchField.activeFocus
        onActivated: backend.selectPrevious()
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        onActivated: backend.undo()
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        context: Qt.ApplicationShortcut
        onActivated: backend.redo()
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: rootMirror
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        HoverHandler {
            onPointChanged: {
                win.lastPointerX = point.position.x
                win.lastPointerY = point.position.y
                win.pointerOverWorkspace = true
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            // 1. Top Header Bar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 58
                color: theme.panelBg
                border.color: theme.border
                border.width: 1
                radius: theme.radius

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 12

                    Rectangle {
                        width: 34
                        height: 34
                        radius: Math.min(6, theme.radius)
                        color: theme.accent

                        Label {
                            anchors.centerIn: parent
                            text: "GD"
                            font.pixelSize: 14
                            font.bold: true
                            color: theme.accentForeground
                        }
                    }

                    ColumnLayout {
                        spacing: 1
                        Label {
                            text: qsTr("GDrive Smart Cache")
                            font.pixelSize: 16
                            font.bold: true
                            color: theme.textPrimary
                        }
                        Label {
                            text: backend.connectionSummary
                            font.pixelSize: 11
                            color: theme.textSecondary
                        }
                    }

                    // Mount status pill
                    Rectangle {
                        color: backend.mounted ? theme.cardSelectedBg : theme.cardBg
                        border.color: backend.mounted ? theme.accent : theme.borderStrong
                        border.width: 1
                        radius: Math.min(4, theme.radius)
                        implicitHeight: 30
                        implicitWidth: mountRow.implicitWidth + 20

                        RowLayout {
                            id: mountRow
                            anchors.centerIn: parent
                            spacing: 6

                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: backend.mounted ? theme.green : theme.yellow
                            }

                            Label {
                                text: backend.mountStatusText
                                font.pixelSize: 12
                                font.bold: true
                                color: theme.textPrimary
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.toggleMount()
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: qsTr("Open ~/Drive")
                        flat: true
                        Material.roundedScale: theme.radius
                        onClicked: backend.openMountFolderInDesktop()
                    }

                    Button {
                        text: qsTr("Sync")
                        flat: true
                        Material.roundedScale: theme.radius
                        onClicked: backend.syncNow()
                    }

                    Button {
                        text: qsTr("Budget")
                        flat: true
                        Material.roundedScale: theme.radius
                        onClicked: budgetDialog.open()
                    }

                    Button {
                        text: qsTr("Connect")
                        highlighted: true
                        Material.roundedScale: theme.radius
                        onClicked: connectDialog.open()
                    }

                    Button {
                        text: "?"
                        flat: true
                        implicitWidth: 36
                        Material.roundedScale: theme.radius
                        onClicked: shortcutOverlay.open()
                    }
                }
            }

            // 2. Status Banner (reports missing tools or active connection info)
            Rectangle {
                Layout.fillWidth: true
                visible: backend.statusBannerText.length > 0
                implicitHeight: 38
                color: backend.statusBannerIsError ? "#2b161b" : theme.panelBg
                border.color: backend.statusBannerIsError ? theme.red : theme.borderStrong
                border.width: 1
                radius: theme.radius

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 10

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: backend.statusBannerIsError ? theme.red : theme.accent
                    }

                    Label {
                        text: backend.statusBannerText
                        font.pixelSize: 12
                        color: theme.textPrimary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Button {
                        text: qsTr("Configure")
                        flat: true
                        implicitHeight: 28
                        font.pixelSize: 11
                        Material.roundedScale: theme.radius
                        onClicked: connectDialog.open()
                    }
                }
            }

            // 3. Smart Disk Space & LFU/LRU Budget Strip
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 96
                color: theme.panelBg
                border.color: theme.border
                border.width: 1
                radius: theme.radius

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Label {
                            text: qsTr("Local Disk Cache: %1 / %2")
                                  .arg(backend.cachedFormatted)
                                  .arg(backend.effectiveBudgetFormatted)
                            font.pixelSize: 13
                            font.bold: true
                            color: theme.textPrimary
                        }

                        Label {
                            text: qsTr("Pinned Offline: %1").arg(backend.pinnedFormatted)
                            font.pixelSize: 12
                            color: theme.accent
                        }

                        Label {
                            text: qsTr("Frequent / Hot Cache: %1").arg(backend.hotCachedFormatted)
                            font.pixelSize: 12
                            color: theme.green
                        }

                        Item { Layout.fillWidth: true }

                        Rectangle {
                            color: theme.cardSelectedBg
                            border.color: theme.accent
                            border.width: 1
                            radius: Math.min(4, theme.radius)
                            implicitHeight: 24
                            implicitWidth: savedLabel.implicitWidth + 16

                            Label {
                                id: savedLabel
                                anchors.centerIn: parent
                                text: {
                                    void(translator.language)
                                    return qsTr("%1% Disk Space Saved (%2 in Cloud)")
                                        .arg(Number(backend.spaceSavedPercent).toLocaleString(Qt.locale(), 'f', 0))
                                        .arg(backend.savedFormatted)
                                }
                                font.pixelSize: 11
                                font.bold: true
                                color: theme.accent
                            }
                        }
                    }

                    // Segmented progress bar showing Pinned + Hot Cached vs Budget Ceiling
                    Rectangle {
                        id: gaugeTrack
                        Layout.fillWidth: true
                        height: 12
                        radius: Math.min(4, theme.radius)
                        color: theme.inputBg
                        border.color: theme.border
                        border.width: 1
                        clip: true

                        Row {
                            anchors.fill: parent

                            Rectangle {
                                width: Math.round(gaugeTrack.width * backend.pinnedUsageRatio)
                                height: parent.height
                                color: theme.accent
                            }

                            Rectangle {
                                width: Math.max(0, Math.round(gaugeTrack.width * (backend.cacheUsageRatio - backend.pinnedUsageRatio)))
                                height: parent.height
                                color: theme.green
                            }
                        }
                    }

                    // Interactive Max Cache Ceiling slider + Reclaim button
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label {
                            text: qsTr("Max Cache Budget:")
                            font.pixelSize: 12
                            color: theme.textSecondary
                        }

                        Slider {
                            id: inlineBudgetSlider
                            Layout.preferredWidth: 220
                            implicitHeight: 22
                            from: 60
                            to: 1000
                            stepSize: 10
                            value: Math.min(1000, backend.maxCacheMb)
                            onMoved: backend.maxCacheMb = Math.round(value)
                        }

                        Label {
                            text: backend.maxCacheFormatted
                            font.pixelSize: 12
                            font.bold: true
                            color: theme.accent
                            Layout.preferredWidth: 72
                        }

                        Label {
                            text: qsTr("Free Disk: %1 (Floor: %2 GB)")
                                  .arg(backend.diskAvailableFormatted)
                                  .arg(backend.minFreeDiskGb)
                            font.pixelSize: 12
                            color: backend.lowDiskFloorActive ? theme.yellow : theme.textMuted
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Button {
                            text: qsTr("Evict Cold Files")
                            flat: true
                            implicitHeight: 28
                            font.pixelSize: 11
                            Material.roundedScale: theme.radius
                            onClicked: backend.evictColdFiles()
                        }
                    }
                }
            }

            // 4. Main Split Workspace
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                // Left Sidebar: Smart Filters & Folders
                Rectangle {
                    Layout.preferredWidth: 210
                    Layout.minimumWidth: 195
                    Layout.maximumWidth: 220
                    Layout.fillWidth: false
                    Layout.fillHeight: true
                    color: theme.panelBg
                    border.color: theme.border
                    border.width: 1
                    radius: theme.radius

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        Label {
                            text: qsTr("SMART VIEWS")
                            font.pixelSize: 11
                            font.bold: true
                            color: theme.textMuted
                        }

                        Repeater {
                            model: [
                                { mode: "all", key: "1", label: qsTr("All Drive Files"), count: backend.totalFileCount },
                                { mode: "frequent", key: "2", label: qsTr("Frequently Accessed"), count: backend.frequentFileCount },
                                { mode: "pinned", key: "3", label: qsTr("Pinned Offline"), count: backend.pinnedFileCount },
                                { mode: "cached", key: "4", label: qsTr("Cached on Disk"), count: backend.cachedFileCount },
                                { mode: "cloud", key: "5", label: qsTr("Cloud Only (0 B)"), count: backend.cloudOnlyFileCount }
                            ]

                            delegate: Rectangle {
                                Layout.fillWidth: true
                                height: 34
                                radius: Math.min(4, theme.radius)
                                color: backend.filterMode === modelData.mode ? theme.cardSelectedBg : "transparent"
                                border.color: backend.filterMode === modelData.mode ? theme.accent : "transparent"
                                border.width: 1

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    Rectangle {
                                        width: 18
                                        height: 18
                                        radius: Math.min(3, theme.radius)
                                        color: theme.cardBg
                                        border.color: theme.border
                                        border.width: 1
                                        Label {
                                            anchors.centerIn: parent
                                            text: modelData.key
                                            font.pixelSize: 10
                                            font.family: "monospace"
                                            color: theme.textMuted
                                        }
                                    }

                                    Label {
                                        text: modelData.label
                                        font.pixelSize: 12
                                        font.bold: backend.filterMode === modelData.mode
                                        color: backend.filterMode === modelData.mode ? theme.textPrimary : theme.textSecondary
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        text: {
                                            void(translator.language)
                                            return Number(modelData.count).toLocaleString(Qt.locale(), 'f', 0)
                                        }
                                        font.pixelSize: 11
                                        color: theme.accent
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.filterMode = modelData.mode
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: theme.border
                            Layout.topMargin: 4
                            Layout.bottomMargin: 4
                        }

                        Label {
                            text: qsTr("FOLDERS")
                            font.pixelSize: 11
                            font.bold: true
                            color: theme.textMuted
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 30
                            radius: Math.min(4, theme.radius)
                            color: backend.currentFolder === "" ? theme.cardSelectedBg : "transparent"

                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                text: qsTr("All Folders")
                                font.pixelSize: 12
                                font.bold: backend.currentFolder === ""
                                color: backend.currentFolder === "" ? theme.textPrimary : theme.textSecondary
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.currentFolder = ""
                            }
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: backend.folders
                            spacing: 2

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 28
                                radius: Math.min(4, theme.radius)
                                color: backend.currentFolder === modelData ? theme.cardSelectedBg : "transparent"

                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    text: modelData
                                    font.pixelSize: 12
                                    color: backend.currentFolder === modelData ? theme.accent : theme.textSecondary
                                    elide: Text.ElideRight
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.currentFolder = modelData
                                }
                            }
                        }

                        Label {
                            text: qsTr("SORT BY")
                            font.pixelSize: 11
                            font.bold: true
                            color: theme.textMuted
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Repeater {
                                model: [
                                    { id: "hot", label: qsTr("Hot") },
                                    { id: "recent", label: qsTr("Recent") },
                                    { id: "size", label: qsTr("Size") },
                                    { id: "name", label: qsTr("Name") }
                                ]
                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    height: 26
                                    radius: Math.min(4, theme.radius)
                                    color: backend.sortMode === modelData.id ? theme.accent : theme.cardBg
                                    border.color: backend.sortMode === modelData.id ? theme.accent : theme.border
                                    border.width: 1

                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        font.pixelSize: 11
                                        font.bold: backend.sortMode === modelData.id
                                        color: backend.sortMode === modelData.id ? theme.accentForeground : theme.textSecondary
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: backend.sortMode = modelData.id
                                    }
                                }
                            }
                        }
                    }
                }

                // Center Panel: Virtual Drive File List
                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 420
                    Layout.fillHeight: true
                    color: theme.panelBg
                    border.color: theme.border
                    border.width: 1
                    radius: theme.radius

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            TextField {
                                id: searchField
                                Layout.fillWidth: true
                                placeholderText: qsTr("Search Drive files or folders (/ to focus)…")
                                text: backend.searchQuery
                                Material.roundedScale: theme.radius
                                onTextEdited: backend.searchQuery = text
                            }

                            Label {
                                text: {
                                    void(translator.language)
                                    return qsTr("%n files", "", backend.visibleFileCount)
                                }
                                font.pixelSize: 12
                                color: theme.textSecondary
                            }
                        }

                        ListView {
                            id: fileListView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 6
                            model: backend.fileModel
                            currentIndex: backend.selectedIndex

                            delegate: Item {
                                width: fileListView.width
                                height: 68

                                Rectangle {
                                    anchors.fill: parent
                                    radius: theme.radius
                                    color: index === backend.selectedIndex ? theme.cardSelectedBg : theme.cardBg
                                    border.color: index === backend.selectedIndex ? theme.accent : theme.border
                                    border.width: index === backend.selectedIndex ? 2 : 1

                                    MouseArea {
                                        anchors.fill: parent
                                        preventStealing: true
                                        onClicked: backend.selectedIndex = index
                                        onDoubleClicked: {
                                            backend.selectedIndex = index
                                            backend.primaryAction()
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 12
                                        spacing: 12

                                        // Category Icon Pill
                                        Rectangle {
                                            Layout.preferredWidth: 42
                                            Layout.preferredHeight: 42
                                            radius: Math.min(6, theme.radius)
                                            color: model.cached ? theme.panelBg : theme.inputBg
                                            border.color: model.pinned ? theme.accent : (model.isFrequent ? theme.green : theme.borderStrong)
                                            border.width: 1

                                            Label {
                                                anchors.centerIn: parent
                                                text: win.categoryBadgeText(model.category)
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: model.pinned ? theme.accent : (model.isFrequent ? theme.green : theme.textSecondary)
                                            }
                                        }

                                        // File Name & Path
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 3

                                            Label {
                                                text: model.name
                                                font.pixelSize: 13
                                                font.bold: true
                                                color: theme.textPrimary
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6

                                                Label {
                                                    text: model.folder
                                                    font.pixelSize: 11
                                                    font.family: "monospace"
                                                    color: theme.textMuted
                                                }

                                                Label {
                                                    text: "·"
                                                    font.pixelSize: 11
                                                    color: theme.textMuted
                                                }

                                                Label {
                                                    text: qsTr("Opened %1x · %2").arg(model.accessCount).arg(model.lastAccessedFormatted)
                                                    font.pixelSize: 11
                                                    color: model.isFrequent ? theme.green : theme.textSecondary
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }
                                            }
                                        }

                                        // Hotness / Retention Score Meter
                                        ColumnLayout {
                                            Layout.preferredWidth: 86
                                            Layout.minimumWidth: 86
                                            Layout.maximumWidth: 86
                                            Layout.fillWidth: false
                                            spacing: 5

                                            Label {
                                                Layout.fillWidth: true
                                                horizontalAlignment: Text.AlignRight
                                                text: {
                                                    void(translator.language)
                                                    return qsTr("Score %1").arg(Number(model.hotScore).toLocaleString(Qt.locale(), 'f', 0))
                                                }
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: model.isFrequent ? theme.green : theme.textSecondary
                                            }

                                            Rectangle {
                                                Layout.fillWidth: true
                                                height: 6
                                                radius: 3
                                                color: theme.inputBg
                                                clip: true

                                                Rectangle {
                                                    width: Math.round(parent.width * Math.min(1.0, model.hotScore / 100.0))
                                                    height: parent.height
                                                    radius: 3
                                                    color: model.pinned ? theme.accent : (model.isFrequent ? theme.green : theme.cyan)
                                                }
                                            }
                                        }

                                        // Residency State Pill + Size
                                        ColumnLayout {
                                            Layout.preferredWidth: 136
                                            Layout.minimumWidth: 136
                                            Layout.maximumWidth: 136
                                            Layout.fillWidth: false
                                            spacing: 4

                                            Rectangle {
                                                Layout.alignment: Qt.AlignRight
                                                color: win.stateBadgeColor(model.state, model.isFrequent)
                                                radius: Math.min(4, theme.radius)
                                                implicitHeight: 20
                                                implicitWidth: badgeText.implicitWidth + 12

                                                Label {
                                                    id: badgeText
                                                    anchors.centerIn: parent
                                                    text: win.stateBadgeLabel(model.state, model.isFrequent)
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: win.stateBadgeFg(model.state, model.isFrequent)
                                                }
                                            }

                                            Label {
                                                Layout.alignment: Qt.AlignRight
                                                text: model.cached
                                                      ? qsTr("%1 on disk").arg(model.sizeFormatted)
                                                      : qsTr("%1 in cloud").arg(model.sizeFormatted)
                                                font.pixelSize: 11
                                                color: model.cached ? theme.textPrimary : theme.textMuted
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Right Inspector & Smart Cache Activity Log
                ColumnLayout {
                    Layout.preferredWidth: 295
                    Layout.minimumWidth: 275
                    Layout.maximumWidth: 305
                    Layout.fillWidth: false
                    Layout.fillHeight: true
                    spacing: 12

                    // Selected File Inspector Card
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 280
                        color: theme.panelBg
                        border.color: theme.border
                        border.width: 1
                        radius: theme.radius

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 8
                            visible: backend.selectedPath.length > 0

                            Label {
                                text: qsTr("FILE CACHE INSPECTOR")
                                font.pixelSize: 11
                                font.bold: true
                                color: theme.textMuted
                            }

                            Label {
                                text: backend.selectedFile.name || ""
                                font.pixelSize: 15
                                font.bold: true
                                color: theme.textPrimary
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Label {
                                text: "~/Drive/" + (backend.selectedFile.path || "")
                                font.pixelSize: 11
                                font.family: "monospace"
                                color: theme.accent
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: theme.border
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                rowSpacing: 5
                                columnSpacing: 8

                                Label { text: qsTr("Cloud Size:"); font.pixelSize: 12; color: theme.textSecondary }
                                Label {
                                    text: backend.selectedFile.sizeFormatted || "0 B"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: theme.textPrimary
                                    Layout.alignment: Qt.AlignRight
                                }

                                Label { text: qsTr("Local Disk Used:"); font.pixelSize: 12; color: theme.textSecondary }
                                Label {
                                    text: backend.selectedFile.cached ? (backend.selectedFile.sizeFormatted || "0 B") : qsTr("0 B (Cloud Stub)")
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: backend.selectedFile.cached ? theme.green : theme.textMuted
                                    Layout.alignment: Qt.AlignRight
                                }

                                Label { text: qsTr("Access Frequency:"); font.pixelSize: 12; color: theme.textSecondary }
                                Label {
                                    text: qsTr("%1 opens (%2)")
                                          .arg(backend.selectedFile.accessCount || 0)
                                          .arg(backend.selectedFile.lastAccessedFormatted || "")
                                    font.pixelSize: 12
                                    color: theme.textPrimary
                                    Layout.alignment: Qt.AlignRight
                                }

                                Label { text: qsTr("LFU/LRU Score:"); font.pixelSize: 12; color: theme.textSecondary }
                                Label {
                                    text: {
                                        void(translator.language)
                                        return Number(backend.selectedFile.hotScore || 0).toLocaleString(Qt.locale(), 'f', 1) + " / 100"
                                    }
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: (backend.selectedFile.isFrequent || false) ? theme.green : theme.cyan
                                    Layout.alignment: Qt.AlignRight
                                }
                            }

                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Button {
                                    Layout.fillWidth: true
                                    implicitHeight: 34
                                    font.pixelSize: 11
                                    leftPadding: 8
                                    rightPadding: 8
                                    text: (backend.selectedFile.cached || false) ? qsTr("Access (Space)") : qsTr("Hydrate (Space)")
                                    highlighted: true
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.primaryAction()
                                }

                                Button {
                                    Layout.fillWidth: true
                                    implicitHeight: 34
                                    font.pixelSize: 11
                                    leftPadding: 8
                                    rightPadding: 8
                                    text: (backend.selectedFile.pinned || false) ? qsTr("Unpin (P)") : qsTr("Pin (P)")
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.togglePinSelected()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Button {
                                    Layout.fillWidth: true
                                    implicitHeight: 34
                                    font.pixelSize: 11
                                    leftPadding: 8
                                    rightPadding: 8
                                    text: qsTr("Evict (E)")
                                    enabled: (backend.selectedFile.cached || false) && !(backend.selectedFile.pinned || false)
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.evictSelected()
                                }

                                Button {
                                    Layout.fillWidth: true
                                    implicitHeight: 34
                                    font.pixelSize: 11
                                    leftPadding: 8
                                    rightPadding: 8
                                    text: qsTr("Open (Enter)")
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.openSelectedInDesktop()
                                }
                            }
                        }
                    }

                    // Live Cache Activity Log
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: theme.panelBg
                        border.color: theme.border
                        border.width: 1
                        radius: theme.radius

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: qsTr("SMART CACHE ACTIVITY")
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: theme.textMuted
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: qsTr("Ctrl+Z Undo")
                                    font.pixelSize: 10
                                    color: backend.canUndo ? theme.accent : theme.textMuted
                                }
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 6
                                model: backend.activityLog

                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 52
                                    radius: Math.min(4, theme.radius)
                                    color: theme.cardBg
                                    border.color: theme.border
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        spacing: 2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6

                                            Rectangle {
                                                color: modelData.badge === "BUDGET" || modelData.badge === "EVICT"
                                                       ? theme.yellow
                                                       : (modelData.badge === "PIN" ? theme.accent : theme.green)
                                                radius: Math.min(3, theme.radius)
                                                implicitWidth: logBadgeLbl.implicitWidth + 8
                                                implicitHeight: 16

                                                Label {
                                                    id: logBadgeLbl
                                                    anchors.centerIn: parent
                                                    text: modelData.badge
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: "black"
                                                }
                                            }

                                            Label {
                                                text: modelData.action
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: theme.textPrimary
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: modelData.time
                                                font.pixelSize: 10
                                                font.family: "monospace"
                                                color: theme.textMuted
                                            }
                                        }

                                        Label {
                                            text: modelData.detail
                                            font.pixelSize: 11
                                            color: theme.textSecondary
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ShortcutOverlay {
        id: shortcutOverlay
    }

    BudgetDialog {
        id: budgetDialog
    }

    ConnectDialog {
        id: connectDialog
    }
}
