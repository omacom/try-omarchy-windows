# Boot the built Omarchy factory image under QEMU/WHPX inside this VM.
# Adapted from jorge-huxley/try-omarchy-win windows/boot-omarchy.ps1 (their proven
# flags: whpx + q35 + qemu64, virtio disk/net/input, sparse NTFS disk), headless
# with QMP screendumps at intervals so the host can watch boot progress.
$ErrorActionPreference = 'Stop'
$g = '\\host.lan\Data\tryomarchy\guest'
$wd = 'C:\tryomarchy'
$vm = Join-Path $wd 'vm'
New-Item -ItemType Directory -Path $vm -Force | Out-Null

$spec = Get-Content (Join-Path $g 'build-spec.json') | ConvertFrom-Json
$cmdline = $spec.runtime.kernelCommandLine -replace 'console=hvc0', 'console=ttyS0 console=tty1'
$expandedMiB = $spec.runtime.storage.expandedSizeMiB

$disk = Join-Path $vm 'disk.raw'
if (-not (Test-Path $disk)) {
    Write-Host 'copying rootfs.ext4 -> disk.raw (6 GB over the share)...'
    Copy-Item (Join-Path $g 'rootfs.ext4') $disk
    fsutil sparse setflag $disk | Out-Null
    $fs = [IO.File]::Open($disk, 'Open', 'ReadWrite')
    try { $fs.SetLength([int64]$expandedMiB * 1MB) } finally { $fs.Dispose() }
    Write-Host "disk prepared, logical $expandedMiB MiB"
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
$qemuArgs = @(
    '-accel','whpx','-machine','q35','-cpu','qemu64','-smp','4','-m','4096',
    '-drive',"file=$disk,format=raw,if=virtio",
    '-kernel',(Join-Path $g 'vmlinuz-linux'),
    '-initrd',(Join-Path $g 'initramfs-linux.img'),
    '-append',"`"$cmdline`"",
    '-device','virtio-gpu-pci',
    '-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
    '-device','virtio-net-pci,netdev=n0','-netdev','user,id=n0',
    '-device','virtio-rng-pci',
    '-display','none',
    '-qmp','tcp:127.0.0.1:4445,server=on,wait=off',
    '-serial',"file:$wd\omarchy-serial.log"
)
$p = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' `
    -ArgumentList $qemuArgs -RedirectStandardError "$wd\omarchy-qemu-err.log" `
    -RedirectStandardOutput "$wd\omarchy-qemu-out.log" -PassThru -WindowStyle Hidden
Write-Host "qemu pid $($p.Id)"

function Invoke-Qmp([string[]]$cmds) {
    $tcp = New-Object Net.Sockets.TcpClient('127.0.0.1', 4445)
    $s = $tcp.GetStream(); $s.ReadTimeout = 5000
    $w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
    $r = New-Object IO.StreamReader($s)
    $r.ReadLine() | Out-Null
    $w.WriteLine('{"execute":"qmp_capabilities"}'); Start-Sleep -Milliseconds 500
    try { $r.ReadLine() | Out-Null } catch {}
    foreach ($c in $cmds) {
        $w.WriteLine($c); Start-Sleep -Milliseconds 800
        try { Write-Host $r.ReadLine() } catch {}
    }
    $tcp.Close()
}

foreach ($t in 60, 120, 180, 240) {
    Start-Sleep (60)
    if ($p.HasExited) {
        Write-Host "QEMU EXITED code $($p.ExitCode); stderr:"
        Get-Content "$wd\omarchy-qemu-err.log"
        break
    }
    $shot = "C:\\tryomarchy\\omarchy-$t.ppm"
    Invoke-Qmp @("{`"execute`":`"screendump`",`"arguments`":{`"filename`":`"$shot`"}}")
    Copy-Item "C:\tryomarchy\omarchy-$t.ppm" "\\host.lan\Data\tryomarchy\omarchy-$t.ppm" -Force -ErrorAction SilentlyContinue
    Write-Host "shot at ${t}s"
}

if (-not $p.HasExited) {
    Invoke-Qmp @('{"execute":"query-status"}')
    Stop-Process -Id $p.Id -Force
}
Copy-Item "$wd\omarchy-serial.log" '\\host.lan\Data\tryomarchy\omarchy-serial.log' -Force -ErrorAction SilentlyContinue
Write-Host 'OMARCHY-BOOT-TEST-DONE'
