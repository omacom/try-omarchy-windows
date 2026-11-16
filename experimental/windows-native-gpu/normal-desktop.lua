-- Private integration probe: load the real user's shipped Omarchy modules.
-- The source root remains read-only; this wrapper lives in the RAM overlay.
dofile(os.getenv("HOME") .. "/.config/hypr/hyprland.lua")
hl.monitor({ output = "WDDM-1", mode = "1280x720@60", position = "0x0", scale = 1 })
hl.config({ animations = { enabled = false } })
