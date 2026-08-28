# Compatibility shim - launch-omarchy.ps1 now auto-detects WINQ-EMU and uses the
# GPU path by itself. Kept so old docs/muscle memory keep working.
param(
    [string]$Dir = "$env:LOCALAPPDATA\TryOmarchy",
    [string]$WinqEmu = 'C:\WINQ-EMU',
    [switch]$Fullscreen,
    [switch]$Fresh
)
if (-not (Test-Path (Join-Path $WinqEmu 'bin\qemu-system-x86_64w.exe'))) {
    throw "WINQ-EMU not found at $WinqEmu - install it, or just run launch-omarchy.ps1 (CPU fallback)."
}
& (Join-Path $PSScriptRoot 'launch-omarchy.ps1') -Dir $Dir -WinqEmu $WinqEmu -Fullscreen:$Fullscreen -Fresh:$Fresh
