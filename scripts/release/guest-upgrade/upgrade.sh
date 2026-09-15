#!/bin/bash
set -euxo pipefail
systemctl status try-omarchy-update-repository.service --no-pager
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $BASELINE_RUNTIME" ]]
sha256sum -c "$HOME/upgrade-preserve.sha256"
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
sync
