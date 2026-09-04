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

    $updateMatched = $false
    if (Test-Path 'release/update.json') {
        for ($attempt = 1; $attempt -le 18; $attempt++) {
            curl.exe --fail --silent --show-error --location --output "$work\update.json" "$base/update.json?attempt=$attempt"
            curl.exe --fail --silent --show-error --location --output "$work\update.json.sig" "$base/update.json.sig?attempt=$attempt"
            $expectedManifest = (Get-FileHash 'release/update.json' -Algorithm SHA256).Hash
            $actualManifest = (Get-FileHash "$work\update.json" -Algorithm SHA256).Hash
            $expectedSignature = (Get-FileHash 'release/update.json.sig' -Algorithm SHA256).Hash
            $actualSignature = (Get-FileHash "$work\update.json.sig" -Algorithm SHA256).Hash
            $manifestMatches = $expectedManifest -eq $actualManifest
            $signatureMatches = $expectedSignature -eq $actualSignature
            if ($manifestMatches -and $signatureMatches) {
                $updateMatched = $true
                break
            }
            Start-Sleep -Seconds 10
        }
        if (-not $updateMatched) { throw "$releasePath kept serving stale update metadata" }
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
