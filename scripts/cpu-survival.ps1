# For each CPU candidate: boot Alpine under WHPX, check the guest SURVIVES 75s of
# real boot load (Skylake-Client-v3 passes launch but dies mid-boot).
$ErrorActionPreference = 'Stop'
$wd = 'C:\tryomarchy'
$candidates = @(
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt',
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+xsave,+avx',
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+xsave,+avx,+f16c,+fma,+bmi1,+bmi2,+avx2,+movbe',
    'IvyBridge-v2',
    'Haswell-v4'
)
foreach ($c in $candidates) {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    $err = "$wd\surv-err.log"
    $p = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList @(
        '-accel','whpx','-machine','q35','-cpu',$c,'-smp','2','-m','1024',
        '-cdrom',(Join-Path $wd 'alpine-virt.iso'),
        '-vga','none','-device','virtio-gpu-pci,id=gpu0',
        '-display','vnc=127.0.0.1:7',
        '-qmp','tcp:127.0.0.1:4445,server=on,wait=off','-net','none'
    ) -RedirectStandardError $err -PassThru -WindowStyle Hidden
    Start-Sleep 75
    if ($p.HasExited) {
        $msg = (Get-Content $err -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch 'warning' } | Select-Object -Last 1)
        Write-Host "DIED     $c :: $msg"
    } else {
        Write-Host "SURVIVE  $c"
        Stop-Process -Id $p.Id -Force
    }
}
Write-Host 'SURVIVAL-DONE'
