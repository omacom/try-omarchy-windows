param([string]$out, [int]$x = 100, [int]$y = 80, [int]$w = 1000, [int]$h = 700, [switch]$read)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class W {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$q = Get-Process qemu-system-x86_64w -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $q) { "no qemu window" | Set-Content $out; exit }
$hwnd = $q.MainWindowHandle
if (-not $read) { [W]::ShowWindow($hwnd, 9) | Out-Null; Start-Sleep -Milliseconds 500; [W]::MoveWindow($hwnd, $x, $y, $w, $h, $true) | Out-Null; Start-Sleep -Milliseconds 500 }
$r = New-Object W+RECT; [W]::GetWindowRect($hwnd, [ref]$r) | Out-Null
"rect=$($r.L),$($r.T),$($r.R),$($r.B) zoomed=$([W]::IsZoomed($hwnd)) title=$($q.MainWindowTitle)" | Set-Content $out
