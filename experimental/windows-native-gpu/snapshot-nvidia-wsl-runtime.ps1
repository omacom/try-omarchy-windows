# Copies installed vendor libraries into a private VM build input. Never publish the output.
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$drivers=@(Get-CimInstance Win32_PnPSignedDriver | Where-Object {
    $_.DeviceClass -eq 'DISPLAY' -and $_.DriverProviderName -eq 'NVIDIA' -and $_.IsSigned
})
if($drivers.Count -ne 1) { throw 'Select one signed NVIDIA display driver before making a private runtime snapshot.' }
$selected=$drivers[0]
# Resolve the active device's driver class internally; do not retain its device ID.
$enumKey='HKLM:\SYSTEM\CurrentControlSet\Enum\'+$selected.DeviceID
$driverClass=(Get-ItemProperty -LiteralPath $enumKey -Name Driver).Driver
$classKey='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$driverClass
$paths=@((Get-ItemProperty -LiteralPath $classKey -Name UserModeDriverNameWsl).UserModeDriverNameWsl)
$matching=@($paths | Where-Object { [IO.Path]::GetFileName($_) -eq 'libnvwgf2umx.so' })
if($matching.Count -ne 1) { throw 'Active driver does not identify one WSL graphics library.' }
$vendorLibrary=(Resolve-Path -LiteralPath ([Environment]::ExpandEnvironmentVariables($matching[0]))).ProviderPath
$store=[IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'))+[IO.Path]::DirectorySeparatorChar
if(-not $vendorLibrary.StartsWith($store,[StringComparison]::OrdinalIgnoreCase)) { throw 'Active WSL library is outside DriverStore.' }
$package=Split-Path -Parent $vendorLibrary
$packageName=Split-Path -Leaf $package
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output) { throw 'Snapshot output already exists; original inputs are retained.' }
New-Item -ItemType Directory -Path $output | Out-Null
$files=[ordered]@{}
foreach($name in @('libnvwgf2umx.so','libnvidia-gpucomp.so')) {
    $source=Join-Path $package $name
    $target=Join-Path $output $name
    $before=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $source -Destination $target
    $copied=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    $after=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if($before -ne $copied -or $before -ne $after) { throw 'Installed vendor library changed during snapshot.' }
    $files[$name]=[ordered]@{sha256=$copied;bytes=(Get-Item -LiteralPath $target).Length}
}
$current=@(Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -eq $selected.DeviceID })
if($current.Count -ne 1 -or $current[0].DriverVersion -ne $selected.DriverVersion -or $current[0].InfName -ne $selected.InfName) {
    throw 'Active NVIDIA driver changed during snapshot; output is incomplete.'
}
$receipt=[ordered]@{schema=1;privateOnly=$true;redistributionApproved=$false;driverProvider='NVIDIA';signedInstalledDriver=$true;
    driverVersion=$selected.DriverVersion;adapterName=$selected.DeviceName;driverStorePackage=$packageName;
    sourceDriverUnchanged=$true;files=$files;collectorSha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()}
$json=($receipt | ConvertTo-Json -Depth 8)+"`n"
[IO.File]::WriteAllText((Join-Path $output 'receipt.json'),$json,[Text.UTF8Encoding]::new($false))
$receipt | ConvertTo-Json -Depth 8
