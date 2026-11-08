# Builds the dxghost library and its tests with MSVC, then runs the tests.
#   virtio-dxg/host/build.ps1 [-Out <dir>] [-NoTest]
# QEMU links the same sources with MinGW (see ../qemu/README.md).
param(
    [string]$Out = (Join-Path $env:TEMP 'virtio-dxg-host'),
    [switch]$NoTest
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $vcvars) { throw 'MSVC x64 build tools were not found' }
# vcvars64.bat runs vswhere itself and expects it on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
New-Item -ItemType Directory -Force $Out | Out-Null

$flags = '/nologo /std:c11 /W4 /WX /O2 /Z7 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
    "/I`"$here\src\compat`" /wd4200"
$lib = @('src\dxghost.c', 'src\objects.c', 'src\commands.c', 'src\kmt.c') |
    ForEach-Object { "`"$here\$_`"" }
$steps = @(
    "cl $flags /c $($lib -join ' ') /Fo`"$Out/`"",
    "lib /nologo /OUT:`"$Out\dxghost.lib`" `"$Out\dxghost.obj`" `"$Out\objects.obj`" `"$Out\commands.obj`" `"$Out\kmt.obj`"",
    "cl $flags `"$here\tests\layout_test.c`" /Fo`"$Out/`" /Fe`"$Out\layout_test.exe`"",
    "cl $flags `"$here\tests\executor_test.c`" `"$Out\dxghost.lib`" /Fo`"$Out/`" /Fe`"$Out\executor_test.exe`""
)
foreach ($step in $steps) {
    cmd /c "`"$vcvars`" >nul && $step"
    if ($LASTEXITCODE) { throw "build step failed: $step" }
}
if (-not $NoTest) {
    foreach ($test in 'layout_test', 'executor_test') {
        & "$Out\$test.exe"
        if ($LASTEXITCODE) { throw "$test failed" }
    }
}
