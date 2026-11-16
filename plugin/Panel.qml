import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

Panel {
  id: root
  moduleName: "ben.wickfs"
  ipcTarget: "ben.wickfs"

  Commons.Translations {
    id: strings
    directory: Qt.resolvedUrl("i18n")
  }

  property var stateData: Model.parseStateJson("", Date.now())

  readonly property bool vertical: bar ? bar.vertical : false
  readonly property int barSize: bar ? bar.barSize : Style.bar.sizeHorizontal
  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  readonly property bool isSyncing: Boolean(stateData.syncing)
  readonly property color statusColor: isSyncing ? "#38bdf8" : "#34d399"
  readonly property color pinnedColor: "#818cf8"

  function tr(key, args) {
    var s = strings.t(key)
    if (!args) return s
    for (var i = 0; i < args.length; i++)
      s = s.replace("%" + (i + 1), String(args[i]))
    return s
  }

  readonly property string barStatusText: isSyncing
    ? root.tr("bar.syncing")
    : root.tr("bar.synced")

  readonly property string cachedText: Model.formatMb(stateData.cachedBytes, Commons.I18n.formatNumber)
  readonly property string maxCacheText: Model.formatMb(stateData.maxCacheBytes, Commons.I18n.formatNumber)
  readonly property string savedText: Model.formatMb(stateData.savedBytes, Commons.I18n.formatNumber)

  readonly property string detailText: {
    if (isSyncing) {
      var pct = Commons.I18n.formatNumber(Math.round((stateData.syncProgress || 0.45) * 100), 0)
      var file = Model.sanitizeText(stateData.activeSyncFile || "Drive", 42)
      return root.tr("panel.detail.syncing", [file, pct])
    }
    return root.tr("panel.detail.synced", [
      Commons.I18n.formatNumber(stateData.cachedFiles, 0),
      Commons.I18n.formatNumber(stateData.totalFiles, 0)
    ])
  }

  function refresh() {
    if (!stateProc.running) stateProc.running = true
  }

  function openWickfsApp() {
    if (root.bar) {
      root.bar.run("sh -c 'exec \"$(command -v wickfs || echo \"$HOME/.local/bin/wickfs\")\"'")
    } else if (!appProc.running) {
      appProc.running = true
    }
  }

  function triggerSync() {
    if (!syncProc.running) syncProc.running = true
  }

  function triggerEvictCold() {
    if (!evictProc.running) evictProc.running = true
  }

  onOpenedChanged: {
    if (opened) refresh()
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Process {
    id: stateProc
    command: [
      "sh", "-c",
      "cat \"$HOME/.local/share/wickfs/bar-status.json\" 2>/dev/null || cat \"$HOME/.local/share/wickfs/state.json\" 2>/dev/null || true"
    ]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.stateData = Model.parseStateJson(text, Date.now())
      }
    }
  }

  Process {
    id: syncProc
    command: ["sh", "-c", "wickfs sync >/dev/null 2>&1 || \"$HOME/.local/bin/wickfs\" sync >/dev/null 2>&1"]
    onExited: root.refresh()
  }

  Process {
    id: evictProc
    command: ["sh", "-c", "wickfs evict --cold >/dev/null 2>&1 || \"$HOME/.local/bin/wickfs\" evict --cold >/dev/null 2>&1"]
    onExited: root.refresh()
  }

  Process {
    id: appProc
    command: ["sh", "-c", "(wickfs || \"$HOME/.local/bin/wickfs\") >/dev/null 2>&1 &"]
  }

  Timer {
    id: pollTimer
    interval: 3000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    labelVisible: false
    hasVisualContent: true
    fixedWidth: root.vertical ? -1 : (pillRow.implicitWidth + Style.space(14))
    fixedHeight: root.vertical ? (verticalCol.implicitHeight + Style.space(10)) : -1
    pressFeedbackButtons: Qt.LeftButton | Qt.RightButton
    tooltipText: root.opened
      ? ""
      : Model.sanitizeText(
          root.tr("panel.title") + " — " + root.barStatusText + " (" + root.cachedText + " / " + root.maxCacheText + ")",
          140
        )

    onPressed: function(b) {
      if (b === Qt.RightButton) root.triggerSync()
      else if (b === Qt.MiddleButton) root.openWickfsApp()
      else root.toggle()
    }

    Rectangle {
      id: pillBg
      visible: !root.vertical
      anchors.centerIn: parent
      width: pillRow.implicitWidth + Style.space(10)
      height: Math.max(Style.space(20), root.barSize - Style.space(4))
      radius: Math.max(Style.space(7), Style.cornerRadius)
      scale: button.pressScale
      color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b,
                     root.opened || button.tooltipHovered ? 0.18 : 0.10)
      border.width: 1
      border.color: root.opened
        ? root.statusColor
        : Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.42)

      Behavior on border.color { ColorAnimation { duration: Style.duration(200) } }

      Row {
        id: pillRow
        anchors.centerIn: parent
        spacing: Style.space(5)

        Rectangle {
          width: Style.space(7)
          height: Style.space(7)
          radius: width / 2
          anchors.verticalCenter: parent.verticalCenter
          color: root.statusColor
          Behavior on color { ColorAnimation { duration: Style.duration(200) } }
        }

        Text {
          text: root.isSyncing ? "\udb80\udd90" : "\udb80\udd5f"
          textFormat: Text.PlainText
          color: root.statusColor
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          anchors.verticalCenter: parent.verticalCenter
          Behavior on color { ColorAnimation { duration: Style.duration(200) } }
        }

        Text {
          text: root.barStatusText
          textFormat: Text.PlainText
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          font.bold: true
          anchors.verticalCenter: parent.verticalCenter
        }

        Text {
          text: "· " + root.cachedText
          textFormat: Text.PlainText
          color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.72)
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          anchors.verticalCenter: parent.verticalCenter
        }
      }
    }

    Column {
      id: verticalCol
      visible: root.vertical
      anchors.centerIn: parent
      spacing: Style.space(3)
      scale: button.pressScale

      Rectangle {
        width: Style.space(6)
        height: Style.space(6)
        radius: width / 2
        anchors.horizontalCenter: parent.horizontalCenter
        color: root.statusColor
      }

      Text {
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.isSyncing ? "\udb80\udd90" : "\udb80\udd5f"
        textFormat: Text.PlainText
        color: root.statusColor
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
      }
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight)

    content: PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      implicitHeight: column.implicitHeight

      onActivateRequested: {
        root.openWickfsApp()
        root.close()
      }
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Keys.onPressed: function(event) {
        if (event.key === Qt.Key_R) {
          root.triggerSync()
          event.accepted = true
        }
      }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(10)

        // Header Row
        Item {
          width: parent.width
          implicitHeight: Math.max(heroBadge.height, heroCol.implicitHeight, openBtn.implicitHeight)

          Rectangle {
            id: heroBadge
            width: Style.space(34)
            height: Style.space(34)
            radius: Math.max(Style.space(8), Style.cornerRadius)
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.18)
            border.width: 1
            border.color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.55)

            Text {
              anchors.centerIn: parent
              textFormat: Text.PlainText
              text: root.isSyncing ? "\udb80\udd90" : "\udb80\udd5f"
              color: root.statusColor
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
            }
          }

          Column {
            id: heroCol
            anchors.left: heroBadge.right
            anchors.leftMargin: Style.space(10)
            anchors.right: openBtn.left
            anchors.rightMargin: Style.space(8)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              text: root.tr("panel.title")
              textFormat: Text.PlainText
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
              font.bold: true
              elide: Text.ElideRight
              width: parent.width
            }

            Text {
              text: root.isSyncing ? root.tr("panel.status.syncing") : root.tr("panel.status.synced")
              textFormat: Text.PlainText
              color: root.statusColor
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              elide: Text.ElideRight
              width: parent.width
            }
          }

          PanelActionButton {
            id: openBtn
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            iconText: "\udb80\udd8d"
            tooltipText: root.tr("panel.action.open")
            foreground: root.foreground
            hoverColor: root.statusColor
            fontFamily: root.fontFamily
            bordered: true
            onClicked: {
              root.openWickfsApp()
              root.close()
            }
          }
        }

        // Sync Status & Progress Card
        Rectangle {
          width: parent.width
          implicitHeight: syncCol.implicitHeight + Style.space(16)
          radius: Math.max(Style.space(8), Style.cornerRadius)
          color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.05)
          border.width: 1
          border.color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.35)

          Column {
            id: syncCol
            anchors.fill: parent
            anchors.margins: Style.space(8)
            spacing: Style.space(6)

            Text {
              text: root.detailText
              textFormat: Text.PlainText
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.bodySmall
              font.bold: true
              width: parent.width
              elide: Text.ElideRight
            }

            Rectangle {
              width: parent.width
              height: Style.space(6)
              radius: height / 2
              color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)

              Rectangle {
                width: Math.max(Style.space(8), parent.width * (root.isSyncing ? root.stateData.syncProgress : 1.0))
                height: parent.height
                radius: height / 2
                color: root.statusColor
                Behavior on width { NumberAnimation { duration: Style.duration(220); easing.type: Easing.OutCubic } }
              }
            }

            Row {
              width: parent.width
              spacing: Style.space(8)

              Rectangle {
                implicitWidth: syncBtnLbl.implicitWidth + Style.space(14)
                implicitHeight: Style.space(22)
                radius: Style.space(4)
                color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.18)
                border.width: 1
                border.color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.45)

                Text {
                  id: syncBtnLbl
                  anchors.centerIn: parent
                  text: root.tr("panel.action.sync")
                  textFormat: Text.PlainText
                  color: root.statusColor
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                  font.bold: true
                }

                MouseArea {
                  anchors.fill: parent
                  cursorShape: Qt.PointingHandCursor
                  onClicked: root.triggerSync()
                }
              }

              Rectangle {
                implicitWidth: evictBtnLbl.implicitWidth + Style.space(14)
                implicitHeight: Style.space(22)
                radius: Style.space(4)
                color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.08)
                border.width: 1
                border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.22)

                Text {
                  id: evictBtnLbl
                  anchors.centerIn: parent
                  text: root.tr("panel.action.evict")
                  textFormat: Text.PlainText
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                }

                MouseArea {
                  anchors.fill: parent
                  cursorShape: Qt.PointingHandCursor
                  onClicked: root.triggerEvictCold()
                }
              }
            }
          }
        }

        // Local Cache Budget Card
        Rectangle {
          width: parent.width
          implicitHeight: budgetCol.implicitHeight + Style.space(16)
          radius: Math.max(Style.space(8), Style.cornerRadius)
          color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.04)
          border.width: 1
          border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.14)

          Column {
            id: budgetCol
            anchors.fill: parent
            anchors.margins: Style.space(8)
            spacing: Style.space(5)

            Row {
              width: parent.width
              Text {
                text: root.tr("panel.cache.header")
                textFormat: Text.PlainText
                color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.65)
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                width: parent.width - budgetVal.implicitWidth
              }
              Text {
                id: budgetVal
                text: root.cachedText + " / " + root.maxCacheText
                textFormat: Text.PlainText
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
              }
            }

            Rectangle {
              width: parent.width
              height: Style.space(6)
              radius: height / 2
              color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)

              Rectangle {
                width: Math.min(parent.width, Math.max(0, parent.width * (root.stateData.cachedBytes / Math.max(1, root.stateData.maxCacheBytes))))
                height: parent.height
                radius: height / 2
                color: root.pinnedColor
                Behavior on width { NumberAnimation { duration: Style.duration(220); easing.type: Easing.OutCubic } }
              }
            }

            Text {
              text: root.tr("panel.cache.saved", [
                Commons.I18n.formatNumber(root.stateData.savedPct, 0),
                root.savedText
              ])
              textFormat: Text.PlainText
              color: root.statusColor
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }
        }

        // File Sync & Residency List
        Text {
          text: root.tr("panel.files.header")
          textFormat: Text.PlainText
          color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.65)
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          font.bold: true
        }

        Column {
          width: parent.width
          spacing: Style.space(4)

          Repeater {
            model: root.stateData.files
            Rectangle {
              required property var modelData
              width: parent.width
              implicitHeight: Style.space(26)
              radius: Style.space(4)
              color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.04)

              Row {
                anchors.fill: parent
                anchors.leftMargin: Style.space(8)
                anchors.rightMargin: Style.space(8)
                spacing: Style.space(6)

                Rectangle {
                  width: Style.space(6)
                  height: Style.space(6)
                  radius: width / 2
                  anchors.verticalCenter: parent.verticalCenter
                  color: modelData.pinned
                    ? root.pinnedColor
                    : (modelData.state === "cached" ? root.statusColor : Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.35))
                }

                Text {
                  text: modelData.name
                  textFormat: Text.PlainText
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                  width: parent.width - fileBadge.implicitWidth - Style.space(20)
                  elide: Text.ElideRight
                  anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                  id: fileBadge
                  text: modelData.pinned
                    ? root.tr("panel.badge.pinned")
                    : (modelData.state === "cached" ? root.tr("panel.badge.cached") : root.tr("panel.badge.cloud"))
                  textFormat: Text.PlainText
                  color: modelData.state === "cloud"
                    ? Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.55)
                    : root.statusColor
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                  font.bold: true
                  anchors.verticalCenter: parent.verticalCenter
                }
              }
            }
          }
        }
      }
    }
  }
}
