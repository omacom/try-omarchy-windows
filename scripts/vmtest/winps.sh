#!/usr/bin/env bash
# Run a PowerShell script (file arg or stdin) inside the Win11 VM over SSH. Never echoes the password.
set -euo pipefail
export SSHPASS="$(docker inspect omarchy-windows --format '{{range .Config.Env}}{{println .}}{{end}}' | sed -n 's/^PASSWORD=//p')"
src="${1:-/dev/stdin}"
sshpass -e ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -p 2222 bts@127.0.0.1 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command -' < "$src" 2>&1 | grep -v "post-quantum\|openssh.com/pq\|^\*\* "
