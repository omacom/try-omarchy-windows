param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [switch]$Latest
)

$ErrorActionPreference = 'Stop'
$repository = 'omacom/try-omarchy-windows'
$releasePath = if ($Latest) { 'latest/download' } else { "download/$Tag" }
$base = "https://github.com/$repository/releases/$releasePath"
$work = Join-Path $env:RUNNER_TEMP "try-omarchy-public-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $work | Out-Null

try {
    $matched = $false
    for ($attempt = 1; $attempt -le 18; $attempt++) {
        curl.exe --fail --silent --show-error --location --output "$work\TryOmarchy.exe" "$base/TryOmarchy.exe?attempt=$attempt"
        curl.exe --fail --silent --show-error --location --output "$work\TryOmarchy.exe.sha256" "$base/TryOmarchy.exe.sha256?attempt=$attempt"
        $expected = ((Get-Content "$work\TryOmarchy.exe.sha256" -Raw).Trim() -split '\s+')[0]
        $actual = (Get-FileHash "$work\TryOmarchy.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $expected) {
            $matched = $true
            break
        }
        Start-Sleep -Seconds 10
    }
    if (-not $matched) { throw "$releasePath kept serving a mismatched launcher and checksum" }

    foreach ($feed in @('update.json', 'update-v2.json')) {
        $updateMatched = $false
        if (-not (Test-Path "release/$feed")) { throw "Missing local $feed" }
        for ($attempt = 1; $attempt -le 18; $attempt++) {
            curl.exe --fail --silent --show-error --location --output "$work\$feed" "$base/$($feed)?attempt=$attempt"
            $metadataOK = $LASTEXITCODE -eq 0
            curl.exe --fail --silent --show-error --location --output "$work\$feed.sig" "$base/$($feed).sig?attempt=$attempt"
            $signatureOK = $LASTEXITCODE -eq 0
            if ($metadataOK -and $signatureOK) {
                $manifestMatches = (Get-FileHash "release/$feed").Hash -eq (Get-FileHash "$work\$feed").Hash
                $signatureMatches = (Get-FileHash "release/$feed.sig").Hash -eq (Get-FileHash "$work\$feed.sig").Hash
                if ($manifestMatches -and $signatureMatches) {
                    $updateMatched = $true
                    break
                }
            }
            Start-Sleep -Seconds 10
        }
        if (-not $updateMatched) { throw "$releasePath kept serving stale $feed metadata" }
    }
    $current = Get-Content "$work\update-v2.json" -Raw | ConvertFrom-Json
    if ($current.version -ne $Tag -or $current.launcher.sha256 -ne $actual) {
        throw 'Current update metadata does not match the public launcher and requested version'
    }

    if (-not $Latest) {
        curl.exe --fail --silent --show-error --location --output "$work\SHA256SUMS" "$base/SHA256SUMS"
        $fixture = "app/testdata/SHA256SUMS.$Tag"
        if ((Get-FileHash $fixture -Algorithm SHA256).Hash -ne
            (Get-FileHash "$work\SHA256SUMS" -Algorithm SHA256).Hash) {
            throw 'Public SHA256SUMS does not match the source pin'
        }
        curl.exe --fail --silent --show-error --location --head "$base/rootfs.ext4.zst" | Out-Null
    }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Output "Verified public $releasePath assets."
