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
# Matching headers hand the delivered camera and battery modules to DKMS
# without install errors.
if grep -q "Installation aborted" /tmp/upgrade-packages.log; then cat /tmp/upgrade-packages.log; exit 1; fi
# The launcher supplies the kernel, so the update never asks to reboot for the
# held linux package. A stand-in gum records what would have been asked.
restart_prompts=$(gum() { printf 'asked: %s\n' "$*"; return 1; }; export -f gum; omarchy-update-restart 2>&1 || true)
if grep -q "Linux kernel has been updated" <<<"$restart_prompts"; then exit 1; fi
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $CANDIDATE_RUNTIME" ]]
[[ $(cat /usr/share/omarchy/version) == "$CANDIDATE_VERSION" ]]
sha256sum -c "$HOME/upgrade-preserve.sha256"
command -v pamixer
command -v playerctl
# The runtime declares the integration packages, so the update installed the
# one the seed removed.
pacman -Q noto-fonts-cjk
# Capture first: a failing command inside for ... in $(...) does not trip set -e.
runtime_dependencies=$(pactree -u -d 1 try-omarchy-runtime | tail -n +2)
[[ -n $runtime_dependencies ]]
for package in $runtime_dependencies; do
  pacman -Q "$package" >/dev/null
done
pacman -Qq | sort > /tmp/packages-after
# Every package survives except the ones Omarchy's own migrations retire:
# Omarchy 4.5 replaces Tensaku with Omasnap and dua-cli with Disktree.
comm -23 <(sort "$HOME/upgrade-packages-before.txt") /tmp/packages-after |
  { grep -vxF -e tensaku -e dua-cli || true; } > /tmp/packages-missing
[[ ! -s /tmp/packages-missing ]] || { cat /tmp/packages-missing; exit 1; }
for package in ${CANDIDATE_ADDED_PACKAGES:-}; do
  pacman -Q "$package"
done
# The image leaves these out, so the update must not bring them either.
for package in ${CANDIDATE_OMITTED_PACKAGES:-}; do
  if pacman -Q "$package" 2>/dev/null; then exit 1; fi
done
[[ -z ${CANDIDATE_MISE:-} || $(pacman -Q try-omarchy-mise) == "try-omarchy-mise $CANDIDATE_MISE-1" ]]
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
# Every migration completed, including the ones the guest makes inert.
if omarchy-migrate --pending; then exit 1; fi
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
# SSH accepts only keys for the quick-start account the seed boot created.
# Catch-up adds the rule to disks from before revision 42 and records it there;
# later disks create the account with the rule and have no record.
cmp /etc/ssh/sshd_config.d/90-try-omarchy-quick-start.conf /usr/share/try-omarchy/quick-start-sshd.conf
sha256sum "$input" > "$HOME/upgrade-input-after.sha256"
systemctl --failed --no-pager
df -h /
sync
