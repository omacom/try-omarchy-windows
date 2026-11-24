#!/bin/bash
# Actual shell output in an OpenGL terminal, inside the disposable guest.
set -eu
trap 'exit 0' HUP INT TERM
printf '\033[2J\033[H\033[1;36mOmarchy\033[0m\n\n'
uname -sr
printf '\n'
ls --color=always /usr/share/omarchy
printf '\n'
printf '%s\n' "$$" > /tmp/omarchy-gpu-terminal-ready
if test -r /opt/wddm-root/native-input-delivery-probe; then
  printf '\nWaiting for the owned QEMU input diagnostic...\n'
  IFS= read -r -t 30 input_token
  test "$input_token" = omarchyinput93
  printf '\n\033[48;2;32;160;96m\033[38;2;0;0;0mInput received: %s\033[0m\n' "$input_token"
  printf '{"shellPid":%s,"terminalPid":%s,"uid":%s,"pty":"%s","token":"%s"}\n' \
    "$BASHPID" "$PPID" "$(id -u)" "$(tty)" "$input_token" > /tmp/omarchy-native-input-received.tmp
  mv -- /tmp/omarchy-native-input-received.tmp /tmp/omarchy-native-input-received
fi
while :; do
  printf '\r\033[K%s  |  %s' "$(date '+%A %H:%M:%S')" "$PWD"
  sleep 1
done
