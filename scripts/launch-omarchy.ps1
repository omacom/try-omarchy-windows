# Try Omarchy for Windows — interactive launcher (developer preview).
# Boots the guest in an SDL window. First boot shows Omarchy's setup form.
#   powershell -ExecutionPolicy Bypass -File launch-omarchy.ps1 [-Fullscreen] [-Fresh]
param(
    [string]$Dir = "$env:LOCALAPPDATA\TryOmarchy",
    [switch]$Fullscreen,
    [switch]$Fresh   # discard the writable disk and start over
)
$ErrorActionPreference = 'Stop'
$g = Join-Path $Dir 'guest'
$vm = Join-Path $Dir 'vm'
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
New-Item -ItemType Directory -Path $vm -Force | Out-Null

$spec = Get-Content (Join-Path $g 'build-spec.json') | ConvertFrom-Json
$cmdline = $spec.runtime.kernelCommandLine -replace 'console=hvc0', 'console=ttyS0 console=tty1'
$expandedMiB = $spec.runtime.storage.expandedSizeMiB

$disk = Join-Path $vm 'disk.raw'
if ($Fresh -and (Test-Path $disk)) { Remove-Item $disk -Force }
if (-not (Test-Path $disk)) {
    Write-Host 'Preparing writable disk (sparse copy)...'
    Copy-Item (Join-Path $g 'rootfs.ext4') $disk
    fsutil sparse setflag $disk | Out-Null
    $fs = [IO.File]::Open($disk, 'Open', 'ReadWrite')
    try { $fs.SetLength([int64]$expandedMiB * 1MB) } finally { $fs.Dispose() }
}

# CPU: qemu64 + every feature upstream WHPX survives. Do NOT add AVX/XSAVE features
# with stock QEMU — the guest kernel panics in fpstate_reset (see docs/FINDINGS.md).
$cpu = 'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes'
$display = 'sdl,gl=off'

$qemuArgs = @(
    '-accel','whpx','-machine','q35','-cpu',$cpu,'-smp','4','-m','4096',
    '-drive',"file=$disk,format=raw,if=virtio",
    '-kernel',(Join-Path $g 'vmlinuz-linux'),
    '-initrd',(Join-Path $g 'initramfs-linux.img'),
    '-append',$cmdline,
    '-vga','none','-device','virtio-gpu-pci,id=gpu0',
    '-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
    '-device','virtio-net-pci,netdev=n0','-netdev','user,id=n0',
    '-device','virtio-rng-pci',
    '-device','intel-hda','-device','hda-duplex,audiodev=snd','-audiodev','dsound,id=snd',
    '-display',$display,
    '-serial',"file:$vm\serial.log",
    '-name','Try Omarchy'
)
if ($Fullscreen) { $qemuArgs += '-full-screen' }

Write-Host 'Booting Omarchy (Ctrl+Alt+G releases the mouse, Ctrl+Alt+F toggles fullscreen)...'
& $qemu @qemuArgs
