$ErrorActionPreference = 'Stop'
$verifier = Join-Path $PSScriptRoot 'verify-public.ps1'
$root = Join-Path ([System.IO.Path]::GetTempPath()) "try-omarchy-verifier-test-$([guid]::NewGuid())"
$previousTemp = $env:RUNNER_TEMP
New-Item -ItemType Directory -Path "$root/release", "$root/app/testdata", "$root/temp" -Force | Out-Null
$env:RUNNER_TEMP = "$root/temp"
Push-Location $root

# Run the real verifier against deterministic downloads, including failed
# requests that leave an older file behind unless the verifier removes it.
function global:curl.exe {
    $url = [string]$args[-1]
    $global:downloadURLs.Add($url)
    $name = ([uri]$url).AbsolutePath.Split('/')[-1]
    $attempt = if ($url -match 'attempt=(\d+)') { [int]$Matches[1] } else { 1 }
    $global:LASTEXITCODE = 0
    $failure = switch ($global:downloadCase) {
        'launcher-failure' { $name -eq 'TryOmarchy.exe' }
        'stale-launcher' { $name -eq 'TryOmarchy.exe' -and $attempt -gt 1 }
        'metadata-failure' { $name -eq 'update-v2.json' }
        'signature-failure' { $name -eq 'update.json.sig' }
        'sums-failure' { $name -eq 'SHA256SUMS' }
        'guest-failure' { $name -eq 'rootfs.ext4.zst' }
        default { $false }
    }
    if ($failure) { $global:LASTEXITCODE = 22; return }
    if ($args -contains '--head') { return }
    $destination = $args[[array]::IndexOf($args, '--output') + 1]
    Copy-Item -LiteralPath "release/$name" -Destination $destination
    if ($global:downloadCase -eq 'stale-launcher' -and $name -eq 'TryOmarchy.exe.sha256' -and $attempt -eq 1) {
        Set-Content -LiteralPath $destination -Value ('0' * 64)
    }
}

try {
    Set-Content release/TryOmarchy.exe 'test launcher'
    $hash = (Get-FileHash release/TryOmarchy.exe).Hash.ToLowerInvariant()
    Set-Content release/TryOmarchy.exe.sha256 "$hash  TryOmarchy.exe"
    $metadata = @{version = 'v1.0.0'; launcher = @{sha256 = $hash}} | ConvertTo-Json -Compress
    foreach ($feed in @('update.json', 'update-v2.json')) {
        Set-Content "release/$feed" $metadata
        Set-Content "release/$feed.sig" 'test signature'
    }
    Set-Content release/SHA256SUMS 'test pinned manifest'
    Copy-Item release/SHA256SUMS app/testdata/SHA256SUMS.v1.0.0

    foreach ($case in @('success', 'launcher-failure', 'stale-launcher', 'metadata-failure', 'signature-failure', 'sums-failure', 'guest-failure', 'wrong-version')) {
        $global:downloadCase = $case
        $global:downloadURLs = [System.Collections.Generic.List[string]]::new()
        if ($case -eq 'wrong-version') {
            Set-Content release/update-v2.json ($metadata.Replace('v1.0.0', 'v2.0.0'))
        }
        $failureMessage = $null
        try { & $verifier -Tag v1.0.0 -Attempts 2 -RetryDelaySeconds 0 | Out-Null }
        catch { $failureMessage = $_.Exception.Message }
        if ($case -eq 'success' -and $failureMessage) { throw "Success case failed: $failureMessage" }
        if ($case -ne 'success' -and -not $failureMessage) { throw "Accepted $case" }
        if ($case -eq 'wrong-version') {
            Set-Content release/update-v2.json $metadata
            if ($failureMessage -notlike '*requested version*') { throw "Wrong version failed for an unrelated reason: $failureMessage" }
        }
        if (Get-ChildItem "$root/temp") { throw "Temporary files remain after $case" }
        Write-Output "ok - $case"
    }

    $global:downloadCase = 'success'
    $global:downloadURLs.Clear()
    & $verifier -Tag v1.0.0 -Latest -Repository tsouth89/try-omarchy-windows -Attempts 1 -RetryDelaySeconds 0 | Out-Null
    if ($global:downloadURLs.Count -ne 6) { throw 'Latest did not check all launcher and feed assets' }
    foreach ($url in $global:downloadURLs) {
        if (-not $url.StartsWith('https://github.com/tsouth89/try-omarchy-windows/releases/latest/download/')) {
            throw "Unexpected legacy Latest URL: $url"
        }
    }
    Write-Output 'ok - legacy Latest feed and launcher URLs'
} finally {
    Pop-Location
    $env:RUNNER_TEMP = $previousTemp
    Remove-Item function:global:curl.exe
    Remove-Variable downloadCase, downloadURLs -Scope Global -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -Recurse -Force
}
