# v0.3.0 signed candidate on the Windows laptop (September 24, 2026)

The Release prepare run [35958752529](https://github.com/omacom/try-omarchy-windows/actions/runs/35958752529) created draft `v0.3.0`. Its `SHA256SUMS` digest is `0f34b70c7551839e9395030d5e3861c85178c7b5f9a4b61ed76a6e177127d51d`. The supplied assets include runtime r20c.

Signing check [35959572969](https://github.com/omacom/try-omarchy-windows/actions/runs/35959572969) produced the launcher from commit `a32482f3e9061b1ed5535dc51e476a21d702cb96`. On the laptop, the transferred file had SHA-256 `0bba36e502ec72b0ff1370cdb61fb791a81f5bdcd20ba0a9d600ce265b90c03d`. Windows reported Authenticode `Valid`, signer subject `CN=Brandon South, O=Brandon South, L=Wilmore, S=ky, C=US`, FileVersion `v0.3.0`, and ProductVersion `v0.3.0`.

## Source installation and copy

The install inventory found no guest receipt matching the public `v0.2.0` manifest digest `597eeb40999779319ddb81ab55ef51f0715ba982865e6fe4697db25abbea2824`. I used the permitted clean `v0.1.0` source at `D:\TryOmarchy-v0.1.0-20260922\clean`. Its receipt identified manifest `3963f1d21fb280134201ebd65e5f339d3e88588e861f3e7681469a704ad7f6a9`.

The source was copied to `D:\TryOmarchy-v0.3.0-20260924\copy1-upgrade`. Robocopy reported success, and both trees contained 223 files with 34,185,909,824 logical bytes. The original installation was not booted or modified. The disposable copy was removed during cleanup. The work folder retains its evidence files.

## Guest SSH trust blocker

The local `guest_known_hosts` pin for `[127.0.0.1]:2244` has fingerprint `SHA256:gToq9ETDiK+3K2mUR4bEbvM7YMqABX1A2IjFmZgqGRc`. With `StrictHostKeyChecking=yes`, the copied guest presented `SHA256:tbu1q3e2hn1FNRpMUhjreYWsrK8BusyGBuEABuXwCpI`. SSH reported that the remote host identification had changed and refused the connection. No alternate pin was added, and no guest command was run over that connection. The inspected v0.1 test folder did not contain a source-specific host-key pin.

An old-launcher baseline attempt was made on the copy to obtain a pre-upgrade user-file hash. Its saved runtime manifest URL pointed to `127.0.0.1:18081`, where no server was running. The launcher logged a runtime setup authentication failure and started stock QEMU in CPU mode. Although its log later reported userspace readiness, the SSH trust check prevented verification of the guest system state or file hash. A QMP ACPI powerdown returned `POWERDOWN`, after which stock WHPX QEMU stopped answering and exited with status 1 without a guest shutdown event. This was not a v0.3.0 candidate boot. The original installation and Windows settings remained untouched.

## Physical upgrade and boot

Not run. The signed candidate was not launched because the source guest did not have a matching trusted host-key pin. The v0.3.0 update receipt, r20c QEMU executable hash, GPU boot, guest systemd state, and preserved user-file hash were not verified. No candidate desktop screenshot was captured.

## Live audio

Not run. The bridge service, PipeWire endpoint list, session-1 playback and capture probes, route-file changes, and restoration of audio choices were not tested.

## Guest reboot and second launch

Not run. Guest reboot persistence, clean poweroff, launcher exit, and a no-download second launch were not tested.

## Interrupted-update recovery

Not run. No rollback copy was created and no interrupted update was attempted.

## Public release verification

Pending publication.
