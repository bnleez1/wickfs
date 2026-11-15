import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: root
    anchors.centerIn: Overlay.overlay
    width: Math.min(580, parent ? parent.width - 48 : 580)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 24

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    onOpened: {
        backend.refreshRemotes()
        clientIdField.text = backend.oauthClientId
        clientSecretField.text = backend.oauthClientSecret
    }

    background: Rectangle {
        color: theme.panelBg
        border.color: theme.borderStrong
        border.width: 1
        radius: theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 14

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
            border.color: backend.oauthAuthorized ? theme.green : (backend.rcloneAvailable ? theme.border : theme.red)
            border.width: 1
            radius: theme.radius
            implicitHeight: rcloneCol.implicitHeight + 24

            ColumnLayout {
                id: rcloneCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: qsTr("Google Drive via rclone")
                        font.pixelSize: 14
                        font.bold: true
                        color: theme.textPrimary
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        color: backend.oauthAuthorized ? theme.green : theme.cyan
                        radius: Math.min(4, theme.radius)
                        implicitWidth: authModeLbl.implicitWidth + 12
                        implicitHeight: 20
                        Label {
                            id: authModeLbl
                            anchors.centerIn: parent
                            text: backend.oauthClientModeLabel
                            font.pixelSize: 10
                            font.bold: true
                            color: "black"
                        }
                    }

                    Rectangle {
                        color: backend.rcloneAvailable ? theme.green : theme.red
                        radius: Math.min(4, theme.radius)
                        implicitWidth: statusLbl.implicitWidth + 12
                        implicitHeight: 20
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
                    text: backend.oauthStatusSummary
                    font.pixelSize: 12
                    font.bold: true
                    color: backend.oauthAuthorized ? theme.green : theme.textSecondary
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

                    TextField {
                        id: clientIdField
                        Layout.fillWidth: true
                        text: backend.oauthClientId
                        placeholderText: qsTr("Google OAuth Client ID (leave empty for built-in)")
                        Material.roundedScale: theme.radius
                    }

                    TextField {
                        id: clientSecretField
                        Layout.fillWidth: true
                        text: backend.oauthClientSecret
                        echoMode: TextInput.Password
                        placeholderText: qsTr("Google OAuth Client Secret (optional)")
                        Material.roundedScale: theme.radius
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: qsTr("Import client_secret.json…")
                        Material.roundedScale: theme.radius
                        onClicked: backend.pickGoogleClientSecretJson(remoteNameField.text)
                    }

                    Button {
                        text: qsTr("Save Client ID")
                        Material.roundedScale: theme.radius
                        onClicked: backend.configureOAuthClient(remoteNameField.text, clientIdField.text, clientSecretField.text)
                    }

                    Button {
                        text: qsTr("GCP Console")
                        flat: true
                        Material.roundedScale: theme.radius
                        onClicked: backend.openGoogleCloudConsole()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: qsTr("Authorize Google OAuth")
                        enabled: backend.rcloneAvailable
                        Material.roundedScale: theme.radius
                        onClicked: {
                            backend.configureOAuthClient(remoteNameField.text, clientIdField.text, clientSecretField.text)
                            backend.launchRcloneAuth(remoteNameField.text)
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: qsTr("Connect Remote")
                        highlighted: true
                        enabled: backend.rcloneAvailable
                        Material.roundedScale: theme.radius
                        onClicked: {
                            backend.configureOAuthClient(remoteNameField.text, clientIdField.text, clientSecretField.text)
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
