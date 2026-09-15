# Missing Neovim theme link in the factory skeleton, September 14, 2026

[#119](https://github.com/omacom/try-omarchy-windows/issues/119) tracks a missing
file the released `omarchy-nvim` package reports on the witness image:

```text
warning: omarchy-nvim: /etc/skel/.config/nvim/lua/plugins/theme.lua (No such file or directory)
omarchy-nvim: 10014 total files, 1 missing file
```

The investigation below identifies the cause, shows the user-visible effect, and
records the fix. It is separate from the duplicate command ownership that
[#116](https://github.com/omacom/try-omarchy-windows/issues/116) / #118 repaired.

## Cause

`omarchy-nvim` owns two copies of its Neovim defaults:

- `/etc/skel/.config/nvim`, installed into every new home by `useradd`, and
- `/usr/share/omarchy-nvim/config`, a fallback for older layouts.

The package's mtree shows that `/etc/skel` copy contains
`lua/plugins/theme.lua` as a symlink:

```text
./etc/skel/.config/nvim/lua/plugins/theme.lua type=link link=../../../../.local/state/omarchy/current/theme/neovim.lua
```

The `/usr/share/omarchy-nvim/config` copy intentionally omits it. That file is
generated per theme: `omarchy-theme-set-templates` renders
`default/themed/neovim.lua.tpl` into
`~/.local/state/omarchy/current/theme/neovim.lua`, and `omarchy-nvim-setup`
recreates the link with the same relative target.

The factory builder replaced `/etc/skel/.config` with the Omarchy source tree and
then rebuilt the Neovim skeleton from `/usr/share/omarchy-nvim/config` alone
(patch 0024). Because that directory has no `theme.lua`, the link disappeared from
the skeleton. Every account created from it, including the instant trial account,
opened Neovim without the active Omarchy theme, and `pacman -Qk` reported the
package file missing.

Read-only inspection of the checksum-verified v0.0.18 artifacts and the
compatibility-21 candidate confirms the link is absent from
`/etc/skel/.config/nvim/lua/plugins/` while the package database records it.

## Fix

`guest-build/0069` changes three things:

- `materialize-omarchy.sh` stashes the package's `/etc/skel/.config/nvim` before
  replacing `.config` and restores it afterward, so the packaged skeleton,
  symlink included, survives. The package-directory copy remains as a fallback
  for layouts without a skeleton.
- `finalize-rootfs.sh` adds `etc/skel/.config/nvim/lua/plugins/theme.lua` to the
  compatibility overlay and bumps the revision to 22. Existing persistent disks
  regain the file when the launcher integration update runs.
- `catch-up` recreates the relative link for a user who has a packaged Neovim
  config but no `theme.lua`, using the same Omarchy-4 and Omarchy-3 theme
  locations as `omarchy-nvim-setup`. It never replaces a `theme.lua` that already
  exists, so a user's own file is left alone.

The contract tests cover the skeleton ordering, the overlay membership, the
revision, and the catch-up behavior, including leaving a user-written file in
place. `scripts/release/smoke-guest.py` gained `nvim-theme-skel`,
`nvim-theme-user` and `omarchy-nvim-files` facts, enabled from compatibility
revision 22, the first revision that carries the repair.

## Evidence

Local builder with the complete patch series applied: 117 guest tests pass (one
optional skip). While the facts were still enabled for revision 21, the pre-fix
compatibility-21 candidate was booted read-only with
`smoke-guest.py --compat-revision 21`; every previous fact passed and the new
facts failed exactly as expected:

```text
TRYOMARCHY_FACT:nvim-theme-skel:
TRYOMARCHY_FACT:nvim-theme-user:
TRYOMARCHY_FACT:omarchy-nvim-files:no
instant guest booted but the image facts are wrong: {'nvim-theme-skel': (None, 'yes'), 'nvim-theme-user': (None, 'yes'), 'omarchy-nvim-files': ('no', 'yes')}
```

The empty values are the earlier `test -L ... && case ...` command form, which
printed nothing when the link was absent; the shipped form prints `no`. The
shipped gate starts at revision 22, so a revision-22 image missing the link
still fails.

Log: `/home/bts/Projects/try-omarchy-evidence/issue119-prefix-smoke/prefix-candidate.log`
(SHA256 `068f2b2c6a5acfb16eaa629234f5e1096287f1e45dc0739f884d95b9f09e80c6`).
The booted disk was the pre-fix `issue116-candidate` image
(decompressed rootfs SHA256
`3537945357c088af347fc02c22c0febb8187c374f140ed4c2fbb988eaf8d50e6`); it was not
rebuilt, so this run demonstrates detection of the defect, not the repair.

## Factory build and boot

[CI run 34922457869](https://github.com/omacom/try-omarchy-windows/actions/runs/34922457869)
passed on branch commit `3005d89`, including the launcher and guest contract
jobs, the full locked factory build, and the headless instant-account boot. The
smoke reports the corrected facts:

```text
TRYOMARCHY_FACT:nvim-theme-skel:yes
TRYOMARCHY_FACT:nvim-theme-user:yes
TRYOMARCHY_FACT:omarchy-nvim-files:yes
```

All previous facts also pass, including `compat-version=yes` for revision 22,
`package-database=clean`, `runtime-package=4.0.3-4` and the Vulkan environment.

## Existing-guest delivery

`smoke-guest-upgrade.py` seeded a disposable copy of the checksum-verified
v0.0.18 image, then booted the rebuilt candidate and ran the normal updater. All
five phases passed: seed, upgrade, reboot, boot the old external image, and
return to the candidate. The strengthened fixtures assert the exact relative
target on the existing disk and in the instant account's home:

```text
:: Updating Try Omarchy launcher integration
try-omarchy-runtime: 2011 total files, 0 missing files
omarchy-nvim: 10014 total files, 0 missing files
+ expected_theme_link=../../../../.local/state/omarchy/current/theme/neovim.lua
++ readlink /etc/skel/.config/nvim/lua/plugins/theme.lua
++ readlink /home/omarchy/.config/nvim/lua/plugins/theme.lua
```

The revision-22 overlay put the link back on the existing disk and `catch-up`
restored the instant account's link. Database consistency, runtime `4.0.3-4`,
exclusive helper ownership, helper hashes, user files, installed packages and
the old-image rollback all still pass, so the delivery does not regress the
existing upgrade path. The published v18 assets are unchanged.

Local evidence: `/home/bts/Projects/try-omarchy-evidence/issue119-upgrade`.

| Log | SHA256 |
| --- | --- |
| 01-seed.log | `f8f321ff43bd5de1465cb4520d0715870c2dfe39f590f6b4a2416bcc2a6d1caa` |
| 02-upgrade.log | `f32d10697b23ed72a437f40b1e1bc28d83c30371a9db9fe637506f5bc79dd146` |
| 03-reboot.log | `b4ee062d93f06c31360e6fafdfac35cc00623aefeeb207163f5429d04c0bd791` |
| 04-old-image.log | `d221f77c31566721abcb740d59cf76b099b6ee4cdd69e007dfcefc3d80ff56e7` |
| 05-return-to-candidate.log | `0d0985b8c104cb96a6e04f794e3c7a685d4a19efd559d615818277b98f85dc77` |

Candidate artifacts came from CI run 34922457869 (guest-manifest data and
SHA256SUMS verified before the run; decompressed rootfs SHA256
`fbff55d881ddfeea2679aa80ba578ef17427dd41ecf3dd6d55f33123698a3c01`). This is
headless Linux/KVM evidence, not new physical Windows acceptance. The v1
hardware gates in [V1-READINESS.md](V1-READINESS.md) remain open.
