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
        border.color: theme.borderStrong
        border.width: 1
        radius: theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Smart Disk Budget & Eviction Policy")
                font.pixelSize: 18
                font.bold: true
                color: theme.textPrimary
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Done")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: root.close()
            }
        }

        Label {
            text: qsTr("WickFS keeps frequently accessed and pinned files on local disk while evicting cold files back to 0-byte cloud stubs whenever the cache exceeds your budget.")
            font.pixelSize: 13
            color: theme.textSecondary
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            color: theme.cardBg
            border.color: theme.border
            border.width: 1
            radius: theme.radius
            implicitHeight: budgetCol.implicitHeight + 24

            ColumnLayout {
                id: budgetCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Maximum Local Cache Size")
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.textPrimary
                        Layout.fillWidth: true
                    }
                    Label {
                        text: backend.maxCacheFormatted
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.accent
                    }
                }

                Slider {
                    id: maxCacheSlider
                    Layout.fillWidth: true
                    from: 50
                    to: 2048
                    stepSize: 10
                    value: Math.min(2048, backend.maxCacheMb)
                    onMoved: backend.maxCacheMb = Math.round(value)
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: [
                            { label: "120 MB", mb: 120 },
                            { label: "250 MB", mb: 250 },
                            { label: "500 MB", mb: 500 },
                            { label: "2 GB", mb: 2048 },
                            { label: "10 GB", mb: 10240 },
                            { label: "25 GB", mb: 25600 }
                        ]
                        delegate: Button {
                            text: modelData.label
                            flat: backend.maxCacheMb !== modelData.mb
                            highlighted: backend.maxCacheMb === modelData.mb
                            Material.roundedScale: theme.radius
                            Layout.fillWidth: true
                            implicitHeight: 30
                            font.pixelSize: 11
                            onClicked: backend.maxCacheMb = modelData.mb
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
            implicitHeight: floorCol.implicitHeight + 24

            ColumnLayout {
                id: floorCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Minimum Free Disk Space Floor")
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.textPrimary
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("%1 GB free (%2 available)").arg(backend.minFreeDiskGb).arg(backend.diskAvailableFormatted)
                        font.pixelSize: 12
                        color: theme.cyan
                    }
                }

                Slider {
                    Layout.fillWidth: true
                    from: 1
                    to: 100
                    stepSize: 1
                    value: backend.minFreeDiskGb
                    onMoved: backend.minFreeDiskGb = Math.round(value)
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Frequent Access Promotion Threshold")
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.textPrimary
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("%1+ opens").arg(backend.frequentThreshold)
                        font.pixelSize: 12
                        color: theme.green
                    }
                }

                Slider {
                    Layout.fillWidth: true
                    from: 1
                    to: 15
                    stepSize: 1
                    value: backend.frequentThreshold
                    onMoved: backend.frequentThreshold = Math.round(value)
                }

                CheckBox {
                    text: qsTr("Automatically keep frequently accessed files hydrated on disk")
                    checked: backend.autoKeepFrequent
                    onToggled: backend.autoKeepFrequent = checked
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                text: qsTr("Evict All Cold Files Now")
                Material.roundedScale: theme.radius
                onClicked: backend.evictColdFiles()
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Enforce Budget")
                highlighted: true
                Material.roundedScale: theme.radius
                onClicked: {
                    backend.enforceCacheBudget()
                    root.close()
                }
            }
        }
    }
}
