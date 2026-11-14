param([string]$OutputPath = (Join-Path $PSScriptRoot '../../dist/TryOmarchy-7Wdev.exe'))
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($target)) | Out-Null
$app = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../app'))
$env:GOOS = 'windows'
$env:GOARCH = 'amd64'
& go -C $app build -trimpath -ldflags '-H windowsgui -s -w -X main.forkIdentity=7Wdev -X main.defaultDataDirectoryName=TryOmarchy-7Wdev' -o $target .
if ($LASTEXITCODE -ne 0) { throw 'Fork launcher build failed.' }
Write-Output $target
