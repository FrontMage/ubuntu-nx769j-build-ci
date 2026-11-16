// SPDX-License-Identifier: MIT
// RedMagic 9 Pro (NX769J) 3-Mode Performance & Turbo Cooling Fan Home Widget.
// Shows active profile (省电 / 均衡 / 狂暴), live Fan RPM, and SM8650 CPU/GPU °C;
// tapping cycles performance modes via /usr/local/bin/nx769j-perf.
import QtQuick
import Quickshell
import qs.Commons
import qs.Commons as Commons

Item {
  id: widget

  property var host: null
  property var settings: ({})
  property bool editing: false
  property int cellsWide: 2
  property int cellsHigh: 1
  property bool shown: true

  readonly property int seconds: Math.max(3, Number(settings.interval) || 5)
  property var reading: host && host.cached ? host.cached : null
  property string failure: ""

  function parseReply(result) {
    if (result.code !== 0) {
      widget.failure = result.timedOut ? "perf timeout" : "perf unavailable"
      return
    }
    try {
      const obj = JSON.parse(result.stdout.trim())
      widget.failure = ""
      obj.at = Date.now()
      widget.reading = obj
      if (widget.host) widget.host.setCached(obj)
    } catch (e) {
      widget.failure = "perf parse error"
    }
  }

  function refresh(force) {
    if (!host || host.busy) return
    if (!force && reading && Date.now() - (reading.at || 0) < seconds * 1000) return
    host.run(["/usr/local/bin/nx769j-perf", "status", "--json"], parseReply, { timeout: 4 })
  }

  function cycleMode() {
    if (!host || host.busy) return
    host.run(["/usr/local/bin/nx769j-perf", "cycle", "--json"], parseReply, { timeout: 5 })
  }

  Component.onCompleted: refresh(false)
  Timer { interval: widget.seconds * 1000; repeat: true; running: widget.shown; onTriggered: widget.refresh(true) }
  onShownChanged: if (shown) refresh(false)

  readonly property color fg: host ? host.foreground : Commons.Color.menu.text
  readonly property color dim: host ? host.dimForeground : Commons.Color.menu.text
  readonly property string activeMode: reading ? String(reading.mode || "balanced") : "balanced"
  readonly property color modeColor: activeMode === "performance" ? Commons.Color.urgent : Commons.Color.accent
  readonly property string modeGlyph: activeMode === "powersave" ? "󰌪"
    : activeMode === "performance" ? "󰓅" : "󰾅"

  readonly property string headlineText: {
    if (!reading) return "性能模式 …"
    const zh = activeMode === "powersave" ? "省电"
      : activeMode === "performance" ? "狂暴" : "均衡"
    const rpm = Number(reading.fan_rpm) || 0
    return rpm > 0 ? ("性能: " + zh + " · " + rpm + " RPM") : ("性能: " + zh + " · 0 RPM")
  }

  readonly property string sublineText: {
    if (!reading) return "点按切换: 省电 / 均衡 / 狂暴"
    const cpu = Number(reading.hotspot_c || reading.peak_cpu_c)
    const gpu = Number(reading.peak_gpu_c)
    const parts = []
    if (isFinite(cpu) && cpu > 0) parts.push("CPU " + Math.round(cpu) + "°C")
    if (isFinite(gpu) && gpu > 0) parts.push("GPU " + Math.round(gpu) + "°C")
    parts.push("点按切换")
    return parts.join(" · ")
  }

  Column {
    anchors.fill: parent
    anchors.margins: Style.spacing.xl
    spacing: Style.spacing.sm

    Row {
      width: parent.width
      spacing: Style.spacing.md

      Text {
        textFormat: Text.PlainText
        text: widget.modeGlyph
        color: widget.modeColor
        font.family: Style.font.family
        font.pixelSize: Style.font.iconLarge
      }

      Text {
        width: parent.width - x
        textFormat: Text.PlainText
        text: widget.failure !== "" ? widget.failure : widget.headlineText
        color: widget.failure !== "" ? Commons.Color.urgent : widget.fg
        font.family: Style.font.family
        font.pixelSize: Style.font.subtitle
        font.bold: true
        elide: Text.ElideRight
      }
    }

    Text {
      width: parent.width
      textFormat: Text.PlainText
      text: widget.sublineText
      color: widget.dim
      font.family: Style.font.family
      font.pixelSize: Style.font.caption
      elide: Text.ElideRight
    }
  }

  MouseArea {
    anchors.fill: parent
    enabled: !widget.editing
    onClicked: widget.cycleMode()
  }
}
