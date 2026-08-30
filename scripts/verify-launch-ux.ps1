# Launch-UX regression checker. Run while the VM is up; exits nonzero on any FAIL.
# The contract (settled 2026-08-28 after hands-on feedback - do not regress):
#   - windowless QEMU binary (no console window), window titled "Try Omarchy"
#   - window opens MAXIMIZED (taskbar visible; never fullscreen by default)
#   - one guest-rendered mouse cursor, without SDL forcing a second host cursor
#   - guest console sized to the maximized client area (video=WxH on the cmdline)
#   - winkey-forwarder and clipboard bridge up (QMP 4446 + clip 4448/4449)
#   powershell -ExecutionPolicy Bypass -File verify-launch-ux.ps1
$ErrorActionPreference = 'Stop'
$fail = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name" -ForegroundColor Red; $script:fail++ }
}
Add-Type -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsZoomed(System.IntPtr h);' -Name Native -Namespace UxCheck

$p = Get-Process qemu-system-x86_64w -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
Check 'windowless QEMU binary running with a window' ($null -ne $p)
if ($null -eq $p) { exit 1 }

Check 'window titled "Try Omarchy"' ($p.MainWindowTitle -eq 'Try Omarchy')
Check 'window is maximized (not fullscreen, not floating)' ([UxCheck.Native]::IsZoomed($p.MainWindowHandle))

$cl = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)").CommandLine
Check 'single guest cursor selected (show-cursor=off)' ($cl -match 'show-cursor=off')
Check 'guest console sized to host (video=WxH)' ($cl -match 'video=\d+x\d+')
Check 'no legacy console binary in use' ($cl -notmatch 'qemu-system-x86_64\.exe')

$net = netstat -ano | Out-String
Check 'winkey-forwarder connected (4446)' ($net -match ':4446\s+\S+\s+ESTABLISHED')
Check 'supervisor connected (4447)' ($net -match ':4447\s+\S+\s+ESTABLISHED')
Check 'clipboard bridge listening (4448+4449)' (($net -match ':4448\s+\S+\s+LISTENING') -and ($net -match ':4449\s+\S+\s+LISTENING'))

if ($fail) { Write-Host "$fail check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'Launch-UX contract holds.' -ForegroundColor Green
