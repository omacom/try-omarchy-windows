param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output) { throw 'Use a fresh interop build directory.' }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs) { throw 'Visual Studio C++ tools are required.' }
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach($path in @($output,$dev,$PSScriptRoot)) {
    if($path -match '["%&|<>^\r\n]') { throw 'Unsupported shell characters in build paths.' }
}
New-Item -ItemType Directory -Path $output | Out-Null
$source=Join-Path $PSScriptRoot 'sdl_d3d12_interop.cpp'
$before=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$binding=Join-Path $PSScriptRoot 'gl_external_api.h'
$bindingBefore=(Get-FileHash -LiteralPath $binding -Algorithm SHA256).Hash.ToLowerInvariant()
$scriptBefore=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$batch=Join-Path $output 'build.cmd'
@"
@echo off
call "$dev" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b %errorlevel%
cd /d "$output"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$source" /Fe:sdl-d3d12-interop.exe /link d3d12.lib dxgi.lib opengl32.lib bcrypt.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& cmd.exe /d /c $batch
if($LASTEXITCODE) { throw "Strict interop build failed: $LASTEXITCODE" }
if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $before) { throw 'Source changed during build.' }
if((Get-FileHash -LiteralPath $binding -Algorithm SHA256).Hash.ToLowerInvariant() -ne $bindingBefore) { throw 'ABI declarations changed during build.' }
if((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $scriptBefore) { throw 'Build script changed during build.' }
@{
    schema=1
    sourceSha256=$before
    binarySha256=(Get-FileHash -LiteralPath (Join-Path $output 'sdl-d3d12-interop.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    bindingHeaderSha256=$bindingBefore
    buildScriptSha256=$scriptBefore
    compiler='MSVC /std:c++17 /W4 /WX /O2'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'receipt.json') -Encoding utf8
