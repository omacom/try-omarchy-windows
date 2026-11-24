#!/bin/bash
# deskload.sh [SECONDS]: a window that animates every frame (glmark2-wayland,
# a light scene, vsync) stays on screen through four phases marked in the
# trace (deskbench.sh records page flips): idle, all cores busy, the GPU busy
# (a heavy off-screen render), both. As the desktop user, in a test VM.
# Run in the desktop session (WAYLAND_DISPLAY, HYPRLAND_INSTANCE_SIGNATURE set).
set -euo pipefail
T=/sys/kernel/tracing/trace_marker
S=${1:-10}
[[ $S =~ ^[1-9][0-9]?$ ]] || { echo "Use 1–99 seconds per phase" >&2; exit 2; }
[[ -w $T ]] || { echo "Start deskbench tracing first" >&2; exit 1; }
workspace=$(hyprctl -j activeworkspace | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
[[ $workspace =~ ^[1-9][0-9]*$ ]] || { echo "Start on a normal workspace" >&2; exit 1; }

workers=()
scene=
stop_workers() {
    local pid
    for pid in "${workers[@]}"; do kill "$pid" 2>/dev/null || :; done
    for pid in "${workers[@]}"; do wait "$pid" 2>/dev/null || :; done
    workers=()
}
cleanup() {
    stop_workers
    if [[ -n $scene ]]; then kill "$scene" 2>/dev/null || :; wait "$scene" 2>/dev/null || :; fi
    hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = $workspace }))" >/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = 9 }))" >/dev/null
timeout -k 2 "$((4 * S + 30))" glmark2-wayland -s 800x600 -b build --run-forever >/dev/null &
scene=$!
sleep 3
for phase in idle cpu gpu cpu+gpu; do
    if [[ $phase == *cpu* ]]; then
        timeout -k 2 "$((S + 8))" stress-ng --cpu "$(nproc)" --cpu-method matrixprod >/dev/null &
        workers+=("$!")
    fi
    if [[ $phase == *gpu* ]]; then
        timeout -k 2 "$((S + 8))" glmark2 --off-screen -s 2560x1440 --run-forever -b terrain >/dev/null &
        workers+=("$!")
    fi
    sleep 1
    for pid in "$scene" "${workers[@]}"; do kill -0 "$pid"; done
    echo "$phase" > "$T"
    sleep "$S"
    for pid in "$scene" "${workers[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Workload exited early in $phase; discard this capture" >&2
            exit 1
        fi
    done
    echo end > "$T"
    stop_workers
done
