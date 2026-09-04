# Try Omarchy for Windows - developer-preview bootstrap.
# Idempotent; rerun after the WHP reboot if prompted.
# Works without Administrator when the machine-wide pieces (WHP feature, QEMU) are
# already in place - only those two need elevation; the guest image and zstd are
# per-user. Admin is required the FIRST time on a machine.
#   powershell -ExecutionPolicy Bypass -File bootstrap.ps1
param([string]$Dir = "$env:LOCALAPPDATA\TryOmarchy")
$ErrorActionPreference = 'Stop'
$release = 'https://github.com/omacom/try-omarchy-windows/releases/download/v0.0.3-preview'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# 1. Windows Hypervisor Platform (works on Home and Pro)
if ($isAdmin) {
    $whp = Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
    if ($whp.State -ne 'Enabled') {
        Write-Host 'Enabling Windows Hypervisor Platform...'
        Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All -NoRestart | Out-Null
        Write-Host 'REBOOT REQUIRED. Reboot, then run this script again.' -ForegroundColor Yellow
        exit 0
    }
    Write-Host 'WHP: enabled'
} else {
    Write-Host 'WHP: cannot verify without Administrator - continuing. If launch fails with' -ForegroundColor Yellow
    Write-Host '"WHPX: No accelerator found", rerun this script from an elevated PowerShell.' -ForegroundColor Yellow
}

# 2. QEMU (needs 11.x for the WHPX interrupt fixes)
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) {
    if (-not $isAdmin) { throw 'QEMU is not installed and installing it needs Administrator. Rerun elevated.' }
    Write-Host 'Installing QEMU via winget...'
    winget install --id SoftwareFreedomConservancy.QEMU --accept-source-agreements --accept-package-agreements --disable-interactivity
}
Write-Host "QEMU: $((& $qemu --version | Select-Object -First 1))"

# 3. zstd for the image decompress (winget id was Facebook.Zstandard, renamed Meta.Zstandard)
function Find-Zstd {
    $c = (Get-Command zstd -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    $link = "$env:LOCALAPPDATA\Microsoft\WinGet\Links\zstd.exe"
    if (Test-Path $link) { return $link }
    # The Links shim often doesn't materialize in the installing session - search the
    # package store directly (seen on both machines bootstrapped so far).
    Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter zstd.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
$zstd = Find-Zstd
if (-not $zstd) {
    Write-Host 'Installing zstd via winget...'
    winget install --id Meta.Zstandard --accept-source-agreements --accept-package-agreements --disable-interactivity
    $zstd = Find-Zstd
    if (-not $zstd) { throw 'zstd not found after install; open a new shell or install zstd manually.' }
}

# 4. Guest image
$g = Join-Path $Dir 'guest'
New-Item -ItemType Directory -Path $g -Force | Out-Null
foreach ($f in 'vmlinuz-linux', 'initramfs-linux.img', 'build-spec.json', 'rootfs.ext4.zst') {
    $dest = Join-Path $g $f
    if (-not (Test-Path $dest)) {
        Write-Host "Downloading $f..."
        curl.exe -L --fail -o $dest "$release/$f"
    }
}
if (-not (Test-Path (Join-Path $g 'rootfs.ext4'))) {
    Write-Host 'Decompressing rootfs (6 GB)...'
    & $zstd -d (Join-Path $g 'rootfs.ext4.zst') -o (Join-Path $g 'rootfs.ext4')
}

Write-Host ''
Write-Host 'Done. Boot Omarchy with:' -ForegroundColor Green
Write-Host '  powershell -ExecutionPolicy Bypass -File launch-omarchy.ps1'
if (Test-Path 'C:\WINQ-EMU\bin\qemu-system-x86_64w.exe') {
    Write-Host 'WINQ-EMU detected: the launcher will use GPU acceleration (virgl + Venus).'
} else {
    Write-Host 'Optional: install WINQ-EMU Alpha 10 to C:\WINQ-EMU for GPU acceleration'
    Write-Host '(https://github.com/cmspam/winq-emu/releases) - without it, CPU rendering is used.'
}
