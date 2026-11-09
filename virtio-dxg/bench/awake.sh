#!/bin/bash
# awake.sh: keeps the dev VM's display on for benchmarks (an idle session
# turns it off, and Hyprland then throttles every client to a few frames).
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export HYPRLAND_INSTANCE_SIGNATURE=$(ls $XDG_RUNTIME_DIR/hypr | head -1)
pkill -x hypridle
for i in $(seq 20); do
    hyprctl monitors | grep -q 'dpmsStatus: 1' && exit 0
    hyprctl eval 'hl.dispatch(hl.dsp.dpms("on"))' >/dev/null
    sleep 0.5
done
echo "awake.sh: display still off" >&2
