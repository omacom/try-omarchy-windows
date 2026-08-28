# Sign a binary or installer with Azure Trusted Signing.
# Runs on Windows only (signtool + the Trusted Signing dlib).
# Prereqs (one-time, see docs/SIGNING.md):
#   winget install Microsoft.Azure.TrustedSigningClientTools
#   winget install Microsoft.WindowsSDK.SignTool   (or any Windows SDK signtool >= 10.0.2261)
#   az login   (an account with the Trusted Signing Certificate Profile Signer role)
# Defaults target the live Trusted Signing setup (see docs/SIGNING.md).
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [string]$Endpoint = 'https://eus.codesigning.azure.net',
    [string]$Account = 'southforgesigning',
    [string]$Profile = 'conduit'
)
$ErrorActionPreference = 'Stop'

$dlib = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'x64' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $dlib) { throw 'Trusted Signing client tools not found - winget install Microsoft.Azure.TrustedSigningClientTools' }

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw 'signtool.exe not found - install a Windows SDK' }

$meta = Join-Path $env:TEMP 'trusted-signing-metadata.json'
@{ Endpoint = $Endpoint; CodeSigningAccountName = $Account; CertificateProfileName = $Profile } |
    ConvertTo-Json | Set-Content $meta -Encoding ascii

& $signtool sign /v /fd SHA256 /tr 'http://timestamp.acs.microsoft.com' /td SHA256 /dlib $dlib /dmdf $meta @Path
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
& $signtool verify /pa /v @Path
