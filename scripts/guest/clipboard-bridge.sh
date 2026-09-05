#!/bin/sh
# Clipboard bridge (guest side) for two-way text sync with the Windows host.
# It waits for the active Wayland session and restarts both directions if the
# compositor is replaced. 10.0.2.2 is the host under QEMU user networking.
HOST=10.0.2.2
PUSH_PORT=4448
PULL_PORT=4449

XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
STATE=$XDG_RUNTIME_DIR/try-omarchy-clipboard
export XDG_RUNTIME_DIR STATE
umask 077
mkdir -p "$STATE"

# wl-paste supplies the selected text on stdin. Keeping it in a file preserves
# trailing newlines and avoids a second clipboard read after the selection moves.
if [ "${1:-}" = --push ]; then
  outgoing=$(mktemp "$STATE/outgoing.XXXXXX") || exit 1
  trap 'rm -f "$outgoing"' EXIT
  head -c 8388609 > "$outgoing" || exit 1
  size=$(wc -c < "$outgoing")
  [ "$size" -gt 0 ] && [ "$size" -le 8388608 ] || exit 0
  sha=$(sha256sum < "$outgoing" | cut -d' ' -f1)
  [ "$sha" = "$(cat "$STATE/last_content" 2>/dev/null)" ] && exit 0
  if { base64 -w0 < "$outgoing"; echo; } | socat -u - TCP:$HOST:$PUSH_PORT,connect-timeout=3 2>/dev/null; then
    printf '%s\n' "$sha" > "$STATE/last_content"
  else
    exit 1
  fi
  exit 0
fi

find_wayland() {
  if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    export WAYLAND_DISPLAY
    return 0
  fi
  for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
    [ -S "$socket" ] || continue
    WAYLAND_DISPLAY=${socket##*/}
    export WAYLAND_DISPLAY
    return 0
  done
  unset WAYLAND_DISPLAY
  return 1
}

PULL_PID=
cleanup() {
	pull_pid=$PULL_PID
	PULL_PID=
	if [ -n "$pull_pid" ]; then
		kill "$pull_pid" 2>/dev/null || true
		wait "$pull_pid" 2>/dev/null || true
	fi
}
stop() {
	cleanup
	exit 0
}
trap cleanup EXIT
trap stop INT TERM

while :; do
  if ! find_wayland; then
    sleep 2
    continue
  fi

  # host -> guest
  (
    while :; do
      # Keep socat directly connected to read. An extra pipe stage buffers
      # small clipboard payloads and makes ordinary text appear stuck.
      socat -u TCP:$HOST:$PULL_PORT,connect-timeout=3 - 2>/dev/null | while IFS= read -r line; do
        line=${line%"$(printf '\r')"}
        printf '%s' "$line" | base64 -d > "$STATE/incoming" 2>/dev/null || continue
        sha256sum < "$STATE/incoming" | cut -d' ' -f1 > "$STATE/last_content"
        if ! wl-copy < "$STATE/incoming"; then
          rm -f "$STATE/last_content"
          break
        fi
      done
      sleep 2
    done
  ) &
  PULL_PID=$!

  # guest -> host. wl-paste exits when its Wayland connection disappears, so
  # the outer loop can discover the replacement socket and restart both sides.
  wl-paste --type text --watch "$0" --push || true

	cleanup
  sleep 2
done
