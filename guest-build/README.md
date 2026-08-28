# Guest image build (v0.0.2)

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s
`win` branch guest builder, plus the patches in this directory. Apply and build
on a Linux box with Docker:

```bash
git clone -b win https://github.com/jorge-huxley/try-omarchy-win
cd try-omarchy-win
git am ../try-omarchy-windows/guest-build/*.patch
sudo bash guest/build-container.sh    # ~10 min; artifacts land in dist/guest/
```

What the patches change (all proven live on hardware 2026-08-28, see NOTES.md):

- Omarchy pin bumped to the v4.0.1 release tag, staged runtime stamped 4.0.1
  (upstream's `version` file lags its tags)
- All 22 upstream themes included (was 6)
- Packages: ttfx + hypridle (screensavers), vulkan-virtio (Venus ICD)
- Guest cursor visible under SDL (the hidden-cursor fragment was a VNC-era assumption)
- Autologin stays permanent: a drop-in disarms upstream's one-boot autologin
  cleanup after provisioning (the VM window is the auth boundary here)
- Clipboard bridge baked in: /usr/local/bin/clipboard-bridge + a systemd user
  unit enabled for all users (host side lives in scripts/clipboard-bridge.ps1)
- /mnt/host automounts the launcher's `-Share` folder (virtio-9p, condition-guarded
  so boots without a share stay clean)

If Arch has moved since the lock was written, refresh it first and review the diff:

```bash
sudo bash guest/build-container.sh --refresh-package-lock guest/packages.lock.json
```
