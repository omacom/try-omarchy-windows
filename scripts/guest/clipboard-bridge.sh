#!/bin/sh
# Clipboard bridge (guest side) - syncs the Wayland clipboard with the Windows host.
# Counterpart: scripts/clipboard-bridge.ps1, started by launch-omarchy.ps1 on the host.
# Requires: wl-clipboard, socat (both tiny). Run inside the Hyprland session
# (needs WAYLAND_DISPLAY), e.g. from an autostart/exec-once. Text only (v1).
# 10.0.2.2 is the host as seen from QEMU user-mode networking.
HOST=10.0.2.2
PUSH_PORT=4448   # guest -> host: one base64 line per change, one connection each
PULL_PORT=4449   # host -> guest: persistent, one base64 line per host change
STATE=/tmp/.clipbridge
mkdir -p "$STATE"

# host -> guest
(
  while :; do
    # NOTE: no tr/sed between socat and the loop - a pipe stage buffers ~4KB and
    # stalls small clipboard payloads. The host writes LF-only lines (see
    # clipboard-bridge.ps1 NewLine); read -r strips nothing else we care about.
    socat -u TCP:$HOST:$PULL_PORT,connect-timeout=3 - 2>/dev/null | while IFS= read -r line; do
      line=${line%"$(printf '\r')"}
      printf '%s' "$line" | base64 -d > "$STATE/incoming" 2>/dev/null || continue
      sha256sum < "$STATE/incoming" | cut -d' ' -f1 > "$STATE/last_set"
      wl-copy < "$STATE/incoming"
    done
    sleep 2
  done
) &

# guest -> host: wl-paste fires the handler on every new selection
exec wl-paste --no-newline --watch sh -c '
  STATE=/tmp/.clipbridge
  cur=$(wl-paste --no-newline 2>/dev/null) || exit 0
  [ -z "$cur" ] && exit 0
  sha=$(printf "%s" "$cur" | sha256sum | cut -d" " -f1)
  [ "$sha" = "$(cat "$STATE/last_set" 2>/dev/null)" ] && exit 0
  [ "$sha" = "$(cat "$STATE/last_sent" 2>/dev/null)" ] && exit 0
  echo "$sha" > "$STATE/last_sent"
  { printf "%s" "$cur" | base64 -w0; echo; } | socat -u - TCP:10.0.2.2:4448,connect-timeout=3
'
