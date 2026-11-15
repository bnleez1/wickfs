import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: root
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, parent ? parent.width - 48 : 560)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 24

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    background: Rectangle {
        color: theme.panelBg
        border.color: theme.borderStrong
        border.width: 1
        radius: theme.radius
    }

    readonly property var shortcuts: [
        { keys: "Space", desc: qsTr("Hydrate on-demand and record access for selected file") },
        { keys: "Enter", desc: qsTr("Open selected file from ~/Drive in desktop application") },
        { keys: "P", desc: qsTr("Toggle Keep Offline (Pin / Unpin selected file)") },
        { keys: "E / Shift+E", desc: qsTr("Evict selected file / Evict all cold cached files") },
        { keys: "M", desc: qsTr("Mount or unmount ~/Drive FUSE3 virtual filesystem") },
        { keys: "S", desc: qsTr("Open Sync Status Panel Widget (syncing vs in-sync monitor)") },
        { keys: "R / F5", desc: qsTr("Sync catalog with Google Drive and enforce disk budget") },
        { keys: "B", desc: qsTr("Open disk space budget & LFU/LRU policy settings") },
        { keys: "C", desc: qsTr("Connect Google Drive (rclone OAuth or mirror folder)") },
        { keys: "O", desc: qsTr("Open ~/Drive mount folder in file manager") },
        { keys: "1 – 5", desc: qsTr("Filter: All, Frequent, Pinned, Cached, or Cloud Only") },
        { keys: "Up / Down / J / K", desc: qsTr("Navigate files in the list") },
        { keys: "/", desc: qsTr("Focus search bar") },
        { keys: "Ctrl+Z / Ctrl+Shift+Z", desc: qsTr("Undo / Redo cache or pin action") },
        { keys: "? / F1", desc: qsTr("Show or hide keyboard shortcuts") },
        { keys: "Q / Ctrl+Q", desc: qsTr("Quit GDrive") }
    ]

    contentItem: ColumnLayout {
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Keyboard Shortcuts")
                font.pixelSize: 18
                font.bold: true
                color: theme.textPrimary
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Close")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: root.close()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: root.shortcuts
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 164
                        Layout.preferredHeight: 26
                        color: theme.cardBg
                        border.color: theme.border
                        border.width: 1
                        radius: Math.min(4, theme.radius)

                        Label {
                            anchors.centerIn: parent
                            text: modelData.keys
                            font.pixelSize: 12
                            font.family: "monospace"
                            color: theme.accent
                        }
                    }

                    Label {
                        text: modelData.desc
                        font.pixelSize: 13
                        color: theme.textSecondary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
