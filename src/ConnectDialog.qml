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

    onOpened: backend.refreshRemotes()

    background: Rectangle {
        color: theme.panelBg
        border.color: theme.borderStrong
        border.width: 1
        radius: theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Connect Google Drive & Remote Source")
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

        Rectangle {
            Layout.fillWidth: true
            color: theme.cardBg
            border.color: backend.rcloneAvailable ? theme.border : theme.red
            border.width: 1
            radius: theme.radius
            implicitHeight: rcloneCol.implicitHeight + 24

            ColumnLayout {
                id: rcloneCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Google Drive via rclone")
                        font.pixelSize: 14
                        font.bold: true
                        color: theme.textPrimary
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        color: backend.rcloneAvailable ? theme.green : theme.red
                        radius: Math.min(4, theme.radius)
                        implicitWidth: statusLbl.implicitWidth + 12
                        implicitHeight: 22
                        Label {
                            id: statusLbl
                            anchors.centerIn: parent
                            text: backend.rcloneAvailable ? qsTr("INSTALLED") : qsTr("MISSING RCLONE")
                            font.pixelSize: 10
                            font.bold: true
                            color: "black"
                        }
                    }
                }

                Label {
                    text: backend.rcloneAvailable
                          ? qsTr("Connect an existing rclone Google Drive remote or authorize a new Google account in your browser.")
                          : qsTr("Install rclone with 'omarchy-pkg-add rclone' to enable Google OAuth and remote cloud syncing.")
                    font.pixelSize: 12
                    color: theme.textSecondary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    TextField {
                        id: remoteNameField
                        Layout.fillWidth: true
                        text: backend.remoteName
                        placeholderText: qsTr("rclone remote name (e.g. gdrive)")
                        Material.roundedScale: theme.radius
                    }

                    TextField {
                        id: remotePathField
                        Layout.fillWidth: true
                        text: backend.remotePath
                        placeholderText: qsTr("Optional remote subfolder")
                        Material.roundedScale: theme.radius
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: qsTr("Authorize Google OAuth")
                        enabled: backend.rcloneAvailable
                        Material.roundedScale: theme.radius
                        onClicked: backend.launchRcloneAuth(remoteNameField.text)
                    }

                    Button {
                        text: qsTr("Connect Remote")
                        highlighted: true
                        enabled: backend.rcloneAvailable
                        Material.roundedScale: theme.radius
                        onClicked: {
                            if (backend.connectRcloneRemote(remoteNameField.text, remotePathField.text))
                                root.close()
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            color: theme.cardBg
            border.color: theme.border
            border.width: 1
            radius: theme.radius
            implicitHeight: mirrorCol.implicitHeight + 24

            ColumnLayout {
                id: mirrorCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label {
                    text: qsTr("Local Cloud Mirror Folder")
                    font.pixelSize: 14
                    font.bold: true
                    color: theme.textPrimary
                }

                Label {
                    text: qsTr("You can also link any local or network directory as the backing cloud store for ~/Drive.")
                    font.pixelSize: 12
                    color: theme.textSecondary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Button {
                    text: qsTr("Choose Backing Folder…")
                    Material.roundedScale: theme.radius
                    onClicked: {
                        root.close()
                        backend.pickLocalSourceFolder()
                    }
                }
            }
        }
    }
}
