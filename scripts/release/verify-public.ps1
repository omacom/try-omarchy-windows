param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [switch]$Latest,
    [ValidateSet('omacom/try-omarchy-windows', 'tsouth89/try-omarchy-windows')]
    [string]$Repository = 'omacom/try-omarchy-windows',
    [ValidateRange(1, 18)][int]$Attempts = 18,
    [ValidateRange(0, 60)][int]$RetryDelaySeconds = 10
)

$ErrorActionPreference = 'Stop'
$releasePath = if ($Latest) { 'latest/download' } else { "download/$Tag" }
$base = "https://github.com/$repository/releases/$releasePath"
$work = Join-Path $env:RUNNER_TEMP "try-omarchy-public-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $work | Out-Null

function Get-PublicAsset([string]$Name, [int]$Attempt) {
    $destination = Join-Path $work $Name
    Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
    curl.exe --fail --silent --show-error --location --connect-timeout 20 --max-time 120 --output $destination "$base/$($Name)?attempt=$Attempt"
    return ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $destination -PathType Leaf))
}

try {
    $matched = $false
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        $launcherOK = Get-PublicAsset 'TryOmarchy.exe' $attempt
        $checksumOK = Get-PublicAsset 'TryOmarchy.exe.sha256' $attempt
        if ($launcherOK -and $checksumOK) {
            $expected = ((Get-Content "$work/TryOmarchy.exe.sha256" -Raw).Trim() -split '\s+')[0]
            $actual = (Get-FileHash "$work/TryOmarchy.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($expected -cmatch '^[0-9a-f]{64}$' -and $actual -eq $expected) {
                $matched = $true
                break
            }
        }
        Start-Sleep -Seconds $RetryDelaySeconds
    }
    if (-not $matched) { throw "$releasePath kept serving a mismatched launcher and checksum" }

    foreach ($feed in @('update.json', 'update-v2.json')) {
        $updateMatched = $false
        if (-not (Test-Path "release/$feed")) { throw "Missing local $feed" }
        for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
            $metadataOK = Get-PublicAsset $feed $attempt
            $signatureOK = Get-PublicAsset "$feed.sig" $attempt
            if ($metadataOK -and $signatureOK) {
                $manifestMatches = (Get-FileHash "release/$feed").Hash -eq (Get-FileHash "$work\$feed").Hash
                $signatureMatches = (Get-FileHash "release/$feed.sig").Hash -eq (Get-FileHash "$work\$feed.sig").Hash
                if ($manifestMatches -and $signatureMatches) {
                    $updateMatched = $true
                    break
                }
            }
            Start-Sleep -Seconds $RetryDelaySeconds
        }
        if (-not $updateMatched) { throw "$releasePath kept serving stale $feed metadata" }
    }
    $current = Get-Content "$work\update-v2.json" -Raw | ConvertFrom-Json
    if ($current.version -ne $Tag -or $current.launcher.sha256 -ne $actual) {
        throw 'Current update metadata does not match the public launcher and requested version'
    }

    if (-not $Latest) {
        if (-not (Get-PublicAsset 'SHA256SUMS' 1)) {
            throw 'Could not download public SHA256SUMS'
        }
        $fixture = "app/testdata/SHA256SUMS.$Tag"
        if ((Get-FileHash $fixture -Algorithm SHA256).Hash -ne
            (Get-FileHash "$work\SHA256SUMS" -Algorithm SHA256).Hash) {
            throw 'Public SHA256SUMS does not match the source pin'
        }
        curl.exe --fail --silent --show-error --location --connect-timeout 20 --max-time 120 --head "$base/rootfs.ext4.zst" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Public guest image is unavailable' }
    }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Output "Verified public $Repository $releasePath assets."
