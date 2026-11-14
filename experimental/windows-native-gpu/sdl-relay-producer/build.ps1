param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output) { throw 'Use a fresh producer build directory.' }
$vswhere=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs) { throw 'Visual Studio C++ tools are required.' }
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach($path in @($output,$dev,$PSScriptRoot)) {
    if($path -match '["%&|<>^!\r\n]') { throw 'Unsupported shell characters in build paths.' }
}
New-Item -ItemType Directory -Path $output | Out-Null
$source=Join-Path $PSScriptRoot 'd3d12_named_producer.cpp'
$sourceBefore=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$scriptBefore=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$batch=Join-Path $output 'build.cmd'
@"
@echo off
call "$dev" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b %errorlevel%
cd /d "$output"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$source" /Fe:d3d12-named-producer.exe /link d3d12.lib dxgi.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& cmd.exe /d /c $batch
if($LASTEXITCODE) { throw "Strict producer build failed: $LASTEXITCODE" }
if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sourceBefore) { throw 'Producer source changed during build.' }
if((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $scriptBefore) { throw 'Producer build script changed.' }
@{
    schema=1
    compiler='MSVC /std:c++17 /W4 /WX /O2'
    sourceSha256=$sourceBefore
    buildScriptSha256=$scriptBefore
    binarySha256=(Get-FileHash -LiteralPath (Join-Path $output 'd3d12-named-producer.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'receipt.json') -Encoding utf8
