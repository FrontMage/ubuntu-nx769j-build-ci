// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Qt.labs.folderlistmodel
import Quickshell
import Quickshell.Bluetooth
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Networking
import Quickshell.Services.SystemTray
import Quickshell.Services.UPower
import Quickshell.Wayland
import qs.Commons
import qs.Commons as Commons
import qs.services as OmarchyServices
import "Readable.js" as Readable
import "StatusModel.js" as StatusModel
import "BarAppsModel.js" as BarAppsModel

// RedMagic 9 Pro (NX769J) Zero-Cutout Phone Status & Multitasking Bar.
//
// Unlike punch-hole phones, NX769J has an under-display selfie camera (0 px
// cutout), making the entire 432 px (portrait) / 960 px (landscape) x 48 px
// strip usable:
//   - Left:   Clock + active-call chip + 1-tap Multitasking / Recents button
//             (with open window count badge; toggles omamobile.switcher).
//   - Center: Scrollable row of Open App icons (ToplevelManager.toplevels
//             matched to DesktopEntries icons; highlights focused app; tap to
//             focus or minimize to Home; hold to open Switcher).
//   - Right:  SystemTray (StatusNotifierItem / Ayatana AppIndicator icons such
//             as FlClash; 1-tap brings window to focus or triggers DBusMenu
//             "Show", hold opens touch-sized DBusMenu sheet) + unread
//             notifications, DND, Bluetooth, cellular, Wi-Fi and battery.
//   - Background / downward pull anywhere on the bar opens Quick Settings.
Item {
  id: root

  // Injected by the host (see shell.qml configureBar).
  property string omarchyPath: Quickshell.env("OMARCHY_PATH")
  property var shell: null
  property var manifest: null
  property var barWidgetRegistry: null
  property var pluginRegistry: null
  property var barConfig: ({})

  // Scalar bar state the host mirrors to other plugins.
  readonly property bool barHidden: false
  readonly property string position: "top"
  readonly property bool vertical: false
  readonly property string fontFamily: Style.font.family
  readonly property int barSize: Math.max(44, Style.bar.sizeHorizontal)
  readonly property real pullThreshold: Style.space(14)

  readonly property color foreground: Commons.Color.bar.text
  readonly property color background: Commons.Color.bar.background
  readonly property color urgent: Commons.Color.bar.active

  function setting(name, fallback) {
    var value = barConfig ? barConfig[name] : undefined
    return value === undefined || value === null || value === "" ? fallback : value
  }

  readonly property string clockFormat: String(setting("clockFormat", "HH:mm"))
  readonly property bool showBatteryPercent: setting("showBatteryPercent", true) !== false
  readonly property string quickSettingsId: String(setting("quickSettings", "omamobile.quicksettings"))
  readonly property string switcherId: "omamobile.switcher"

  readonly property var notifications: shell && typeof shell.firstPartyServiceFor === "function"
    ? shell.firstPartyServiceFor("omarchy.notifications") : null
  readonly property bool doNotDisturb: notifications ? notifications.doNotDisturb === true : false

  // Shared app library for crisp .desktop + hicolor PNG/SVG icon lookup.
  Loader {
    id: localLibrary
    active: !!root.shell && !root.shell.appLibrary
    sourceComponent: Component { OmarchyServices.AppLibrary { } }
  }
  readonly property var appLibrary: shell && shell.appLibrary ? shell.appLibrary : localLibrary.item

  // ---------------------------------------------------------------- open apps & switcher state

  readonly property var desktopEntries: DesktopEntries.applications ? (DesktopEntries.applications.values || []) : []
  readonly property var openWindows: {
    var raw = ToplevelManager.toplevels ? ToplevelManager.toplevels.values : []
    return BarAppsModel.filterVisibleToplevels(raw)
  }
  readonly property bool appOnCurrentWorkspace: !!Hyprland.focusedWorkspace
    && !!Hyprland.focusedWorkspace.toplevels
    && Hyprland.focusedWorkspace.toplevels.values.length > 0
  readonly property var activeScreenToplevel: appOnCurrentWorkspace ? ToplevelManager.activeToplevel : null
  readonly property bool switcherOpen: shell && typeof shell.isPluginOpen === "function"
    ? shell.isPluginOpen(switcherId) === true : false

  property string pendingFocusKey: ""

  Connections {
    target: ToplevelManager.toplevels
    function onValuesChanged() {
      if (!root.pendingFocusKey) return
      var key = root.pendingFocusKey
      var wins = root.openWindows
      for (var i = 0; i < wins.length; i++) {
        var cls = String(wins[i].appId || "").toLowerCase()
        if (cls === key || (key === "flclash" && cls.indexOf("clash") >= 0)) {
          root.pendingFocusKey = ""
          wins[i].activate()
          return
        }
      }
    }
  }

  Timer {
    id: pendingFocusClear
    interval: 2500
    onTriggered: root.pendingFocusKey = ""
  }

  function isToplevelFocusedOnScreen(toplevel) {
    return !!toplevel && root.appOnCurrentWorkspace && ToplevelManager.activeToplevel === toplevel
  }

  function entryForToplevel(toplevel) {
    if (!toplevel) return null
    return BarAppsModel.entryFor(toplevel.appId, root.desktopEntries)
  }

  function nameForToplevel(toplevel) {
    if (!toplevel) return ""
    return BarAppsModel.appName(toplevel.appId, toplevel.title, root.desktopEntries)
  }

  function iconForToplevel(toplevel) {
    if (!toplevel) return ""
    var entry = entryForToplevel(toplevel)
    var iconName = entry && entry.icon ? String(entry.icon) : ""
    if (iconName && root.appLibrary && typeof root.appLibrary.iconSource === "function")
      return root.appLibrary.iconSource(iconName)
    if (iconName) {
      var direct = Quickshell.iconPath(iconName, true)
      if (direct) return direct
    }
    var appId = String(toplevel.appId || "")
    if (appId) {
      if (root.appLibrary && typeof root.appLibrary.iconSource === "function") {
        var byApp = root.appLibrary.iconSource(appId)
        if (byApp && byApp.indexOf("application-x-executable") < 0) return byApp
      }
      var byTheme = Quickshell.iconPath(appId, true)
      if (byTheme) return byTheme
    }
    return ""
  }

  function closeTransientSheets() {
    closeTrayMenu()
    if (shell && typeof shell.hide === "function") {
      if (typeof shell.isPluginOpen === "function" && shell.isPluginOpen(switcherId))
        shell.hide(switcherId)
      if (typeof shell.isPluginOpen === "function" && shell.isPluginOpen(quickSettingsId))
        shell.hide(quickSettingsId)
    }
  }

  function goHome() {
    closeTransientSheets()
    Quickshell.execDetached(["hyprctl", "dispatch", "hl.dsp.focus({ workspace = \"empty\" })"])
    Quickshell.execDetached(["qs", "ipc", "-p", Quickshell.shellDir, "call", "shell", "call", "omamobile.home", "goHome", ""])
  }

  function focusToplevel(toplevel) {
    if (!toplevel) return
    closeTransientSheets()
    toplevel.activate()
  }

  function activateOrMinimizeToplevel(toplevel) {
    if (!toplevel) return
    if (isToplevelFocusedOnScreen(toplevel)) goHome()
    else focusToplevel(toplevel)
  }

  function openSwitcher() {
    closeTrayMenu()
    if (shell && typeof shell.summon === "function" && shell.summon(switcherId, "{}"))
      return
    Quickshell.execDetached(["omarchy-shell", "shell", "summon", switcherId, "{}"])
  }

  function toggleSwitcher() {
    closeTrayMenu()
    if (shell && typeof shell.toggle === "function" && shell.toggle(switcherId, "{}"))
      return
    Quickshell.execDetached(["omarchy-shell", "shell", "toggle", switcherId, "{}"])
  }

  // ---------------------------------------------------------------- system tray & mobile menu sheet

  readonly property var trayItems: {
    var raw = SystemTray.items ? SystemTray.items.values : []
    return BarAppsModel.filterTrayItems(raw, Status.Passive)
  }

  property bool trayMenuOpen: false
  property var activeTrayItem: null
  property var submenuStack: []
  readonly property int submenuDepth: submenuStack.length
  readonly property string submenuTitle: submenuDepth > 0 ? submenuStack[submenuDepth - 1].title : ""
  readonly property var currentTrayMenuChildren: submenuDepth > 0
    ? submenuStack[submenuDepth - 1].opener.children
    : activeTrayMenuOpener.children

  Component {
    id: submenuOpenerComp
    QsMenuOpener {}
  }

  QsMenuOpener {
    id: activeTrayMenuOpener
    menu: root.activeTrayItem ? root.activeTrayItem.menu : null
  }

  Loader {
    source: Qt.resolvedUrl("../omamobile.back/BackLinkClient.qml")
    onLoaded: {
      item.name = "tray menu"
      item.handler = function() {
        if (root.submenuDepth > 0) {
          root.leaveTraySubmenu()
          return true
        }
        if (root.trayMenuOpen) {
          root.closeTrayMenu()
          return true
        }
        return false
      }
      item.active = Qt.binding(function() { return root.trayMenuOpen })
    }
  }

  function resetTraySubmenus() {
    var stack = submenuStack
    submenuStack = []
    for (var i = stack.length - 1; i >= 0; i--) {
      if (stack[i] && stack[i].opener) stack[i].opener.destroy()
    }
  }

  function enterTraySubmenu(entry, title) {
    var opener = submenuOpenerComp.createObject(root, { menu: entry })
    if (!opener) return
    var next = submenuStack.slice()
    next.push({ opener: opener, title: String(title || "") })
    submenuStack = next
  }

  function leaveTraySubmenu() {
    if (submenuStack.length === 0) return
    var next = submenuStack.slice()
    var top = next.pop()
    submenuStack = next
    if (top && top.opener) top.opener.destroy()
  }

  function openTrayMenu(item) {
    if (!item || !item.menu) return false
    resetTraySubmenus()
    activeTrayItem = item
    trayMenuOpen = true
    return true
  }

  function closeTrayMenu() {
    trayMenuOpen = false
    resetTraySubmenus()
  }

  property var delayedShowEntry: null
  property var delayedActivateItem: null
  property bool delayedFlClashActivate: false

  Timer {
    id: delayedTrayRestore
    interval: 65
    onTriggered: {
      var entry = root.delayedShowEntry
      var item = root.delayedActivateItem
      var isClash = root.delayedFlClashActivate
      root.delayedShowEntry = null
      root.delayedActivateItem = null
      root.delayedFlClashActivate = false
      if (entry && typeof entry.triggered === "function") {
        entry.triggered()
      } else if (isClash) {
        Quickshell.execDetached([
          "gdbus", "call", "--session",
          "--dest", "com.follow.clash",
          "--object-path", "/com/follow/clash",
          "--method", "org.gtk.Application.Activate", "{}"
        ])
      }
      if (item && !item.onlyMenu && typeof item.activate === "function") {
        item.activate()
      }
    }
  }

  function toplevelForTrayItem(item) {
    return BarAppsModel.matchToplevelForTrayItem(item, root.openWindows, root.desktopEntries)
  }

  function activateTrayItem(item, menuOpener) {
    if (!item) return "none"
    if (trayMenuOpen && activeTrayItem === item) {
      closeTrayMenu()
      return "closed-menu"
    }

    var matchedWin = toplevelForTrayItem(item)
    if (matchedWin) {
      if (isToplevelFocusedOnScreen(matchedWin)) {
        if (item.menu && openTrayMenu(item)) return "opened-menu"
        goHome()
        return "minimized-home"
      }
      focusToplevel(matchedWin)
      return "focused-window"
    }

    // Window is currently hidden to the tray (e.g. FlClash minimizeOnExit).
    var menuRows = menuOpener && menuOpener.children ? (menuOpener.children.values || []) : []
    var showEntry = BarAppsModel.findPrimaryShowMenuEntry(menuRows)
    var isClash = BarAppsModel.isFlClashTrayItem(item)

    if (showEntry || !item.onlyMenu || isClash) {
      closeTransientSheets()
      root.pendingFocusKey = isClash ? "com.follow.clash" : String(item.id || "").toLowerCase()
      pendingFocusClear.restart()

      if (root.appOnCurrentWorkspace) {
        // Switch to an empty phone workspace first so the restored window gets
        // its own screen instead of splitting the currently viewed app.
        Quickshell.execDetached(["hyprctl", "dispatch", "hl.dsp.focus({ workspace = \"empty\" })"])
        root.delayedShowEntry = showEntry
        root.delayedActivateItem = item
        root.delayedFlClashActivate = isClash && !showEntry
        delayedTrayRestore.restart()
      } else {
        if (showEntry && typeof showEntry.triggered === "function") {
          showEntry.triggered()
        } else if (isClash) {
          Quickshell.execDetached([
            "gdbus", "call", "--session",
            "--dest", "com.follow.clash",
            "--object-path", "/com/follow/clash",
            "--method", "org.gtk.Application.Activate", "{}"
          ])
        }
        if (!item.onlyMenu && typeof item.activate === "function") {
          item.activate()
        }
      }
      return "restored-window"
    }

    if (openTrayMenu(item)) return "opened-menu"
    return "noop"
  }

  // ---------------------------------------------------------------- connectivity & battery state

  SystemClock {
    id: clock
    precision: SystemClock.Minutes
  }

  readonly property var networkDevices: Networking.devices ? Networking.devices.values : []
  readonly property var wifiDevice: StatusModel.findDevice(networkDevices, DeviceType.Wifi)
  readonly property var wiredDevice: StatusModel.findDevice(networkDevices, DeviceType.Wired)
  readonly property var wifiNetwork: StatusModel.connectedNetwork(wifiDevice && wifiDevice.networks ? wifiDevice.networks.values : [])
  readonly property string networkKind: {
    if (wiredDevice && wiredDevice.connected) return "ethernet"
    if (wifiNetwork) return "wifi"
    return wifiDevice && !Networking.wifiEnabled ? "off" : "disconnected"
  }
  readonly property string networkIcon: StatusModel.networkIcon(networkKind,
    wifiNetwork ? Math.round((wifiNetwork.signalStrength || 0) * 100) : -1)

  property var cell: null
  readonly property string cellGlyph: StatusModel.cellGlyph(cell)
  readonly property string cellText: StatusModel.cellText(cell)

  Process {
    id: cellWatch
    command: ["omamobile-cell-watch"]
    running: true
    stdout: SplitParser {
      onRead: function(line) {
        var c = StatusModel.parseCell(line)
        if (c) root.cell = c
      }
    }
    onExited: { root.cell = null; cellRestart.restart() }
  }
  Timer { id: cellRestart; interval: 5000; onTriggered: cellWatch.running = true }

  property string callLine: ""
  property double nowMs: Date.now()
  readonly property string callChip: StatusModel.callChip(callLine, nowMs) || ""

  Process {
    id: callWatch
    command: ["omamobile-call-watch", "--observe"]
    running: true
    stdout: SplitParser { onRead: function(line) { root.callLine = String(line) } }
    onExited: { root.callLine = ""; callRestart.restart() }
  }
  Timer { id: callRestart; interval: 5000; onTriggered: callWatch.running = true }
  Timer {
    interval: 1000
    repeat: true
    running: root.callChip !== ""
    onTriggered: root.nowMs = Date.now()
  }

  readonly property string notificationsHistory: Quickshell.env("HOME") + "/.local/state/omarchy/notifications/history"
  readonly property string notificationsStore: Quickshell.env("HOME") + "/.local/state/omarchy/notifications/history.json"
  property bool storeLayout: false
  property double seenStamp: 0
  property int unread: 0

  FolderListModel {
    id: historyFiles
    folder: "file://" + root.notificationsHistory
    nameFilters: ["*.json"]
    showDirs: false
    showDotAndDotDot: false
    showHidden: false
    sortField: FolderListModel.Unsorted
    onCountChanged: Qt.callLater(root.countUnread)
    onStatusChanged: Qt.callLater(root.countUnread)
  }

  Connections {
    target: historyFiles
    function onRowsInserted() { Qt.callLater(root.countUnread) }
    function onRowsRemoved() { Qt.callLater(root.countUnread) }
    function onModelReset() { Qt.callLater(root.countUnread) }
    function onDataChanged() { Qt.callLater(root.countUnread) }
  }

  FileView {
    id: historyStore
    path: root.notificationsStore
    printErrors: false
    watchChanges: true
    onFileChanged: reload()
    onLoaded: { root.storeLayout = true; Qt.callLater(root.countUnread) }
    onLoadFailed: { root.storeLayout = false; Qt.callLater(root.countUnread) }
  }

  Timer { interval: 30000; repeat: true; running: root.storeLayout; onTriggered: historyStore.reload() }

  function countUnread() {
    if (storeLayout) {
      unread = StatusModel.unreadStored(historyStore.text(), seenStamp)
      return
    }
    var names = []
    for (var i = 0; i < historyFiles.count; i++) names.push(historyFiles.get(i, "fileName"))
    unread = StatusModel.unreadCount(names, seenStamp)
  }

  function rewatchHistory() {
    historyStore.reload()
    if (historyFiles.count > 0) return
    historyFiles.folder = ""
    historyFiles.folder = "file://" + root.notificationsHistory
  }

  Timer { interval: 15000; running: true; onTriggered: root.rewatchHistory() }

  function markSeen(value) {
    var stamp = StatusModel.parseStamp(value)
    if (stamp > seenStamp) seenStamp = stamp
    rewatchHistory()
    countUnread()
  }

  FileView {
    path: Quickshell.env("HOME") + "/.local/state/omamobile/notifications-seen"
    printErrors: false
    watchChanges: true
    onLoaded: root.markSeen(text())
    onFileChanged: reload()
  }

  readonly property var btAdapter: Bluetooth.defaultAdapter
  readonly property bool bluetoothConnected: {
    var devices = Bluetooth.devices ? Bluetooth.devices.values : []
    for (var i = 0; i < devices.length; i++)
      if (devices[i] && devices[i].connected) return true
    return false
  }

  readonly property var battery: UPower.displayDevice
  readonly property bool hasBattery: !!battery && battery.isPresent === true
  readonly property int batteryPercent: hasBattery ? Math.round(battery.percentage * 100) : -1
  readonly property bool charging: hasBattery && battery.state === UPowerDeviceState.Charging
  readonly property string batteryIcon: StatusModel.batteryIcon(batteryPercent, charging,
    hasBattery && battery.state === UPowerDeviceState.FullyCharged)

  // ---------------------------------------------------------------- host API & IPC

  function summonBarWidget(id) { return false }
  function hideBarWidget(id) { return false }
  function isBarWidgetOpen(id) { return false }
  function panelWidgetIdAt(section, index) { return "" }
  function debugBarGeometry() { return [{ position: position, size: barSize }] }

  function openQuickSettings() {
    closeTrayMenu()
    if (shell && typeof shell.summon === "function")
      shell.summon(quickSettingsId, JSON.stringify({ source: "bar" }))
  }

  function toggleQuickSettings() {
    closeTrayMenu()
    if (shell && typeof shell.toggle === "function")
      shell.toggle(quickSettingsId, JSON.stringify({ source: "bar" }))
  }

  IpcHandler {
    target: "omamobile.bar"

    function pullDown(): string {
      root.openQuickSettings()
      return "ok"
    }

    function toggleQuickSettings(): string {
      root.toggleQuickSettings()
      return "ok"
    }

    function toggleSwitcher(): string {
      root.toggleSwitcher()
      return "ok"
    }

    function activateTray(idOrIndex: string): string {
      var q = String(idOrIndex || "").trim().toLowerCase()
      var items = root.trayItems
      if (items.length === 0) return "empty"
      var target = null
      if (q === "" || q === "0") {
        target = items[0]
      } else {
        for (var i = 0; i < items.length; i++) {
          if (String(items[i].id || "").toLowerCase() === q || String(items[i].title || "").toLowerCase() === q) {
            target = items[i]
            break
          }
        }
      }
      if (!target) return "not-found"
      root.activeTrayItem = target
      return root.activateTrayItem(target, activeTrayMenuOpener)
    }

    function statusJson(): string {
      var ids = []
      for (var i = 0; i < root.trayItems.length; i++)
        ids.push(String(root.trayItems[i].id || root.trayItems[i].title || ""))
      var winClasses = []
      for (var j = 0; j < root.openWindows.length; j++)
        winClasses.push(String(root.openWindows[j].appId || ""))
      return JSON.stringify({
        openWindows: root.openWindows.length,
        windowClasses: winClasses,
        activeAppId: root.activeScreenToplevel ? String(root.activeScreenToplevel.appId || "") : "",
        trayCount: root.trayItems.length,
        trayIds: ids,
        switcherOpen: root.switcherOpen,
        trayMenuOpen: root.trayMenuOpen
      })
    }

    function notificationsSeen(stamp: string): string {
      root.markSeen(stamp)
      return String(root.unread)
    }

    function unread(): string {
      return String(root.unread)
    }
  }

  // ---------------------------------------------------------------- bar surface

  Variants {
    model: Quickshell.screens

    PanelWindow {
      id: barWindow

      required property var modelData
      screen: modelData

      anchors { top: true; left: true; right: true }
      implicitHeight: root.barSize
      exclusionMode: ExclusionMode.Auto
      color: root.background
      WlrLayershell.namespace: "omarchy-bar"
      WlrLayershell.layer: WlrLayer.Top

      Item {
        z: 1
        anchors.fill: parent
        anchors.leftMargin: Style.spacing.lg
        anchors.rightMargin: Style.spacing.lg

        // ---------------- LEFT: Clock + Call chip + 1-Tap Switcher Button
        Text {
          id: clockText
          anchors.left: parent.left
          anchors.verticalCenter: parent.verticalCenter
          textFormat: Text.PlainText
          text: Qt.formatDateTime(clock.date, root.clockFormat)
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.title
          font.bold: true
        }

        Rectangle {
          id: callChipPill
          objectName: "callChip"
          visible: root.callChip !== ""
          z: 2
          anchors.left: clockText.right
          anchors.leftMargin: Style.spacing.sm
          anchors.verticalCenter: parent.verticalCenter
          height: Math.round(root.barSize * 0.66)
          width: chipRow.implicitWidth + Style.spacing.md * 2
          radius: height / 2
          color: chipTap.pressed ? Qt.darker("#1e9e53", 1.2) : "#1e9e53"

          Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: Style.spacing.xs
            Text {
              anchors.verticalCenter: parent.verticalCenter
              textFormat: Text.PlainText
              text: String.fromCodePoint(0xF03F2)
              color: "white"
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
            }
            Text {
              anchors.verticalCenter: parent.verticalCenter
              textFormat: Text.PlainText
              text: root.callChip
              color: "white"
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
              font.bold: true
            }
          }

          MouseArea {
            id: chipTap
            anchors.centerIn: parent
            width: parent.width
            height: Math.max(parent.height, root.barSize)
            onClicked: Quickshell.execDetached(["omaphone", "--in-call"])
          }
        }

        // 1-Tap Multitasking / Recents button (toggles omamobile.switcher)
        Rectangle {
          id: switcherButton
          objectName: "switcherButton"
          z: 2
          anchors.left: callChipPill.visible ? callChipPill.right : clockText.right
          anchors.leftMargin: Style.spacing.sm
          anchors.verticalCenter: parent.verticalCenter
          height: Math.round(root.barSize * 0.66)
          width: Math.max(height + 4, switcherRow.implicitWidth + Style.spacing.md * 2)
          radius: height / 2
          color: root.switcherOpen ? Commons.Color.accent
            : switcherTap.pressed ? Style.pressedFillFor(root.foreground, Commons.Color.accent)
            : Util.alpha(root.foreground, root.openWindows.length > 0 ? 0.14 : 0.08)
          border.width: 1
          border.color: root.switcherOpen ? Commons.Color.accent : Util.alpha(root.foreground, 0.22)

          Row {
            id: switcherRow
            anchors.centerIn: parent
            spacing: 4

            Text {
              anchors.verticalCenter: parent.verticalCenter
              textFormat: Text.PlainText
              text: String.fromCodePoint(0xF0570)
              color: root.switcherOpen ? root.background : root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.iconLarge
            }

            Text {
              visible: root.openWindows.length > 0
              anchors.verticalCenter: parent.verticalCenter
              textFormat: Text.PlainText
              text: String(root.openWindows.length)
              color: root.switcherOpen ? root.background : root.foreground
              font.family: root.fontFamily
              font.pixelSize: Readable.px(Style.font.caption)
              font.bold: true
            }
          }

          MouseArea {
            id: switcherTap
            anchors.centerIn: parent
            width: Math.max(parent.width + 4, 38)
            height: root.barSize
            property real pressX: 0
            property real pressY: 0
            property bool pulled: false

            onPressed: function(mouse) {
              pressX = mouse.x
              pressY = mouse.y
              pulled = false
            }
            onPositionChanged: function(mouse) {
              var dy = mouse.y - pressY
              var dx = mouse.x - pressX
              if (!pulled && dy > root.pullThreshold && Math.abs(dy) > Math.abs(dx)) {
                pulled = true
                root.openQuickSettings()
              }
            }
            onClicked: if (!pulled) root.toggleSwitcher()
          }
        }

        // ---------------- RIGHT: SystemTray (e.g. FlClash) + Status Icons
        Row {
          id: rightRow
          z: 2
          anchors.right: parent.right
          anchors.verticalCenter: parent.verticalCenter
          spacing: Style.spacing.md

          // SystemTray items (StatusNotifierItem / Ayatana AppIndicator)
          Row {
            id: trayRow
            objectName: "systemTray"
            visible: root.trayItems.length > 0
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.spacing.xs

            Repeater {
              model: root.trayItems

              delegate: Item {
                id: traySlot
                required property var modelData
                required property int index

                width: 34
                height: root.barSize

                readonly property var matchedWindow: root.toplevelForTrayItem(modelData)
                readonly property bool windowFocused: !!matchedWindow && root.isToplevelFocusedOnScreen(matchedWindow)
                readonly property bool menuActive: root.trayMenuOpen && root.activeTrayItem === modelData

                QsMenuOpener {
                  id: pillMenuOpener
                  menu: traySlot.modelData ? traySlot.modelData.menu : null
                }

                Rectangle {
                  anchors.centerIn: parent
                  width: 30
                  height: 30
                  radius: 9
                  color: traySlot.menuActive ? Commons.Color.accent
                    : trayTap.pressed ? Style.pressedFillFor(root.foreground, Commons.Color.accent)
                    : traySlot.windowFocused ? Util.alpha(Commons.Color.accent, 0.22)
                    : Util.alpha(root.foreground, 0.10)
                  border.width: traySlot.windowFocused || traySlot.menuActive ? 1.5 : 1
                  border.color: traySlot.windowFocused || traySlot.menuActive
                    ? Commons.Color.accent
                    : Util.alpha(root.foreground, 0.18)

                  TrayIcon {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    icon: traySlot.modelData ? traySlot.modelData.icon : ""
                  }
                }

                MouseArea {
                  id: trayTap
                  anchors.fill: parent
                  pressAndHoldInterval: 420
                  property real pressX: 0
                  property real pressY: 0
                  property bool pulled: false
                  property bool held: false

                  onPressed: function(mouse) {
                    pressX = mouse.x
                    pressY = mouse.y
                    pulled = false
                    held = false
                  }
                  onPositionChanged: function(mouse) {
                    var dy = mouse.y - pressY
                    var dx = mouse.x - pressX
                    if (!pulled && dy > root.pullThreshold && Math.abs(dy) > Math.abs(dx)) {
                      pulled = true
                      root.openQuickSettings()
                    }
                  }
                  onPressAndHold: {
                    if (pulled) return
                    held = true
                    if (!root.openTrayMenu(traySlot.modelData))
                      root.activateTrayItem(traySlot.modelData, pillMenuOpener)
                  }
                  onClicked: {
                    if (pulled || held) return
                    root.activateTrayItem(traySlot.modelData, pillMenuOpener)
                  }
                }
              }
            }
          }

          Row {
            id: status
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.spacing.md

            Rectangle {
              objectName: "unreadNotifications"
              visible: root.unread > 0
              anchors.verticalCenter: parent.verticalCenter
              height: Math.max(18, Math.round(root.barSize * 0.45))
              width: Math.max(height, unreadText.implicitWidth + Style.spacing.md * 2)
              radius: height / 2
              color: Commons.Color.accent

              Text {
                id: unreadText
                anchors.centerIn: parent
                textFormat: Text.PlainText
                text: String(root.unread)
                color: root.background
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
                font.bold: true
              }
            }

            StatusGlyph {
              visible: root.doNotDisturb
              text: "󰂛"
            }

            StatusGlyph {
              visible: !!root.btAdapter && root.btAdapter.enabled
              text: root.bluetoothConnected ? "󰂱" : "󰂯"
            }

            Row {
              visible: root.cellGlyph !== ""
              spacing: Style.spacing.xs
              anchors.verticalCenter: parent.verticalCenter
              opacity: root.cell && root.cell.state === "searching" ? 0.55 : 1

              StatusGlyph { text: root.cellGlyph }
              Text {
                visible: text !== ""
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.PlainText
                text: root.cellText
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
                font.bold: true
              }
            }

            StatusGlyph {
              visible: root.networkIcon !== ""
              text: root.networkIcon
              opacity: root.networkKind === "off" || root.networkKind === "disconnected" ? 0.55 : 1
            }

            Row {
              visible: root.hasBattery
              spacing: 3
              anchors.verticalCenter: parent.verticalCenter

              StatusGlyph {
                text: root.batteryIcon
                color: root.batteryPercent >= 0 && root.batteryPercent <= 10 && !root.charging ? root.urgent : root.foreground
              }

              Text {
                visible: root.showBatteryPercent
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.PlainText
                text: root.batteryPercent + "%"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
              }
            }
          }
        }

        // ---------------- CENTER (Zero-Cutout Zone): Scrollable Open Apps Row
        Item {
          id: runningAppsStrip
          objectName: "runningAppsStrip"
          z: 2
          anchors.left: switcherButton.right
          anchors.leftMargin: Style.spacing.xs
          anchors.right: rightRow.left
          anchors.rightMargin: Style.spacing.xs
          anchors.top: parent.top
          anchors.bottom: parent.bottom
          clip: true

          ListView {
            id: appsListView
            visible: root.openWindows.length > 0 && runningAppsStrip.width >= 30
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.min(parent.width, Math.max(0, contentWidth))
            orientation: ListView.Horizontal
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width
            spacing: 4
            clip: true
            model: root.openWindows

            delegate: Item {
              id: appSlot
              required property var modelData
              required property int index

              width: 32
              height: root.barSize

              readonly property bool isFocused: root.isToplevelFocusedOnScreen(modelData)
              readonly property string iconUrl: root.iconForToplevel(modelData)
              readonly property string appTitle: root.nameForToplevel(modelData)

              Rectangle {
                anchors.centerIn: parent
                width: 30
                height: 30
                radius: 9
                color: appTap.pressed ? Style.pressedFillFor(root.foreground, Commons.Color.accent)
                  : appSlot.isFocused ? Util.alpha(Commons.Color.accent, 0.24)
                  : Util.alpha(root.foreground, 0.08)
                border.width: appSlot.isFocused ? 1.5 : 0
                border.color: Commons.Color.accent

                Image {
                  id: appIconImg
                  anchors.centerIn: parent
                  width: 20
                  height: 20
                  fillMode: Image.PreserveAspectFit
                  asynchronous: true
                  sourceSize.width: Math.max(20, Math.round(width * Screen.devicePixelRatio))
                  sourceSize.height: Math.max(20, Math.round(height * Screen.devicePixelRatio))
                  source: appSlot.iconUrl
                  visible: status === Image.Ready
                }

                Text {
                  visible: appIconImg.status !== Image.Ready
                  anchors.centerIn: parent
                  textFormat: Text.PlainText
                  text: BarAppsModel.monogram(appSlot.appTitle, appSlot.modelData ? appSlot.modelData.appId : "")
                  color: appSlot.isFocused ? Commons.Color.accent : root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Readable.px(Style.font.caption)
                  font.bold: true
                }
              }

              // Active-window underline indicator
              Rectangle {
                visible: appSlot.isFocused
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                width: 14
                height: 2.5
                radius: 1.25
                color: Commons.Color.accent
              }

              MouseArea {
                id: appTap
                anchors.fill: parent
                pressAndHoldInterval: 450
                property real pressX: 0
                property real pressY: 0
                property bool pulled: false
                property bool held: false

                onPressed: function(mouse) {
                  pressX = mouse.x
                  pressY = mouse.y
                  pulled = false
                  held = false
                }
                onPositionChanged: function(mouse) {
                  var dy = mouse.y - pressY
                  var dx = mouse.x - pressX
                  if (!pulled && dy > root.pullThreshold && Math.abs(dy) > Math.abs(dx) * 1.2) {
                    pulled = true
                    root.openQuickSettings()
                  }
                }
                onPressAndHold: {
                  if (pulled) return
                  held = true
                  root.openSwitcher()
                }
                onClicked: {
                  if (pulled || held) return
                  root.activateOrMinimizeToplevel(appSlot.modelData)
                }
              }
            }
          }
        }
      }

      // Background strip touch target: empty areas, clock, and right-side
      // status icons open Quick Settings on pull-down or toggle on tap.
      MouseArea {
        id: touch
        z: 0
        anchors.fill: parent
        property real pressY: 0
        property bool pulled: false

        onPressed: function(mouse) { pressY = mouse.y; pulled = false }
        onPositionChanged: function(mouse) {
          if (!pulled && mouse.y - pressY > root.pullThreshold) {
            pulled = true
            root.openQuickSettings()
          }
        }
        onClicked: if (!pulled) root.toggleQuickSettings()
      }
    }
  }

  // ---------------------------------------------------------------- touch tray menu overlay

  PanelWindow {
    id: trayMenuWindow
    visible: root.trayMenuOpen
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Normal
    color: "transparent"
    WlrLayershell.namespace: "omamobile-tray-menu"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: root.trayMenuOpen ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    Rectangle {
      anchors.fill: parent
      color: Commons.Color.menu.scrim
      opacity: 0.72
    }

    MouseArea {
      anchors.fill: parent
      onClicked: root.closeTrayMenu()
    }

    Item {
      anchors.fill: parent
      focus: root.trayMenuOpen
      Keys.onEscapePressed: {
        if (root.submenuDepth > 0) root.leaveTraySubmenu()
        else root.closeTrayMenu()
      }

      Rectangle {
        id: menuCard
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Style.space(8)
        anchors.rightMargin: Style.space(10)
        width: Math.min(parent.width - Style.space(20), Style.space(264))
        height: Math.min(parent.height - Style.space(24), menuHeaderCol.height + menuFlick.contentHeight + Style.space(20))
        radius: Math.max(12, Style.cornerRadius)
        color: Commons.Color.menu.background
        border.width: 1
        border.color: Util.alpha(Commons.Color.menu.text, 0.22)

        // Prevent taps inside the card from dismissing through the scrim.
        MouseArea { anchors.fill: parent }

        Column {
          id: menuHeaderCol
          anchors.top: parent.top
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.margins: Style.space(10)
          spacing: Style.space(6)

          Row {
            width: parent.width
            height: Style.space(36)
            spacing: Style.space(8)

            Rectangle {
              visible: root.submenuDepth > 0
              width: Style.space(34)
              height: Style.space(34)
              anchors.verticalCenter: parent.verticalCenter
              radius: width / 2
              color: backSubTap.pressed ? Style.pressedFillFor(Commons.Color.menu.text, Commons.Color.accent) : Util.alpha(Commons.Color.menu.text, 0.08)

              Text {
                anchors.centerIn: parent
                textFormat: Text.PlainText
                text: String.fromCodePoint(0xF0141)
                color: Commons.Color.menu.text
                font.family: root.fontFamily
                font.pixelSize: Style.font.iconLarge
              }

              MouseArea {
                id: backSubTap
                anchors.fill: parent
                onClicked: {
                  menuFlick.contentY = 0
                  root.leaveTraySubmenu()
                }
              }
            }

            TrayIcon {
              visible: root.submenuDepth === 0 && !!root.activeTrayItem
              width: 20
              height: 20
              anchors.verticalCenter: parent.verticalCenter
              icon: root.activeTrayItem ? root.activeTrayItem.icon : ""
            }

            Text {
              anchors.verticalCenter: parent.verticalCenter
              width: parent.width - (root.submenuDepth > 0 ? Style.space(44) : 30)
              textFormat: Text.PlainText
              text: root.submenuDepth > 0 ? root.submenuTitle
                : (root.activeTrayItem ? String(root.activeTrayItem.title || root.activeTrayItem.id || "Tray") : "Tray")
              color: Commons.Color.menu.text
              font.family: root.fontFamily
              font.pixelSize: Style.font.subtitle
              font.bold: true
              elide: Text.ElideRight
            }
          }

          Rectangle {
            width: parent.width
            height: 1
            color: Util.alpha(Commons.Color.menu.text, 0.16)
          }
        }

        Flickable {
          id: menuFlick
          anchors.top: menuHeaderCol.bottom
          anchors.topMargin: Style.space(4)
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.bottom: parent.bottom
          anchors.leftMargin: Style.space(8)
          anchors.rightMargin: Style.space(8)
          anchors.bottomMargin: Style.space(8)
          contentWidth: width
          contentHeight: menuRowsCol.implicitHeight
          clip: true
          boundsBehavior: Flickable.StopAtBounds
          interactive: contentHeight > height

          Column {
            id: menuRowsCol
            width: menuFlick.width
            spacing: 2

            Repeater {
              model: root.currentTrayMenuChildren

              delegate: Item {
                id: mRow
                required property var modelData
                required property int index

                readonly property bool isSep: !!modelData && modelData.isSeparator === true
                readonly property string labelTxt: modelData ? String(modelData.text || "").replace(/_/g, "") : ""

                width: menuRowsCol.width
                height: isSep ? Style.space(10) : Math.max(44, Style.space(44))
                opacity: !modelData || modelData.enabled !== false ? 1.0 : 0.45

                Rectangle {
                  visible: mRow.isSep
                  anchors.left: parent.left
                  anchors.right: parent.right
                  anchors.leftMargin: Style.space(8)
                  anchors.rightMargin: Style.space(8)
                  anchors.verticalCenter: parent.verticalCenter
                  height: 1
                  color: Util.alpha(Commons.Color.menu.text, 0.14)
                }

                Rectangle {
                  visible: !mRow.isSep
                  anchors.fill: parent
                  radius: Math.max(8, Style.cornerRadius)
                  color: rowTouch.pressed ? Style.pressedFillFor(Commons.Color.menu.text, Commons.Color.accent) : "transparent"
                }

                Text {
                  id: checkGlyph
                  visible: !mRow.isSep && !!mRow.modelData && mRow.modelData.buttonType !== QsMenuButtonType.None
                  anchors.left: parent.left
                  anchors.leftMargin: Style.space(10)
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(20)
                  textFormat: Text.PlainText
                  text: mRow.modelData && mRow.modelData.checkState === Qt.Checked ? "✓" : ""
                  color: Commons.Color.accent
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                  font.bold: true
                }

                Text {
                  visible: !mRow.isSep
                  anchors.left: parent.left
                  anchors.leftMargin: checkGlyph.visible ? Style.space(34) : Style.space(14)
                  anchors.right: subArrow.visible ? subArrow.left : parent.right
                  anchors.rightMargin: Style.space(10)
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: mRow.labelTxt
                  color: Commons.Color.menu.text
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                  elide: Text.ElideRight
                }

                Text {
                  id: subArrow
                  visible: !mRow.isSep && !!mRow.modelData && mRow.modelData.hasChildren === true
                  anchors.right: parent.right
                  anchors.rightMargin: Style.space(10)
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: "›"
                  color: Commons.Color.menu.text
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.title
                }

                MouseArea {
                  id: rowTouch
                  anchors.fill: parent
                  enabled: !mRow.isSep && !!mRow.modelData && mRow.modelData.enabled !== false
                  onClicked: {
                    if (mRow.modelData.hasChildren) {
                      menuFlick.contentY = 0
                      root.enterTraySubmenu(mRow.modelData, mRow.labelTxt)
                    } else {
                      mRow.modelData.triggered()
                      root.closeTrayMenu()
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

  // ---------------------------------------------------------------- reusable components

  component TrayIcon: Item {
    id: trayIconRoot
    required property var icon
    readonly property bool symbolic: BarAppsModel.iconIsSymbolic(icon)
    readonly property string resolvedSource: BarAppsModel.resolveTrayIconUrl(icon, Util.fileUrl)

    Image {
      id: trayIconImage
      anchors.fill: parent
      fillMode: Image.PreserveAspectFit
      sourceSize.width: Math.max(20, Math.round(Math.min(width, height) * Screen.devicePixelRatio))
      sourceSize.height: Math.max(20, Math.round(Math.min(width, height) * Screen.devicePixelRatio))
      source: trayIconRoot.resolvedSource
      visible: !trayIconRoot.symbolic && status === Image.Ready
      layer.enabled: trayIconRoot.symbolic
    }

    Rectangle {
      anchors.fill: trayIconImage
      color: root.foreground
      visible: trayIconRoot.symbolic && trayIconImage.status === Image.Ready
      layer.enabled: trayIconRoot.symbolic
      layer.effect: MultiEffect {
        maskEnabled: true
        maskSource: trayIconImage
        maskThresholdMin: 0.5
        maskSpreadAtMin: 1.0
      }
    }

    Text {
      visible: trayIconImage.status !== Image.Ready
      anchors.centerIn: parent
      textFormat: Text.PlainText
      text: "󰀻"
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: Style.font.iconLarge
    }
  }

  component StatusGlyph: Text {
    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    textFormat: Text.PlainText
    color: root.foreground
    font.family: root.fontFamily
    font.pixelSize: Style.font.iconLarge
  }
}
