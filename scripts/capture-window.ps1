# Screenshot the Try Omarchy window from the HOST side (works on the GL path,
# where QMP screendump returns "no surface"). Captures the window's client area
# via PrintWindow with PW_RENDERFULLCONTENT so GPU-composited content is included.
#   powershell -ExecutionPolicy Bypass -File capture-window.ps1 -Out shot.png
param(
    [Parameter(Mandatory)][string]$Out,
    [string]$ProcessName = 'qemu-system-x86_64w'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "no $ProcessName window found" }
$hwnd = $p.MainWindowHandle
$r = New-Object WinCap+RECT
[WinCap]::GetClientRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { throw 'window has no client area (minimized?)' }
$bmp = New-Object Drawing.Bitmap($w, $h)
$gfx = [Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
# PW_CLIENTONLY (1) | PW_RENDERFULLCONTENT (2): client area incl. GPU content
$ok = [WinCap]::PrintWindow($hwnd, $hdc, 3)
$gfx.ReleaseHdc($hdc)
if (-not $ok) { Write-Warning 'PrintWindow reported failure; saving whatever rendered' }
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()
Write-Host "saved $Out ($w x $h)"
