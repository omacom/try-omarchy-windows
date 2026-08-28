# Boot Alpine under WHPX with a given CPU model, log in via QMP send-key, print
# CPU flags to the console, screendump. Runs entirely in one session (Windows
# sshd kills the process tree when the session exits, so qemu must not outlive it).
param([string]$cpu = 'Haswell-v4')
$ErrorActionPreference = 'Stop'
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
$wd = 'C:\tryomarchy'
$qmp = '\\host.lan\Data\tryomarchy\qmp.ps1'
$p = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList @(
    '-accel','whpx','-machine','q35','-cpu',$cpu,'-smp','2','-m','1024',
    '-cdrom',(Join-Path $wd 'alpine-virt.iso'),
    '-device','VGA',
    '-display','vnc=127.0.0.1:7',
    '-qmp','tcp:127.0.0.1:4445,server=on,wait=off','-net','none'
) -RedirectStandardError "$wd\avx-err.log" -PassThru -WindowStyle Hidden
Start-Sleep 40
if ($p.HasExited) { Write-Host 'QEMU-EXITED-EARLY'; Get-Content "$wd\avx-err.log" | Select-Object -Last 3; exit 1 }

powershell -ExecutionPolicy Bypass -File $qmp type 'root'
powershell -ExecutionPolicy Bypass -File $qmp key ret
Start-Sleep 3
powershell -ExecutionPolicy Bypass -File $qmp type 'clear; echo FLAGCHECK; for f in avx2 fma bmi2 f16c aes; do grep -m1 -qw $f /proc/cpuinfo && echo "$f yes" || echo "$f NO"; done'
powershell -ExecutionPolicy Bypass -File $qmp key ret
Start-Sleep 2
powershell -ExecutionPolicy Bypass -File $qmp shot flagcheck
if ($p.HasExited) { Write-Host 'QEMU-DIED-DURING-CHECK'; exit 1 }
Stop-Process -Id $p.Id -Force
Write-Host "FLAGCHECK-DONE cpu=$cpu"
