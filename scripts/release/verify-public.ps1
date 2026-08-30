param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [switch]$Latest
)

$ErrorActionPreference = 'Stop'
$repository = 'tsouth89/try-omarchy-windows'
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
