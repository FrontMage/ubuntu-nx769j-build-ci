-- Keep only your personal input overrides here. Uncommented settings below
-- replace Omarchy's defaults.

-- Keyboard layout and options.
-- See https://wiki.hypr.land/Configuring/Basics/Variables/#input
-- hl.config({
--   input = {
--     kb_layout = "us,dk,eu",
--     kb_options = "omarchy:shift_caps_compose,grp:alts_toggle",
--     repeat_rate = 40,
--     repeat_delay = 250,
--     numlock_by_default = true,
--     sensitivity = 0.35,
--     accel_profile = "flat",
--     touchpad = {
--       natural_scroll = true,
--       clickfinger_behavior = true,
--       scroll_factor = 0.4,
--       disable_while_typing = false,
--       drag_3fg = 1,
--     },
--   },
-- })

-- NX769J Omarchy Mobile: 10-slot Goodix GT9916R touchscreen bound to WAYLAND-1.
-- Single-finger edge swipes belong exclusively to omamobile.back & apps
-- (workspace_swipe_touch = false prevents Hyprland's 1-finger edge swipe from
-- hijacking edge gestures if gaps/borders are toggled), while 3-finger
-- horizontal swipe switches open app workspaces cleanly.
hl.config({
  input = {
    touchdevice = {
      enabled = true,
      output = "WAYLAND-1",
      transform = 0,
    },
  },
  gestures = {
    workspace_swipe_touch = false,
  },
})
hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })
