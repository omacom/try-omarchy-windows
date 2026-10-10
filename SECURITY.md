# Security

## Reporting a vulnerability

Please report security issues privately, not as a public issue. Use GitHub's
[private vulnerability reporting](https://github.com/omacom/try-omarchy-windows/security/advisories/new)
(the repository's **Security** tab, then **Report a vulnerability**).

Include the Try Omarchy version, your Windows version and steps to reproduce.
Do not attach guest disks or backups. Reporters who want credit will be credited.

## Supported versions

Fixes ship in the latest release only. Use the newest version from the
[releases page](https://github.com/omacom/try-omarchy-windows/releases).

## Scope

In scope: the Windows launcher in `app/`, the importer in `migrate/`, the
release and signing scripts, and the guest and runtime build recipes in this
repository. Issues in upstream projects (Omarchy, QEMU, WINQ-EMU, Arch packages)
are welcome here when Try Omarchy ships or configures them in a way that makes
them exploitable; otherwise report them upstream.

## Release integrity

`TryOmarchy.exe` is Authenticode-signed with the publisher `CN=Brandon South`.
The launcher verifies SHA256 checksums for the graphics runtime and guest image
before using them. The [user guide](docs/USER-GUIDE.md) explains how to check
the signature, and [RELEASING.md](docs/RELEASING.md) describes the release
process.

## Maintainer

Brandon South ([@btsouth](https://github.com/btsouth)) maintains this
project and handles security reports, audits and hardening.
