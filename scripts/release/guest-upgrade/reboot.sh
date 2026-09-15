#!/bin/bash
set -euxo pipefail
systemctl is-active try-omarchy-update-repository.service
systemctl is-active try-omarchy-system-ownership.service
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $CANDIDATE_RUNTIME" ]]
[[ $(cat /usr/share/omarchy/version) == "$CANDIDATE_VERSION" ]]
sha256sum -c "$HOME/upgrade-preserve.sha256"
for path in /etc /usr /usr/lib /usr/share; do
  [[ $(stat -c '%u:%g' "$path") == 0:0 ]]
done
sudo pacman -Dk
sha256sum -c "$HOME/upgrade-nvim.sha256"
[[ $(readlink /usr/bin/omarchy-nvim-refresh) == "$(cat "$HOME/upgrade-nvim-link")" ]]
for helper in /usr/bin/omarchy-nvim-refresh /usr/bin/omarchy-nvim-setup; do
  [[ $(pacman -Qqo "$helper") == omarchy-nvim ]]
done
sudo pacman -Qk try-omarchy-runtime
sudo pacman -Qk omarchy-nvim
[[ -L /etc/skel/.config/nvim/lua/plugins/theme.lua ]]
[[ -L "$HOME/.config/nvim/lua/plugins/theme.lua" ]]
sudo systemctl restart try-omarchy-update-repository.service
systemctl is-active try-omarchy-update-repository.service
[[ ! -e /var/lib/pacman/db.lck ]]
sync
