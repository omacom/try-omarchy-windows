# Pinch guest readiness, September 24, 2026

This records the checks behind guest patch `0093`, which brings the pinch
touchpad's device rules to existing guests. The launcher is unchanged and pinch
still needs `-experimental-pinch`. No release was built or published.

## Candidate image

CI run [36089724762](https://github.com/omacom/try-omarchy-windows/actions/runs/36089724762)
built the complete guest image from test branch
`codex/pinch-guest-candidate-20260924` at `2b621fd505c5ba05d79b306cdb4770ad36295e03`.
That branch is the lock refresh from #186 (patch `0092`) plus this change. The
job's boot smoke test passed. The candidate `SHA256SUMS` digest is
`58944382453dd8163eba6ed0e4a177d5df5608b7bd1d18ac220956ba3bc1a081`, and every
downloaded artifact matched it.

## Headless upgrade from v0.3.0

`scripts/release/smoke-guest-upgrade.py` ran on a Linux KVM host with the public
`v0.3.0` guest as the baseline, verified against its published `SHA256SUMS`
digest `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. All
five phases passed: seed, upgrade, candidate reboot, a boot of the older v0.3.0
image, and the return to the candidate.

The v0.3.0 instant account starts with the unguarded loader from patch `0083`.
The seed phase appended a personal `repeat_rate` line after it. After the
upgrade, `input.lua` had exactly one guarded block, no unguarded loader, and the
personal line in its original position. `input.lua.before-try-omarchy-pinch`
matched the pre-upgrade file, catch-up recorded revision 34 and a per-user
record, and `/run/try-omarchy/pinch-gestures` read `ready`.

A uinput device named `QEMU Virtio Pinch Touchpad` had no
`LIBINPUT_IGNORE_DEVICE` property while the guest was ready. With
`/run/try-omarchy/pinch-ready` removed and a udev change event, the property was
`1`. After `try-omarchy-pinch-ready` ran again, it was gone. The `input.lua` hash
stayed the same through the candidate reboot, the older image and the return.

## Fresh factory user

The same harness then ran with the candidate as both baseline and target, and
all five phases passed. The new account's `input.lua` already had the guarded
block from the skeleton and stayed byte-identical, and no backup was made. The
first boot's journal shows catch-up finishing at 5.5 seconds, before the account
existed, then owner provisioning finishing at 13.44 seconds and
`try-omarchy-pinch-ready` reporting `pinch gestures are ready` at 13.46 seconds.
A new install therefore has pinch ready on its first boot. The uinput check
passed here too.

## Existing laptop guest

The AMD Windows 11 laptop's long-lived test guest at
`D:\TryOmarchy-V1-20260919\polish-storage\moved` was created from an August 25
factory image, before patch `0083`. On September 21 it got a hand-written
device rule for pinch testing. For this check its `input.lua` was set back to
that day's own backup, the untouched Omarchy default with SHA-256
`1c3904e31df667d5bee09ddad3744365f1341e8a6833855d92872789dbe667f7`. The
hand-written version is kept as `input.lua.manual-pinch-20260924`. A manifest of
the 11 files under `~/.config/hypr` and `~/Documents` was recorded before a
clean poweroff.

The guest had just been upgraded to the public v0.3.0 guest and r20c runtime.
The unsigned launcher built from #183 at `a9ec7de`, SHA-256
`df86b36d056464e0afd1c1705414558e0ee6b2c3a4347c3a02e130729f6024d7`, then
installed the candidate guest from a local copy using the digest above, kept the
r20c runtime, and started with `-experimental-pinch`. Userspace was ready at
`22:41:21` laptop time.

- The compat version was `34:7.2.6-arch2-1`. Catch-up logged that it loaded the
  pinch device rules and kept the original, and the gate logged `pinch gestures
  are ready`.
- `input.lua` equals the backup followed by the guarded fragment. The backup
  matches the default above. The other 10 recorded files were unchanged.
- The real `QEMU Virtio Pinch Touchpad` event node had `ID_INPUT_TOUCHPAD=1` and
  no `LIBINPUT_IGNORE_DEVICE`. Hyprland listed `qemu-virtio-pinch-touchpad` and
  `hyprctl configerrors` was empty.
- QEMU's command line included `virtio-pinch-pci`, and its log reported the
  Precision Touchpad bridge registered.

## Not yet shown

Hyprland does not report per-device tap settings, so no tap clicks from a
pinch still needs a physical check on this guest. Physical pinch and two-finger
scrolling on the migrated guest, a fresh Windows install of the candidate image,
Firefox, mixed DPI and fullscreen remain open. Automatic activation is separate
launcher work.
