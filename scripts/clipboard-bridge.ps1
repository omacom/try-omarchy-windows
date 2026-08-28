# Clipboard bridge (host side) - two-way text clipboard sync with the Omarchy guest.
# Counterpart: scripts/guest/clipboard-bridge.sh (runs inside the guest session).
# Transport is plain TCP through QEMU's user-mode network - the guest reaches the
# host as 10.0.2.2, so this works identically on stock QEMU and WINQ-EMU, GL or not
# (no SPICE, no vdagent, compositor-native via wl-clipboard on the guest side).
#   port 4448: guest -> host. One connection per clipboard change; a single
#              base64(UTF-8) line, then close.
#   port 4449: host -> guest. One persistent connection; the host writes a
#              base64(UTF-8) line per Windows clipboard change.
# Text only (v1). Loop prevention: each side skips content it just received.
#   powershell -ExecutionPolicy Bypass -File clipboard-bridge.ps1
param([int]$PushPort = 4448, [int]$PullPort = 4449, [string]$LogFile = '')
$ErrorActionPreference = 'Continue'
function Log([string]$m) { if ($LogFile) { try { Add-Content -Path $LogFile -Value "$(Get-Date -Format HH:mm:ss) $m" } catch { } } }

$push = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $PushPort)
$pull = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $PullPort)
$push.Start(); $pull.Start()
Write-Host "clipboard-bridge: guest->host on $PushPort, host->guest on $PullPort"

$pullClient = $null; $pullWriter = $null
$lastSeen = $null         # last host clipboard content we processed
$lastFromGuest = $null    # last content received from the guest

while ($true) {
    Start-Sleep -Milliseconds 400

    # (Re)accept the persistent host->guest connection
    if ($pull.Pending()) {
        if ($pullClient) { try { $pullClient.Close() } catch { } }
        $pullClient = $pull.AcceptTcpClient()
        $pullWriter = New-Object IO.StreamWriter($pullClient.GetStream())
        $pullWriter.AutoFlush = $true
        $pullWriter.NewLine = "`n"   # CRLF would corrupt the guest's base64 -d
        Log 'pull: guest connected'
    }

    # Drain guest -> host events
    while ($push.Pending()) {
        try {
            $c = $push.AcceptTcpClient()
            $c.GetStream().ReadTimeout = 3000
            $r = New-Object IO.StreamReader($c.GetStream())
            $line = $r.ReadLine()
            $c.Close()
            if ($line) {
                $text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($line))
                if ($text -and $text -ne $lastSeen) {
                    $lastFromGuest = $text
                    Set-Clipboard -Value $text
                    $lastSeen = $text
                }
            }
        } catch { }
    }

    # Host clipboard -> guest
    try { $cur = Get-Clipboard -Raw } catch { Log "poll error: $_"; $cur = $null }
    if ($cur -and $cur -ne $lastSeen) {
        $lastSeen = $cur
        Log "poll: change ($($cur.Length) chars), writer=$([bool]$pullWriter)"
        if ($cur -ne $lastFromGuest -and $pullWriter) {
            try {
                $pullWriter.WriteLine([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($cur)))
                Log 'send: ok'
            } catch {
                Log "send failed: $_"
                try { $pullClient.Close() } catch { }
                $pullClient = $null; $pullWriter = $null
            }
        }
    }
}
