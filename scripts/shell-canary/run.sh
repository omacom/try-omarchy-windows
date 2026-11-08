#!/usr/bin/env bash
set -euo pipefail
guest=/guest-source/guest
mkdir -p /results
cp "$guest/pacman-x86_64.conf" /etc/pacman.conf
pacman-key --init
pacman-key --populate archlinux
test "$(gpg --batch --with-colons --show-keys "$guest/keys/omarchy-pkgs.asc" | awk -F: '/^fpr:/ {print $10; exit}')" = 40DFB630FF42BCFFB047046CF0134EE680CAC571
pacman-key --add "$guest/keys/omarchy-pkgs.asc"
pacman-key --lsign-key 40DFB630FF42BCFFB047046CF0134EE680CAC571
# Match build-container.sh's OMARCHY_PACMAN_DISABLE_SANDBOX=1: the hosted
# Docker kernel cannot enforce pacman's Landlock downloader. Signatures and
# the container boundary stay enabled; this does not alter the guest config.
pacman --disable-sandbox -Syu --noconfirm
# The shell's own runtime from the guest package list: Qt image formats decode
# the .webp theme backgrounds and ImageMagick measures them for the picker.
pacman --disable-sandbox -S --needed --noconfirm quickshell qt6-base qt6-declarative qt6-wayland \
  qt6-svg qt6-multimedia qt6-imageformats imagemagick sway inotify-tools dbus mesa git python \
  jq fontconfig ttf-jetbrains-mono-nerd pipewire wireplumber
pacman -Q > /results/packages.txt
cp /etc/pacman.conf /results/pacman.conf
cp "$guest/spec.json" /results/build-spec.json
"$guest/scripts/fetch-source.sh" --destination /tmp/omarchy-source
# materialize requires the package-owned Neovim skeleton; it is unrelated to
# the shell. Seed its directory without installing the editor/toolchain.
mkdir -p /tmp/root/etc/skel/.config/nvim
"$guest/scripts/materialize-omarchy.sh" --root /tmp/root --source /tmp/omarchy-source
python3 "$guest/scripts/apply-omarchy-backports.py" --root /tmp/root --spec "$guest/spec.json"
cp -a /tmp/root/usr/bin/. /usr/bin/
find /tmp/root/usr/share/omarchy/shell -type f -print0 | sort -z | \
  xargs -0 sha256sum > /results/shell-sha256.txt
useradd --create-home canary
cp -a /tmp/root/etc/skel/. /home/canary/
mkdir -p /home/canary/.local/state/omarchy/current /tmp/canary-runtime
ln -s /tmp/root/usr/share/omarchy/themes/tokyo-night /home/canary/.local/state/omarchy/current/theme
background=$(find /tmp/root/usr/share/omarchy/themes/tokyo-night/backgrounds -type f -print -quit)
test -n "$background"
ln -s "$background" /home/canary/.local/state/omarchy/current/background
chown -R canary:canary /home/canary /tmp/canary-runtime
# The unprivileged probe and host runner both write diagnostic files here.
chmod 0777 /results
chmod 700 /tmp/canary-runtime
mkdir -p /tmp/canary-bin
install -m 0755 /repo/scripts/shell-canary/hyprctl_fixture.py /tmp/canary-bin/hyprctl
runuser -u canary -- env HOME=/home/canary XDG_RUNTIME_DIR=/tmp/canary-runtime \
  PATH=/tmp/canary-bin:/usr/bin \
  OMARCHY_PATH=/tmp/root/usr/share/omarchy QT_QPA_PLATFORM=wayland \
  QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 \
  dbus-run-session -- python3 /repo/scripts/shell-canary/probe.py
