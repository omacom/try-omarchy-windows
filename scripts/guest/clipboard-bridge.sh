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
        sha256sum < "$STATE/incoming" | cut -d' ' -f1 > "$STATE/last_set"
        wl-copy < "$STATE/incoming" || break
      done
      sleep 2
    done
  ) &
  PULL_PID=$!

  # guest -> host. wl-paste exits when its Wayland connection disappears, so
  # the outer loop can discover the replacement socket and restart both sides.
  wl-paste --no-newline --watch sh -c '
    cur=$(wl-paste --no-newline 2>/dev/null) || exit 0
    [ -z "$cur" ] && exit 0
    sha=$(printf "%s" "$cur" | sha256sum | cut -d" " -f1)
    [ "$sha" = "$(cat "$STATE/last_set" 2>/dev/null)" ] && exit 0
    [ "$sha" = "$(cat "$STATE/last_sent" 2>/dev/null)" ] && exit 0
    echo "$sha" > "$STATE/last_sent"
    { printf "%s" "$cur" | base64 -w0; echo; } | socat -u - TCP:10.0.2.2:4448,connect-timeout=3 2>/dev/null
  ' || true

	cleanup
	sleep 2
done
