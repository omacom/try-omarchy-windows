#!/bin/bash
# Clipboard bridge (guest side) for two-way text and image sync with the
# Windows host. It waits for the active Wayland session and restarts both
# directions if the compositor is replaced. 10.0.2.2 is the host under QEMU
# user networking. One line per item: base64 text, or "png:" + base64 PNG.
HOST=10.0.2.2
PUSH_PORT=4448
PULL_PORT=4449
TEXT_LIMIT=8388608
IMAGE_LIMIT=16777216

XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
STATE=$XDG_RUNTIME_DIR/try-omarchy-clipboard
export XDG_RUNTIME_DIR STATE
umask 077
mkdir -p "$STATE"

# wl-paste supplies the selected text on stdin. Keeping it in a file preserves
# trailing newlines and avoids a second clipboard read after the selection moves.
case "${1:-}" in
--push|--receive|--push-image|--receive-image|--push-files|--receive-files|--receive-transfer|--receive-drop)
  # URI selections belong to the file watcher, never the text bridge.
  if [ "$1" = --push ] && wl-paste --list-types 2>/dev/null | grep -Eq '^(text/uri-list|x-special/gnome-copied-files)$'; then exit 0; fi
  kind=text
  limit=$TEXT_LIMIT
  case $1 in
  --push-image|--receive-image) kind=png; limit=$IMAGE_LIMIT ;;
  --push-files|--receive-files) kind=files; limit=$IMAGE_LIMIT ;;
  esac
  outgoing=$(mktemp "$STATE/outgoing.XXXXXX") || exit 1
  trap 'rm -f "$outgoing"' EXIT
  head -c $((limit + 1)) > "$outgoing" || exit 1
  size=$(wc -c < "$outgoing")
  [ "$size" -gt 0 ] && [ "$size" -le "$limit" ] || exit 0
  # Serialize both directions, including delivery. A completed push must not
  # overwrite the state of a newer host value received while it was sending.
  exec 9> "$STATE/lock"
  flock -x 9 || exit 1
  if [ "$1" = --receive-drop ]; then
    file-transfer drop-receive --state "$STATE" < "$outgoing" || exit 1
    exit 0
  fi
  if [ "$1" = --receive-transfer ]; then
    file-transfer clipboard-receive --state "$STATE" < "$outgoing" || exit 1
    exit 0
  fi
  if [ "$1" = --push-files ]; then
    file-transfer clipboard-send --state "$STATE" < "$outgoing"
    result=$?
    [ "$result" -eq 0 ] && exit 0
    [ "$result" -eq 3 ] || exit "$result"
    clipboard-files pack < "$outgoing" > "$outgoing.zip" || { rm -f "$outgoing.zip"; exit 1; }
    mv "$outgoing.zip" "$outgoing" || exit 1
  fi
  sha=$(sha256sum < "$outgoing" | cut -d' ' -f1)
  case $1 in
  --receive|--receive-image|--receive-files)
    printf '%s\n' "$sha" > "$STATE/last_content"
    # wl-copy forks a clipboard owner. It must not inherit the lock descriptor.
    if [ "$kind" = files ]; then
      clipboard-files unpack < "$outgoing" > "$outgoing.uris" || { rm -f "$outgoing.uris" "$STATE/last_content"; exit 1; }
      # Hash our local snapshot representation to suppress the file watcher echo.
      if ! clipboard-files pack < "$outgoing.uris" > "$outgoing.zip"; then
        clipboard-files discard < "$outgoing.uris" || true
        rm -f "$outgoing.uris" "$outgoing.zip" "$STATE/last_content"; exit 1
      fi
      sha256sum < "$outgoing.zip" | cut -d' ' -f1 > "$STATE/last_content"
      rm -f "$outgoing.zip"
      file-transfer clipboard-mark --state "$STATE" < "$outgoing.uris" || exit 1
      wl-copy --type text/uri-list < "$outgoing.uris" 9>&-
      result=$?
      if [ "$result" -ne 0 ]; then clipboard-files discard < "$outgoing.uris" || true; fi
      rm -f "$outgoing.uris"
      [ "$result" -eq 0 ]
    elif [ "$kind" = png ]; then
      wl-copy --type image/png < "$outgoing" 9>&-
    else
      # Windows text arrives as UTF-8. Left to guess the type, wl-copy can fall
      # back to libmagic, which reads a PowerShell prompt ("PS C:\...") as an
      # HP-GL plotter file and offers nothing an app can paste. A text type
      # keeps the usual text/plain, UTF8_STRING, TEXT and STRING offers too.
      wl-copy --type 'text/plain;charset=utf-8' < "$outgoing" 9>&-
    fi || {
      rm -f "$STATE/last_content"
      exit 1
    }
    exit 0
    ;;
  esac
  [ "$sha" = "$(cat "$STATE/last_content" 2>/dev/null)" ] && exit 0
  prefix=""
  [ "$kind" = png ] && prefix="png:"
  [ "$kind" = files ] && prefix="files:"
  if { printf '%s' "$prefix"; base64 -w0 < "$outgoing"; echo; } | timeout 20s socat -u - TCP:$HOST:$PUSH_PORT,connect-timeout=3 2>/dev/null 9>&-; then
    printf '%s\n' "$sha" > "$STATE/last_content"
  else
    exit 1
  fi
  exit 0
  ;;
esac

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
IMAGE_PID=
FILES_PID=
GNOME_FILES_PID=
cleanup() {
	pull_pid=$PULL_PID
	image_pid=$IMAGE_PID
	PULL_PID=
	IMAGE_PID=
	for pid in $pull_pid $image_pid $FILES_PID $GNOME_FILES_PID; do
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	done
  FILES_PID=
  GNOME_FILES_PID=
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
      file-transfer clipboard-pull 2>/dev/null | while IFS= read -r line; do
        # A suffix pattern scans huge LF-only frames quadratically in Bash.
        # Inspect fixed-length slices and keep decoding in the base64 process.
        if [[ ${line: -1} == $'\r' ]]; then line=${line:0:-1}; fi
        receive=--receive
        case ${line:0:9} in
        png:*) receive=--receive-image; line=${line:4} ;;
        files:*) receive=--receive-files; line=${line:6} ;;
        transfer:*) receive=--receive-transfer; line=${line:9} ;;
        drop:*) receive=--receive-drop; line=${line:5} ;;
        esac
        printf '%s' "$line" | base64 -d > "$STATE/incoming" 2>/dev/null || continue
        "$0" $receive < "$STATE/incoming" || break
      done
      sleep 2
    done
  ) &
  PULL_PID=$!

  # guest -> host, images. wl-paste only runs the handler when the selection
  # offers the requested type, so a text copy leaves this watcher idle.
  wl-paste --type image/png --watch "$0" --push-image 2>/dev/null &
  IMAGE_PID=$!
  wl-paste --type text/uri-list --watch "$0" --push-files 2>/dev/null &
  FILES_PID=$!
  wl-paste --type x-special/gnome-copied-files --watch "$0" --push-files 2>/dev/null &
  GNOME_FILES_PID=$!

  # guest -> host. wl-paste exits when its Wayland connection disappears, so
  # the outer loop can discover the replacement socket and restart both sides.
  wl-paste --type text --watch "$0" --push || true

	cleanup
  sleep 2
done
