# Probe which -cpu models/features QEMU's WHPX backend accepts on this machine.
# For each candidate: launch bare QEMU (SeaBIOS only), alive after 5s = accepted.
$ErrorActionPreference = 'Stop'
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$candidates = @(
    'qemu64',
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt',
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+xsave,+avx',
    'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+xsave,+avx,+f16c,+fma,+bmi1,+bmi2,+avx2,+movbe',
    'Nehalem-v2',
    'IvyBridge-v2',
    'Haswell-v4',
    'Skylake-Client-v3',
    'max'
)
foreach ($c in $candidates) {
    $err = "C:\tryomarchy\probe-err.log"
    $p = Start-Process -FilePath $qemu -ArgumentList @(
        '-accel','whpx','-machine','q35','-cpu',$c,'-smp','2','-m','512',
        '-display','none','-vga','none','-net','none'
    ) -RedirectStandardError $err -PassThru -WindowStyle Hidden
    Start-Sleep 5
    if ($p.HasExited) {
        $msg = (Get-Content $err -ErrorAction SilentlyContinue | Select-Object -Last 1)
        Write-Host "REJECT  $c :: $msg"
    } else {
        $warn = (Get-Content $err -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
        Write-Host "ACCEPT  $c (stderr lines: $warn)"
        Stop-Process -Id $p.Id -Force
    }
    Start-Sleep 1
}
Write-Host 'PROBE-DONE'
