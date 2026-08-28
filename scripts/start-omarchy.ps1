# Start the Omarchy guest under WHPX and keep it running while this session lives.
# Disk persists at C:\tryomarchy\vm\disk.raw (prepared by boot-omarchy-test.ps1).
$ErrorActionPreference = 'Stop'
$g = '\\host.lan\Data\tryomarchy\guest'
$wd = 'C:\tryomarchy'
$disk = Join-Path $wd 'vm\disk.raw'
if (-not (Test-Path $disk)) { throw 'disk.raw missing; run boot-omarchy-test.ps1 first' }

$spec = Get-Content (Join-Path $g 'build-spec.json') | ConvertFrom-Json
$cmdline = ($spec.runtime.kernelCommandLine -replace 'console=hvc0', 'console=ttyS0 console=tty1') + ' systemd.debug-shell=1'

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
$qemuArgs = @(
    '-accel','whpx','-machine','q35','-cpu','qemu64','-smp','4','-m','4096',
    '-drive',"file=$disk,format=raw,if=virtio",
    '-kernel',(Join-Path $g 'vmlinuz-linux'),
    '-initrd',(Join-Path $g 'initramfs-linux.img'),
    '-append',"`"$cmdline`"",
    '-vga','none',
    '-device','virtio-gpu-pci,id=gpu0',
    '-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
    '-device','virtio-net-pci,netdev=n0','-netdev','user,id=n0',
    '-device','virtio-rng-pci',
    '-display','vnc=127.0.0.1:7',
    '-qmp','tcp:127.0.0.1:4445,server=on,wait=off',
    '-serial',"file:$wd\omarchy-serial.log"
)
$p = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' `
    -ArgumentList $qemuArgs -RedirectStandardError "$wd\omarchy-qemu-err.log" `
    -RedirectStandardOutput "$wd\omarchy-qemu-out.log" -PassThru -WindowStyle Hidden
Write-Host "OMARCHY-RUNNING pid $($p.Id)"
Wait-Process -Id $p.Id
Write-Host "OMARCHY-EXITED code $($p.ExitCode)"
Get-Content "$wd\omarchy-qemu-err.log" -ErrorAction SilentlyContinue | Select-Object -Last 5
