#!/bin/bash
set -euxo pipefail
pacman -Q try-omarchy-runtime
[[ $(pacman -Q try-omarchy-runtime) == "try-omarchy-runtime $BASELINE_RUNTIME" ]]
mkdir -p "$HOME/Documents" "$HOME/.config/upgrade-test"
printf 'persistent user document\n' > "$HOME/Documents/upgrade-preserve.txt"
printf 'custom settings\n' > "$HOME/.config/upgrade-test/settings"
printf '\n# upgrade preservation fixture\n' | sudo tee -a /etc/sysctl.d/90-omarchy-file-watchers.conf
sha256sum "$HOME/Documents/upgrade-preserve.txt" "$HOME/.config/upgrade-test/settings" /etc/sysctl.d/90-omarchy-file-watchers.conf > "$HOME/upgrade-preserve.sha256"
sha256sum /usr/bin/omarchy-nvim-refresh /usr/bin/omarchy-nvim-setup > "$HOME/upgrade-nvim.sha256"
readlink /usr/bin/omarchy-nvim-refresh > "$HOME/upgrade-nvim-link"
# An existing disk can lack a package that newer images depend on. Remove one
# that nothing else needs; the upgrade must install it again.
sudo pacman -Rdd --noconfirm noto-fonts-cjk
if pacman -Q noto-fonts-cjk; then exit 1; fi
pacman -Qqe > "$HOME/upgrade-packages-before.txt"
# A personal input override that the pinch migration must keep.
printf '\n-- upgrade preservation fixture\nhl.config({ input = { repeat_rate = 40 } })\n' >> "$HOME/.config/hypr/input.lua"
cp -p "$HOME/.config/hypr/input.lua" "$HOME/upgrade-input-before.lua"
sync
