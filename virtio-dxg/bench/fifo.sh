#!/bin/bash
# fifo.sh [FRAMES]: vkcube with vsync (FIFO) on the desktop; prints its frame
# rate, which follows the compositor's.
while IFS= read -r l; do [ -n "$l" ] && export "$l"; done < /run/environment.d/90-virtio-dxg.conf
export XDG_RUNTIME_DIR=/run/user/$(id -u)
# Right after boot the session may not be up yet.
for i in $(seq 60); do ls $XDG_RUNTIME_DIR/wayland-? >/dev/null 2>&1 && break; sleep 1; done
export WAYLAND_DISPLAY=$(basename "$(ls $XDG_RUNTIME_DIR/wayland-? 2>/dev/null | head -1)")
n=${1:-600}
# Start-up first: vkcube with 1 frame.
t0=$(date +%s%N); timeout 30 vkcube --wsi wayland --c 1 >/dev/null 2>&1; t1=$(date +%s%N)
timeout 60 vkcube --wsi wayland --c $((n + 1)) >/dev/null 2>&1; rc=$?; t2=$(date +%s%N)
s=$(( (t1 - t0) / 1000 )); a=$(( (t2 - t1) / 1000 - s ))
echo "vkcube FIFO: $n frames in $((a / 1000)) ms = $(( n * 1000000 / (a > 0 ? a : 1) )) fps (start-up $((s / 1000)) ms, rc=$rc)"
