<#
.SYNOPSIS
Inspect an explicitly selected WSL distro and optionally run GPU smoke tests.
.DESCRIPTION
Requires Python 3 inside the distro. Never installs software or changes WSL
configuration. Missing graphics tools are reported as unavailable. Application
checks use temporary projects. The optional Godot check opens brief windows.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Distribution,
    [ValidateSet('CUDA', 'OPTIX', 'HIP', 'ONEAPI')][string[]]$BlenderBackend = @(),
    [switch]$Godot,
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$probePath = Join-Path $PSScriptRoot 'probe.py'
$guestPath = & wsl.exe --distribution $Distribution --exec wslpath -a -u $probePath
if ($LASTEXITCODE -ne 0) { throw "Cannot access the probe in WSL distribution '$Distribution'." }
$probeArguments = @('--distribution', $Distribution, '--exec', 'python3', $guestPath.Trim())
foreach ($backend in $BlenderBackend) { $probeArguments += @('--blender', $backend) }
if ($Godot) { $probeArguments += '--godot' }
$output = & wsl.exe @probeArguments
if ($LASTEXITCODE -ne 0) { throw 'The probe failed. Check that Python 3 and the source directory are accessible inside WSL.' }
$json = $output -join "`n"
$null = $json | ConvertFrom-Json # Reject a non-JSON failure before saving a report.
if ($OutputPath) {
    $destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
    [IO.File]::WriteAllText($destination, $json, [Text.UTF8Encoding]::new($false))
}
$json
