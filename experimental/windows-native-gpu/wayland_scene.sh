#!/bin/sh
set -eu
trap 'printf "closed\n" > /tmp/desktop-scene-exit; exit 0' HUP TERM
printf '\033[2J\033[H'
printf '\033[1;36mTry Omarchy - Wayland surface\033[0m\n\n'
printf 'Real Foot terminal / JetBrains Mono\n'
printf 'Composed by Hyprland on NVIDIA\n\n'
printf '\033[48;2;235;70;70m     \033[48;2;70;200;100m     \033[48;2;60;130;235m     \033[0m\n\n'
printf 'Desktop integration in progress\n'
/usr/bin/fc-match -f '%{file}\n' sans-serif > /tmp/scene-font-sans
/usr/bin/fc-match -f '%{file}\n' monospace > /tmp/scene-font-mono
printf 'ready\n' > /tmp/desktop-scene-ready
sequence=0
while :; do
    sequence=$((sequence+1))
    printf '\033[10;1H\033[2KPTY updates: %04d' "$sequence"
    printf '%s\n' "$sequence" > /tmp/scene-update
    /usr/bin/sleep 1
done
