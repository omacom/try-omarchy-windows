# QMP driver for the running Omarchy guest (port 4445).
# Usage: qmp.ps1 shot NAME | type STRING | key NAME[,NAME...] | status
param([Parameter(Mandatory)][string]$op, [string]$arg = '')
$ErrorActionPreference = 'Stop'

$tcp = New-Object Net.Sockets.TcpClient('127.0.0.1', 4445)
$s = $tcp.GetStream(); $s.ReadTimeout = 5000
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
$r = New-Object IO.StreamReader($s)
$r.ReadLine() | Out-Null
$w.WriteLine('{"execute":"qmp_capabilities"}'); Start-Sleep -Milliseconds 400
try { $r.ReadLine() | Out-Null } catch {}

function Send-Keys([string[]]$qcodes) {
    $keys = ($qcodes | ForEach-Object { "{`"type`":`"qcode`",`"data`":`"$_`"}" }) -join ','
    $w.WriteLine("{`"execute`":`"send-key`",`"arguments`":{`"keys`":[$keys]}}")
    Start-Sleep -Milliseconds 120
    try { $r.ReadLine() | Out-Null } catch {}
}

switch ($op) {
    'shot' {
        $w.WriteLine("{`"execute`":`"screendump`",`"arguments`":{`"filename`":`"$($arg -replace '\\','\\\\')`"}}")
        Start-Sleep -Milliseconds 1500
        try { Write-Host $r.ReadLine() } catch {}
        Write-Host "shot $arg"
    }
    'type' {
        foreach ($ch in $arg.ToCharArray()) {
            $c = [string]$ch
            if ($c -cmatch '^[a-z0-9]$') { Send-Keys @($c) }
            elseif ($c -cmatch '^[A-Z]$') { Send-Keys @('shift', $c.ToLower()) }
            else {
                switch ($c) {
                    '-' { Send-Keys @('minus') }
                    '.' { Send-Keys @('dot') }
                    ' ' { Send-Keys @('spc') }
                    '_' { Send-Keys @('shift','minus') }
                    '@' { Send-Keys @('shift','2') }
                    '/' { Send-Keys @('slash') }
                    '\' { Send-Keys @('backslash') }
                    '|' { Send-Keys @('shift','backslash') }
                    ':' { Send-Keys @('shift','semicolon') }
                    ';' { Send-Keys @('semicolon') }
                    ',' { Send-Keys @('comma') }
                    '=' { Send-Keys @('equal') }
                    '*' { Send-Keys @('shift','8') }
                    '$' { Send-Keys @('shift','4') }
                    '(' { Send-Keys @('shift','9') }
                    ')' { Send-Keys @('shift','0') }
                    '>' { Send-Keys @('shift','dot') }
                    '<' { Send-Keys @('shift','comma') }
                    "'" { Send-Keys @('apostrophe') }
                    '"' { Send-Keys @('shift','apostrophe') }
                    '{' { Send-Keys @('shift','bracket_left') }
                    '}' { Send-Keys @('shift','bracket_right') }
                    '[' { Send-Keys @('bracket_left') }
                    ']' { Send-Keys @('bracket_right') }
                    '!' { Send-Keys @('shift','1') }
                    '#' { Send-Keys @('shift','3') }
                    '%' { Send-Keys @('shift','5') }
                    '^' { Send-Keys @('shift','6') }
                    '&' { Send-Keys @('shift','7') }
                    '+' { Send-Keys @('shift','equal') }
                    '?' { Send-Keys @('shift','slash') }
                    '`' { Send-Keys @('grave_accent') }
                    '~' { Send-Keys @('shift','grave_accent') }
                    default { throw "unmapped char: $c" }
                }
            }
        }
        Write-Host "typed"
    }
    'key' {
        foreach ($k in $arg -split '/') { Send-Keys ($k -split ',') }
        Write-Host "keyed"
    }
    'status' {
        $w.WriteLine('{"execute":"query-status"}'); Start-Sleep -Milliseconds 500
        Write-Host $r.ReadLine()
    }
    default { throw "unknown op: $op" }
}
$tcp.Close()
