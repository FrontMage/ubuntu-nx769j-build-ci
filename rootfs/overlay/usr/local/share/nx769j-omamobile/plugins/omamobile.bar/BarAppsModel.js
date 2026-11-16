// SPDX-License-Identifier: MIT
// Pure helpers for NX769J's zero-cutout phone status bar:
//   1. Matching Wayland ToplevelManager windows to DesktopEntry icons & names
//      (mirroring SwitcherModel.js & omamobile-launch, including Chromium CRX,
//      Omarchy web apps, and --app-id TUI entries).
//   2. Filtering & matching StatusNotifierItem / libayatana-appindicator tray
//      items (e.g. FlClash -> com.follow.clash) to open windows or DBusMenu
//      "Show" actions for 1-tap activation on touch screens.

var CRX_ID = /^[a-z]+-([a-p]{32})-[^-]+$/
var URL_APP = /^[a-z]+-(.+)-[^-]+$/
var SKIP_CLASSES = ["org.omarchy.screensaver"]
var WRAPPERS = [
  "env", "sh", "bash", "setsid", "nohup", "uwsm-app", "uwsm", "systemd-run",
  "gtk-launch", "python", "python3", "flatpak", "xdg-terminal-exec",
  "omarchy-launch-tui", "omarchy-launch-or-focus",
  "omarchy-launch-or-focus-tui", "omarchy-launch-webapp"
]
var GENERIC = ["desktop", "app", "application", "client", "gui", "main", "launcher"]
var SHOW_LABELS = [
  "show", "open", "restore", "show window", "open window",
  "main window", "show main window", "flclash",
  "\u663e\u793a", "\u6253\u5f00", "\u4e3b\u7a97\u53e3"
]

function lower(v) {
  return String(v === undefined || v === null ? "" : v).toLowerCase()
}

function stripDesktop(id) {
  return lower(id).replace(/\.desktop$/, "")
}

function execOf(entry) {
  if (!entry) return ""
  if (entry.execString) return String(entry.execString)
  try {
    if (entry.command && typeof entry.command.join === "function") return entry.command.join(" ")
  } catch (e) {
  }
  return ""
}

function webAppKey(url) {
  var m = /^[a-z][a-z0-9+.-]*:\/\/([^\/?#]+)([^?#]*)/i.exec(String(url || ""))
  if (!m) return ""
  var host = m[1].replace(/^[^@]*@/, "").toLowerCase()
  var path = m[2] || "/"
  return host + "_" + path.replace(/\//g, "_")
}

function webAppUrl(entry) {
  var exec = execOf(entry)
  var m = /--app=("?)([^"\s]+)\1/.exec(exec) || /omarchy-launch-webapp\s+("?)([^"\s]+)\1/.exec(exec)
  return m ? m[2] : ""
}

function appIdFlagOf(entry) {
  var exec = execOf(entry)
  var m = /--app-id=("?)([^"\s]+)\1/.exec(exec)
  if (!m) return ""
  var v = lower(m[2])
  return v.indexOf("tui.") === 0 ? "" : v
}

