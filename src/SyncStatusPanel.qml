import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: root
    anchors.centerIn: Overlay.overlay
    width: Math.min(540, parent ? parent.width - 48 : 540)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 24

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    background: Rectangle {
        color: theme.panelBg
        border.color: backend.syncing ? theme.cyan : theme.green
        border.width: 1
        radius: theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                width: 12
                height: 12
                radius: 6
                color: backend.syncing ? theme.cyan : theme.green
            }

            Label {
                text: backend.syncStateLabel
                font.pixelSize: 18
                font.bold: true
                color: theme.textPrimary
                Layout.fillWidth: true
            }

            Rectangle {
                color: backend.syncing ? theme.cyan : theme.green
                radius: Math.min(4, theme.radius)
                implicitHeight: 22
                implicitWidth: pillLbl.implicitWidth + 14

                Label {
                    id: pillLbl
                    anchors.centerIn: parent
                    text: backend.syncing ? qsTr("SYNCING") : qsTr("IN SYNC")
                    font.pixelSize: 10
                    font.bold: true
                    color: "black"
                }
            }

            Button {
                text: qsTr("Close")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: root.close()
            }
        }

        // Sync Progress & Status Card
        Rectangle {
            Layout.fillWidth: true
            color: theme.cardBg
            border.color: theme.border
            border.width: 1
            radius: theme.radius
            implicitHeight: syncCardCol.implicitHeight + 24

            ColumnLayout {
                id: syncCardCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: backend.syncDetailText
                        font.pixelSize: 13
                        font.bold: true
                        color: backend.syncing ? theme.cyan : theme.green
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Label {
                        text: backend.connectionSummary
                        font.pixelSize: 12
                        color: theme.textSecondary
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 8
                    radius: Math.min(4, theme.radius)
                    color: theme.inputBg
                    border.color: theme.border
                    border.width: 1
                    clip: true

                    Rectangle {
                        width: Math.round(parent.width * (backend.syncing ? backend.syncProgress : 1.0))
                        height: parent.height
                        radius: Math.min(4, theme.radius)
                        color: backend.syncing ? theme.cyan : theme.green
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    Label {
                        text: qsTr("Synced Files: %1 / %2").arg(backend.syncedFileCount).arg(backend.totalFileCount)
                        font.pixelSize: 12
                        color: theme.textPrimary
                    }

                    Label {
                        text: qsTr("Cached Locally: %1 (%2)").arg(backend.cachedFileCount).arg(backend.cachedFormatted)
                        font.pixelSize: 12
                        color: theme.textSecondary
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        text: qsTr("Cloud Stubs: %1").arg(backend.cloudOnlyFileCount)
                        font.pixelSize: 12
                        color: theme.textMuted
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: theme.border
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: theme.green
                    }

                    Label {
                        text: backend.autoStartSummary
                        font.pixelSize: 12
                        color: theme.textPrimary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        color: theme.inputBg
                        border.color: theme.green
                        border.width: 1
                        radius: Math.min(3, theme.radius)
                        implicitHeight: 20
                        implicitWidth: autoBadgeLbl.implicitWidth + 10

                        Label {
                            id: autoBadgeLbl
                            anchors.centerIn: parent
                            text: qsTr("BOOT + WAKE")
                            font.pixelSize: 9
                            font.bold: true
                            color: theme.green
                        }
                    }
                }
            }
        }

        // Recent Sync Events
        Label {
            text: qsTr("RECENT SYNC & HYDRATION EVENTS")
            font.pixelSize: 11
            font.bold: true
            color: theme.textMuted
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 170
            clip: true
            spacing: 6
            model: backend.activityLog

            delegate: Rectangle {
                width: ListView.view.width
                height: 48
                radius: Math.min(4, theme.radius)
                color: theme.cardBg
                border.color: theme.border
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: modelData.badge === "SYNC" || modelData.badge === "READY"
                               ? theme.green
                               : (modelData.badge === "FETCH" ? theme.cyan : theme.accent)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1

                        Label {
                            text: modelData.action
                            font.pixelSize: 12
                            font.bold: true
                            color: theme.textPrimary
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Label {
                            text: modelData.detail
                            font.pixelSize: 11
                            color: theme.textSecondary
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        text: modelData.time
                        font.pixelSize: 10
                        font.family: "monospace"
                        color: theme.textMuted
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                text: backend.syncing ? qsTr("Complete Sync") : qsTr("Simulate Syncing State")
                Material.roundedScale: theme.radius
                onClicked: {
                    if (backend.syncing)
                        backend.finishSyncSimulation()
                    else
                        backend.beginSyncSimulation(backend.selectedFile.name || "Q4-Roadmap-Deck.pptx", 1)
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Sync Now (R)")
                highlighted: true
                Material.roundedScale: theme.radius
                onClicked: backend.syncNow()
            }
        }
    }
}
