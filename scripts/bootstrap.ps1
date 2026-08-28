# Try Omarchy for Windows — developer-preview bootstrap.
# Run as Administrator. Idempotent; rerun after the WHP reboot if prompted.
#   powershell -ExecutionPolicy Bypass -File bootstrap.ps1
param([string]$Dir = "$env:LOCALAPPDATA\TryOmarchy")
$ErrorActionPreference = 'Stop'
$release = 'https://github.com/tsouth89/try-omarchy-windows/releases/download/v0.0.1-preview'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}

# 1. Windows Hypervisor Platform (works on Home and Pro)
$whp = Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
if ($whp.State -ne 'Enabled') {
    Write-Host 'Enabling Windows Hypervisor Platform...'
    Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All -NoRestart | Out-Null
    Write-Host 'REBOOT REQUIRED. Reboot, then run this script again.' -ForegroundColor Yellow
    exit 0
}
Write-Host 'WHP: enabled'

# 2. QEMU (needs 11.x for the WHPX interrupt fixes)
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) {
    Write-Host 'Installing QEMU via winget...'
    winget install --id SoftwareFreedomConservancy.QEMU --accept-source-agreements --accept-package-agreements --disable-interactivity
}
Write-Host "QEMU: $((& $qemu --version | Select-Object -First 1))"

# 3. zstd for the image decompress
$zstd = (Get-Command zstd -ErrorAction SilentlyContinue).Source
if (-not $zstd) {
    $cand = "$env:LOCALAPPDATA\Microsoft\WinGet\Links\zstd.exe"
    if (-not (Test-Path $cand)) {
        Write-Host 'Installing zstd via winget...'
        winget install --id Facebook.Zstandard --accept-source-agreements --accept-package-agreements --disable-interactivity
    }
    if (Test-Path $cand) { $zstd = $cand } else { $zstd = (Get-Command zstd -ErrorAction Stop).Source }
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
Write-Host "  powershell -ExecutionPolicy Bypass -File launch-omarchy.ps1"
