# Try Omarchy for Windows — GPU-accelerated launcher (WINQ-EMU / Venus Vulkan).
# Requires WINQ-EMU Alpha 10 installed at C:\WINQ-EMU (patched WHPX: -cpu host works,
# Venus Vulkan + virgl GL forwarding to the host GPU).
#   powershell -ExecutionPolicy Bypass -File launch-omarchy-gpu.ps1 [-Fullscreen] [-Fresh]
param(
    [string]$Dir = "$env:LOCALAPPDATA\TryOmarchy",
    [string]$WinqEmu = 'C:\WINQ-EMU',
    [switch]$Fullscreen,
    [switch]$Fresh   # discard the writable disk and start over
)
$ErrorActionPreference = 'Stop'
$g = Join-Path $Dir 'guest'
$vm = Join-Path $Dir 'vm'
$qemu = Join-Path $WinqEmu 'bin\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) { throw "WINQ-EMU not found at $qemu" }
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

# WINQ-EMU's patched WHPX survives -cpu host (XSAVE/AVX included) — see FINDINGS.md.
# virtio-vga-gl IS the VGA device (no -vga none needed, per WINQ-EMU's own launcher).
$qemuArgs = @(
    '-machine','q35,accel=whpx','-cpu','host','-smp','6','-m','6G',
    '-drive',"file=$disk,format=raw,if=virtio",
    '-kernel',(Join-Path $g 'vmlinuz-linux'),
    '-initrd',(Join-Path $g 'initramfs-linux.img'),
    '-append',$cmdline,
    '-device','virtio-vga-gl,blob=on,hostmem=4G,venus=on',
    '-display','sdl,gl=on',
    '-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
    '-device','virtio-net-pci,netdev=n0','-netdev','user,id=n0',
    '-device','virtio-rng-pci',
    '-device','virtio-sound-pci',
    '-serial',"file:$vm\serial-gpu.log",
    '-qmp','tcp:127.0.0.1:4445,server=on,wait=off',
    '-qmp','tcp:127.0.0.1:4446,server=on,wait=off',
    '-no-reboot',
    '-name','Try Omarchy (GPU)'
)
if ($Fullscreen) { $qemuArgs += '-full-screen' }

# SDL's keyboard grab installs a system-wide Win-key hook that leaks past window
# focus (see FINDINGS.md). Disable it; winkey-forwarder.ps1 does it right instead:
# Super reaches Omarchy only while the VM window is focused (over QMP port 4446).
$env:SDL_GRAB_KEYBOARD = '0'
$fwd = Join-Path $PSScriptRoot 'winkey-forwarder.ps1'
$fwdProc = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',$fwd -PassThru

Write-Host 'Booting Omarchy with Venus GPU acceleration (Ctrl+Alt+G grabs/releases the mouse)...'
try {
    & $qemu @qemuArgs
} finally {
    if ($fwdProc -and -not $fwdProc.HasExited) { Stop-Process -Id $fwdProc.Id -Force }
}
