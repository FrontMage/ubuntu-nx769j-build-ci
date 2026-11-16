// SPDX-License-Identifier: MIT
// NX769J 3-Mode Performance & Thermal Profile Switcher row for QuickSettings:
//   省电 (powersave / Eco) | 均衡 (balanced) | 狂暴 (performance / Diablo-Turbo)
// Displays live SM8650 CPU °C & 25 kHz Turbo Fan RPM; tap any pill to select
// directly or tap header to cycle modes.
import QtQuick
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Readable.js" as Readable

Column {
  id: root
  objectName: "nx769jPerfModeRow"

  property string mode: "balanced"
  property string labelZh: "均衡"
  property real cpuTempC: -1
  property real gpuTempC: -1
  property int fanRpm: 0
  property int fanDutyPercent: 0
  property bool busy: false
  property color foreground: Commons.Color.popups.text

  signal modeSelected(string modeId)
  signal cycleRequested()

  spacing: Style.spacing.sm

  readonly property color activeAccent: mode === "performance"
    ? Commons.Color.urgent
    : Commons.Color.accent

  readonly property string modeGlyph: mode === "powersave" ? "󰌪"
    : mode === "performance" ? "󰓅" : "󰾅"

  readonly property string titleText: "性能模式: " + (
    mode === "powersave" ? "省电"
    : mode === "performance" ? "狂暴"
    : "均衡"
  )

  readonly property string telemetryText: {
    const parts = []
    if (isFinite(cpuTempC) && cpuTempC > 0)
      parts.push("CPU " + Math.round(cpuTempC) + "°C")
    if (fanRpm > 0)
      parts.push("󰈐 " + fanRpm + " RPM")
    else
      parts.push("󰈐 0 RPM")
    return parts.join(" · ")
  }

  Item {
    id: headerBar
    width: parent.width
    implicitHeight: Math.max(24, Style.space(26))

    Row {
      id: titleRow
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.spacing.sm

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: root.modeGlyph
        color: root.activeAccent
        font.family: Style.font.family
        font.pixelSize: Style.font.title
      }

      Text {
        objectName: "perfModeTitle"
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: root.titleText
        color: root.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.subtitle
        font.bold: true
      }
    }

    Text {
      id: telemetryLabel
      objectName: "perfTelemetryText"
      anchors.right: parent.right
      anchors.left: titleRow.right
      anchors.leftMargin: Style.spacing.md
      anchors.verticalCenter: parent.verticalCenter
      horizontalAlignment: Text.AlignRight
      textFormat: Text.PlainText
      text: root.telemetryText
      color: root.fanRpm > 0 ? root.activeAccent : Qt.darker(root.foreground, 1.35)
      font.family: Style.font.family
      font.pixelSize: Readable.px(Style.font.caption)
      elide: Text.ElideLeft
      opacity: root.busy ? 0.65 : 1
    }

    TapHandler {
      gesturePolicy: TapHandler.DragThreshold
      cursorShape: Qt.PointingHandCursor
      onTapped: root.cycleRequested()
    }
  }

  Row {
    id: pillBar
    width: parent.width
    spacing: Style.spacing.sm
    readonly property real segWidth: (width - spacing * 2) / 3

    PerfSegment {
      objectName: "perfSeg-powersave"
      width: pillBar.segWidth
      segId: "powersave"
      segGlyph: "󰌪"
      segLabel: "省电"
      selected: root.mode === "powersave"
      segAccent: Commons.Color.accent
      foreground: root.foreground
      onPicked: root.modeSelected("powersave")
    }

    PerfSegment {
      objectName: "perfSeg-balanced"
      width: pillBar.segWidth
      segId: "balanced"
      segGlyph: "󰾅"
      segLabel: "均衡"
      selected: root.mode === "balanced"
      segAccent: Commons.Color.accent
      foreground: root.foreground
      onPicked: root.modeSelected("balanced")
    }

    PerfSegment {
      objectName: "perfSeg-performance"
      width: pillBar.segWidth
      segId: "performance"
      segGlyph: "󰓅"
      segLabel: "狂暴"
      selected: root.mode === "performance"
      segAccent: Commons.Color.urgent
      foreground: root.foreground
      onPicked: root.modeSelected("performance")
    }
  }

  component PerfSegment: BorderSurface {
    id: seg

    property string segId: ""
    property string segGlyph: ""
    property string segLabel: ""
    property bool selected: false
    property color segAccent: Commons.Color.accent
    property color foreground: Commons.Color.popups.text

    signal picked()

    implicitHeight: Math.max(42, Style.space(44))
    radius: Style.cornerRadius

    readonly property var _activeBorder: Border.flat(
      segAccent,
      Math.max(1, Style.selectedBorderWidth || Style.normalBorderWidth)
    )

    color: segTap.pressed
      ? Style.pressedFillFor(foreground, segAccent)
      : selected
        ? Util.alpha(segAccent, Style.selectedFillAlpha)
        : Style.normalFillFor(foreground, segAccent)
    borderSpec: selected ? _activeBorder : Border.controlSpec("normal", foreground, segAccent)

    Behavior on color { ColorAnimation { duration: 120 } }

    Row {
      anchors.centerIn: parent
      spacing: Style.spacing.xs

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: seg.segGlyph
        color: seg.selected ? seg.segAccent : seg.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.subtitle
      }

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: seg.segLabel
        color: seg.selected ? seg.segAccent : seg.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.subtitle
        font.bold: seg.selected
      }
    }

    TapHandler {
      id: segTap
      gesturePolicy: TapHandler.DragThreshold
      cursorShape: Qt.PointingHandCursor
      onTapped: seg.picked()
    }
  }
}
