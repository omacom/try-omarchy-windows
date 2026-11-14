#!/bin/bash
# SPDX-License-Identifier: MIT
# Guest-only prerequisite. All state belongs to the disposable RAM overlay.
set -euo pipefail
export LC_ALL=C
stage=guards
failed() {
    local status=$? line=$1
    printf 'GUEST_USER_NETWORK_FAILED stage=%s exit=%s line=%s\n' "$stage" "$status" "$line"
    systemctl status systemd-resolved.service --no-pager || true
    journalctl -b -u systemd-resolved.service --no-pager -n 40 -o short-monotonic || true
    exit "$status"
}
trap 'failed "$LINENO"' ERR
test "$(id -u)" = 0
test "$(cat /sys/block/vda/ro)" = 1
test "$(findmnt -n -o FSTYPE /)" = overlay
printf 'GUEST_USER_NETWORK_BEGIN sourceBlockReadonly=true rootOverlay=true browserVerified=false\n'
stage=network-manager
modprobe virtio_net
timeout 15 systemctl start NetworkManager.service
timeout 10 systemctl start systemd-resolved.service
udevadm settle --timeout=10
interfaces=()
for path in /sys/class/net/*; do
    if [[ -L "$path/device/driver" && "$(basename "$(readlink -f "$path/device/driver")")" = virtio_net ]]; then
        interfaces+=("${path##*/}")
    fi
done
test "${#interfaces[@]}" = 1
interface=${interfaces[0]}
stage=dhcp
nmcli --wait 25 device connect "$interface"
ip -4 address show dev "$interface"
ip -4 route show dev "$interface"
stage=dns
resolvectl status "$interface" --no-pager
readlink -f /etc/resolv.conf
cat /etc/resolv.conf
resolved=false
for attempt in {1..10}; do
    if timeout 2 getent ahostsv4 example.com; then resolved=true; break; fi
    sleep 0.2
done
test "$resolved" = true
stage=https
body=/run/wddm-user-network-example.html
test ! -e "$body"
curl --disable --ipv4 --noproxy '*' --fail --show-error --silent \
    --proto '=https' --tlsv1.2 --connect-timeout 10 --max-time 20 \
    --max-filesize 1048576 --output "$body" \
    --write-out 'GUEST_USER_NETWORK_HTTPS httpCode=%{http_code} sslVerifyResult=%{ssl_verify_result} downloadedBytes=%{size_download}\n' \
    https://example.com/
test -s "$body"
sha256sum "$body"
printf 'GUEST_USER_NETWORK_READY dns=true https=true certificateVerification=true ipv4=true browserRenderingVerified=false\n'
