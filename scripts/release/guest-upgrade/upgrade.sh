#!/bin/bash
set -euxo pipefail
systemctl status try-omarchy-update-repository.service --no-pager
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $BASELINE_RUNTIME" ]]
sha256sum -c "$HOME/upgrade-preserve.sha256"
# Prove the compatibility payload supplied loadable modules before any package
# update can mask a missing-module regression on the persistent disk.
sudo modprobe tun
[[ -c /dev/net/tun ]]
sudo modprobe v4l2loopback
[[ -c /dev/video42 ]]
[[ $(cat "/usr/lib/modules/$(uname -r)/.tryomarchy-complete") == "$(cat /usr/share/try-omarchy/compat-version)" ]]
for metadata in modules.order modules.builtin modules.builtin.modinfo; do
  [[ -f /usr/lib/modules/$(uname -r)/$metadata ]]
done
# A lock deliberately owned by this test must block the transaction unchanged.
[[ ! -e /var/lib/pacman/db.lck ]]
printf 'upgrade-test-owned-lock\n' | sudo tee /var/lib/pacman/db.lck
if sudo pacman -Syu --noconfirm > /tmp/upgrade-lock-test.log 2>&1; then exit 1; fi
grep -q 'unable to lock database' /tmp/upgrade-lock-test.log
grep -qx 'upgrade-test-owned-lock' /var/lib/pacman/db.lck
sudo rm /var/lib/pacman/db.lck
# Let optional reboot/orphan prompts time out without accepting them.
GUM_CONFIRM_TIMEOUT=1s omarchy-update -y > /tmp/upgrade-packages.log 2>&1 || { cat /tmp/upgrade-packages.log; exit 1; }
if grep -q "command failed to execute correctly" /tmp/upgrade-packages.log; then cat /tmp/upgrade-packages.log; exit 1; fi
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $CANDIDATE_RUNTIME" ]]
[[ $(cat /usr/share/omarchy/version) == "$CANDIDATE_VERSION" ]]
sha256sum -c "$HOME/upgrade-preserve.sha256"
command -v pamixer
command -v playerctl
pacman -Qq | sort > /tmp/packages-after
comm -23 <(sort "$HOME/upgrade-packages-before.txt") /tmp/packages-after > /tmp/packages-missing
[[ ! -s /tmp/packages-missing ]]
sudo pacman -Dk
sha256sum -c "$HOME/upgrade-nvim.sha256"
[[ $(readlink /usr/bin/omarchy-nvim-refresh) == "$(cat "$HOME/upgrade-nvim-link")" ]]
for helper in /usr/bin/omarchy-nvim-refresh /usr/bin/omarchy-nvim-setup; do
  [[ $(pacman -Qqo "$helper") == omarchy-nvim ]]
done
sudo pacman -Qk try-omarchy-runtime
# Revision 22 puts the packaged Neovim theme link back on existing disks and
# catch-up restores it for the instant account created from the broken skeleton.
sudo pacman -Qk omarchy-nvim
expected_theme_link="../../../../.local/state/omarchy/current/theme/neovim.lua"
[[ $(readlink /etc/skel/.config/nvim/lua/plugins/theme.lua) == "$expected_theme_link" ]]
[[ $(readlink "$HOME/.config/nvim/lua/plugins/theme.lua") == "$expected_theme_link" ]]
omarchy-migrate
sha256sum -c "$HOME/upgrade-preserve.sha256"
# Revision 34 loads the pinch device rules once, keeps the personal override,
# and leaves the pre-upgrade file beside it unless the skeleton already had them.
input="$HOME/.config/hypr/input.lua"
[[ -f /usr/share/try-omarchy/pinch-input.lua ]]
[[ $(grep -cx -- '-- BEGIN TRY OMARCHY PINCH DEVICE' "$input") == 1 ]]
if grep -qx 'dofile("/usr/share/try-omarchy/pinch-input.lua")' "$input"; then exit 1; fi
grep -qx 'hl.config({ input = { repeat_rate = 40 } })' "$input"
if grep -qx -- '-- BEGIN TRY OMARCHY PINCH DEVICE' "$HOME/upgrade-input-before.lua"; then
  cmp "$input" "$HOME/upgrade-input-before.lua"
else
  cmp "$HOME/.config/hypr/input.lua.before-try-omarchy-pinch" "$HOME/upgrade-input-before.lua"
fi
[[ $(cat /run/try-omarchy/pinch-gestures) == ready ]]
sudo python3 /mnt/host/pinch-udev.py
sha256sum "$input" > "$HOME/upgrade-input-after.sha256"
sync
