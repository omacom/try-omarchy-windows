# Guest image build

The guest image is built from [jorge-huxley/try-omarchy-win](https://github.com/jorge-huxley/try-omarchy-win)'s
`win` branch guest builder, plus the patches in this directory. The release
helper checks out the exact commit in `source.lock.json`, applies every patch,
and runs the guest contract tests before building:

```bash
scripts/release/build-guest.sh --contract-only
scripts/release/build-guest.sh --output /path/to/artifacts
```

The second command needs Docker and currently takes about ten minutes. Release
CI also boots the resulting factory image with `scripts/release/smoke-guest.py`
before it uploads anything.

What the patches change (all proven live on hardware 2026-08-28):

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
- An explicit `tryomarchy.instant=1` kernel flag creates and finalizes a local
  trial account, while boots without the flag keep upstream's normal setup form

If Arch has moved since the lock was written, refresh it first and review the diff:

```bash
sudo bash guest/build-container.sh --refresh-package-lock guest/packages.lock.json
```