function programOf(entry) {
  var words = execOf(entry).split(/\s+/).filter(function(w) { return w.length > 0 })
  while (words.length && (words[0] === "env" || /^[A-Za-z_][A-Za-z0-9_]*=/.test(words[0]))) words.shift()
  if (!words.length) return ""
  var program = lower(words[0].replace(/^"|"$/g, "").replace(/^.*\//, ""))
  return WRAPPERS.indexOf(program) >= 0 ? "" : program
}

function entryFor(appId, entries) {
  var cls = lower(appId)
  if (!cls || !entries) return null
  var list = []
  for (var i = 0; i < entries.length; i++) if (entries[i]) list.push(entries[i])

  for (var a = 0; a < list.length; a++)
    if (stripDesktop(list[a].id) === cls) return list[a]
  for (var b = 0; b < list.length; b++)
    if (list[b].startupClass && lower(list[b].startupClass) === cls) return list[b]
  for (var t = 0; t < list.length; t++)
    if (appIdFlagOf(list[t]) === cls) return list[t]

  var crx = CRX_ID.exec(cls)
  if (crx) {
    for (var c = 0; c < list.length; c++) {
      if (lower(list[c].startupClass) === "crx_" + crx[1]) return list[c]
      if (lower(execOf(list[c])).indexOf("--app-id=" + crx[1]) >= 0) return list[c]
    }
  }

  var web = URL_APP.exec(cls)
  if (web) {
    var want = web[1].replace(/_+$/, "")
    for (var d = 0; d < list.length; d++) {
      var key = webAppKey(webAppUrl(list[d]))
      if (key && key.replace(/_+$/, "") === want) return list[d]
    }
    for (var e = 0; e < list.length; e++) {
      var k = webAppKey(webAppUrl(list[e]))
      if (k && want.indexOf(k.split("_")[0] + "_") === 0) return list[e]
    }
  }

  for (var f = 0; f < list.length; f++) {
    var id = stripDesktop(list[f].id)
    var last = id.indexOf(".") >= 0 ? id.slice(id.lastIndexOf(".") + 1) : ""
    if (last.length > 2 && GENERIC.indexOf(last) < 0 && last === cls) return list[f]
  }

  // Also check if window's reverse-DNS tail matches entry id or StartupWMClass
  var clsTail = cls.indexOf(".") >= 0 ? cls.slice(cls.lastIndexOf(".") + 1) : ""
  if (clsTail.length > 2 && GENERIC.indexOf(clsTail) < 0) {
    for (var h = 0; h < list.length; h++) {
      var eid = stripDesktop(list[h].id)
      var elast = eid.indexOf(".") >= 0 ? eid.slice(eid.lastIndexOf(".") + 1) : eid
      if (elast === clsTail || programOf(list[h]) === clsTail) return list[h]
    }
  }

  for (var g = 0; g < list.length; g++) {
    var program = programOf(list[g])
    if (program && program === cls) return list[g]
  }
  return null
}

function appName(appId, title, entries) {
  var entry = entryFor(appId, entries)
  var name = entry ? String(entry.name || "").trim() : ""
  if (name) return name
  var t = String(title || "").trim()
  if (t) return t
  var c = String(appId || "").trim()
  return c || "App"
}

function monogram(name, appId) {
  var src = String(name || "").trim()
  if (!src) {
    var c = String(appId || "").trim()
    src = c.indexOf(".") >= 0 ? c.slice(c.lastIndexOf(".") + 1) : c
  }
  if (!src) return "?"
  var ch = src.charAt(0).toUpperCase()
  return ch || "?"
}

function filterVisibleToplevels(toplevels) {
  var list = toplevels || []
  var out = []
  for (var i = 0; i < list.length; i++) {
    var tl = list[i]
    if (!tl) continue
    var cls = lower(tl.appId)
    if (SKIP_CLASSES.indexOf(cls) >= 0) continue
    out.push(tl)
  }
  return out
}

function isFlClashTrayItem(item) {
  if (!item) return false
  var id = lower(item.id)
  var title = lower(item.title)
  var tip = lower(item.tooltipTitle)
  var icon = lower(item.icon)
  return id.indexOf("flclash") >= 0
    || title.indexOf("flclash") >= 0
    || tip.indexOf("flclash") >= 0
    || icon.indexOf("flclash") >= 0
}

function filterTrayItems(items, passiveStatus) {
  var list = items || []
  var out = []
  for (var i = 0; i < list.length; i++) {
    var item = list[i]
    if (!item) continue
    if (passiveStatus !== undefined && item.status === passiveStatus) continue
    if (lower(item.id).indexOf("localsend") >= 0) continue
    out.push(item)
  }
  return out
}

function trayCandidateKeys(item) {
  if (!item) return []
  var raw = [item.id, item.title, item.tooltipTitle]
  var keys = []
  function add(k) {
    var v = stripDesktop(k).trim()
    if (!v) return
    var slash = v.lastIndexOf("/")
    if (slash >= 0) v = v.slice(slash + 1)
    if (v && keys.indexOf(v) < 0) keys.push(v)
  }
  for (var i = 0; i < raw.length; i++) add(raw[i])
  if (isFlClashTrayItem(item)) {
    add("com.follow.clash")
    add("flclash")
  }
  return keys
}

function matchToplevelForTrayItem(item, toplevels, entries) {
  var keys = trayCandidateKeys(item)
  if (!keys.length) return null
  var wins = filterVisibleToplevels(toplevels)
  for (var i = 0; i < wins.length; i++) {
    var tl = wins[i]
    var cls = lower(tl.appId)
    var tail = cls.indexOf(".") >= 0 ? cls.slice(cls.lastIndexOf(".") + 1) : cls
    var entry = entryFor(tl.appId, entries)
    var eid = entry ? stripDesktop(entry.id) : ""
    var ename = entry ? lower(entry.name) : ""
    for (var k = 0; k < keys.length; k++) {
      var key = keys[k]
      if (cls === key || tail === key || eid === key || ename === key) return tl
      if (key === "flclash" && cls.indexOf("clash") >= 0) return tl
    }
  }
  return null
}

function cleanMenuLabel(text) {
  return String(text === undefined || text === null ? "" : text)
    .replace(/_/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .toLowerCase()
}

function findPrimaryShowMenuEntry(children) {
  var list = children || []
  for (var i = 0; i < list.length; i++) {
    var row = list[i]
    if (!row || row.isSeparator || row.enabled === false || row.hasChildren) continue
    var lbl = cleanMenuLabel(row.text)
    if (SHOW_LABELS.indexOf(lbl) >= 0) return row
  }
  return null
}

function iconIsSymbolic(icon) {
  var name = String(icon || "").split("?")[0]
  return name.slice(-9) === "-symbolic"
}

function resolveTrayIconUrl(icon, fileUrlFn) {
  var s = String(icon || "").trim()
  if (!s) return ""
  if (s.indexOf("image://") === 0 || s.indexOf("file://") === 0) return s
  if (s.charAt(0) === "/") {
    return typeof fileUrlFn === "function" ? fileUrlFn(s) : ("file://" + s)
  }
  return s
}

if (typeof module !== "undefined") {
  module.exports = {
    entryFor: entryFor,
    appName: appName,
    monogram: monogram,
    filterVisibleToplevels: filterVisibleToplevels,
    isFlClashTrayItem: isFlClashTrayItem,
    filterTrayItems: filterTrayItems,
    trayCandidateKeys: trayCandidateKeys,
    matchToplevelForTrayItem: matchToplevelForTrayItem,
    cleanMenuLabel: cleanMenuLabel,
    findPrimaryShowMenuEntry: findPrimaryShowMenuEntry,
    iconIsSymbolic: iconIsSymbolic,
    resolveTrayIconUrl: resolveTrayIconUrl
  }
}
