# WHPX smoke test: boot Alpine Linux under -accel whpx (no TCG fallback, so if
# it boots at all, WHPX works), screendump via QMP, copy the shot to the share.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$wd = 'C:\tryomarchy'
New-Item -ItemType Directory -Path $wd -Force | Out-Null

$iso = Join-Path $wd 'alpine-virt.iso'
if (-not (Test-Path $iso)) {
    $base = 'https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64'
    $idx = (Invoke-WebRequest -Uri "$base/" -UseBasicParsing).Content
    if ($idx -notmatch '(alpine-virt-[\d.]+-x86_64\.iso)') { throw "no virt iso in index" }
    $name = $Matches[1]
    Write-Host "downloading $name"
    Invoke-WebRequest -Uri "$base/$name" -OutFile $iso -UseBasicParsing
}
Write-Host "iso: $((Get-Item $iso).Length / 1MB) MB"

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
$qemuArgs = @(
    '-machine','q35','-accel','whpx','-cpu','qemu64','-m','2048','-smp','2',
    '-cdrom',$iso,'-device','VGA','-display','none',
    '-qmp','tcp:127.0.0.1:4444,server=on,wait=off',
    '-serial',"file:$wd\serial.log"
)
$p = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' `
    -ArgumentList $qemuArgs -RedirectStandardError "$wd\qemu-err.log" `
    -RedirectStandardOutput "$wd\qemu-out.log" -PassThru -WindowStyle Hidden
Write-Host "qemu pid $($p.Id), booting 45s..."
Start-Sleep 45
if ($p.HasExited) {
    Write-Host "QEMU EXITED code $($p.ExitCode); stderr:"
    Get-Content "$wd\qemu-err.log"
    exit 1
}

$tcp = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $tcp.GetStream(); $s.ReadTimeout = 5000
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
$r = New-Object IO.StreamReader($s)
$r.ReadLine() | Out-Null                                # greeting
$w.WriteLine('{"execute":"qmp_capabilities"}'); Start-Sleep 1; $r.ReadLine() | Out-Null
$w.WriteLine('{"execute":"screendump","arguments":{"filename":"C:\\tryomarchy\\shot.ppm"}}')
Start-Sleep 2
$w.WriteLine('{"execute":"query-status"}'); Start-Sleep 1
1..3 | ForEach-Object { try { Write-Host $r.ReadLine() } catch {} }
$tcp.Close()

Stop-Process -Id $p.Id -Force
Copy-Item "$wd\shot.ppm" '\\host.lan\Data\tryomarchy\shot.ppm' -Force
Write-Host 'WHPX-BOOT-TEST-DONE'
