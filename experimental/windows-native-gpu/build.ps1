param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'build'), [switch]$HighResolutionDiagnostic, [switch]$StreamingDiagnostic)
$ErrorActionPreference = 'Stop'
if ($StreamingDiagnostic) { $HighResolutionDiagnostic = $true }
if ($StreamingDiagnostic -and -not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path $PSScriptRoot 'build-stream'
} elseif ($HighResolutionDiagnostic -and -not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path $PSScriptRoot 'build-4k'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ Build Tools and the Windows SDK.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio C++ toolchain found.' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
# Fixed compiler invocation inside one cmd shell; no filesystem deletion/move.
# Reject shell metacharacters before constructing the quoted batch file.
foreach ($path in @($dev, $PSScriptRoot, $output)) {
    if ($path -match '["%&|<>^\r\n]') { throw 'Build paths contain unsupported shell characters.' }
}
$batch = Join-Path $output 'build-native-gpu.cmd'
$profileDefine = if ($HighResolutionDiagnostic) { '/DWDDM_BRIDGE_4K_LAB=1' } else { '' }
if ($StreamingDiagnostic) { $profileDefine += ' /DWDDM_BRIDGE_STREAM_LAB=1' }
$captureTestDirectory = Join-Path $output ('capture-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $captureTestDirectory | Out-Null
@"
@echo off
call "$dev" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b %errorlevel%
cd /d "$output"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\wire_test.cpp" /Fe:wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qmp_socket_wait_test.cpp" /Fe:qmp-socket-wait-test.exe /link ws2_32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
qmp-socket-wait-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qmp_deadline_budget_test.cpp" /Fe:qmp-deadline-budget-test.exe /link ws2_32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
qmp-deadline-budget-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qmp_relay_transaction_test.cpp" /Fe:qmp-relay-transaction-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
qmp-relay-transaction-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\owned_thread_completion_test.cpp" /Fe:owned-thread-completion-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
owned-thread-completion-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\stopped_group_retirement_poll_test.cpp" /Fe:stopped-group-retirement-poll-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
stopped-group-retirement-poll-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\atomic_line_log_test.cpp" /Fe:atomic-line-log-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
atomic-line-log-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\launcher_session_stream_test.cpp" /Fe:launcher-session-stream-test.exe /link ws2_32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
launcher-session-stream-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\driver_wire_test.cpp" /Fe:driver-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
driver-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\framed_write_test.cpp" /Fe:framed-write-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
framed-write-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\driver_actor_router_test.cpp" /Fe:driver-actor-router-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
driver-actor-router-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\driver_coordinator_test.cpp" /Fe:driver-coordinator-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
driver-coordinator-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\bounded_gpu_retirement_wait_test.cpp" /Fe:bounded-gpu-retirement-wait-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
bounded-gpu-retirement-wait-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\checked_fence_completion_test.cpp" /Fe:checked-fence-completion-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
checked-fence-completion-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\application_memory_profile_test.cpp" /Fe:application-memory-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
application-memory-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\mapped_residency_admission_test.cpp" /Fe:mapped-residency-admission-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
mapped-residency-admission-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\deferred_residency_test.cpp" /Fe:deferred-residency-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
deferred-residency-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\gpu_mapping_accounting_test.cpp" /Fe:gpu-mapping-accounting-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
gpu-mapping-accounting-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\fair_rpc_mutex_test.cpp" /Fe:fair-rpc-mutex-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
fair-rpc-mutex-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\readonly_rootfs_args_test.cpp" /Fe:readonly-rootfs-args-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
readonly-rootfs-args-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\owned_qemu_process_test.cpp" /Fe:owned-qemu-process-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
owned-qemu-process-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\launcher_lifetime_test.cpp" /Fe:launcher-lifetime-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
launcher-lifetime-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\launcher_profile_test.cpp" /Fe:launcher-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
launcher-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\launcher_binding_fixture.cpp" /Fe:launcher-binding-fixture.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qemu_graphics_arguments_test.cpp" /Fe:qemu-graphics-arguments-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
qemu-graphics-arguments-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qemu_launcher_arguments_test.cpp" /Fe:qemu-launcher-arguments-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_resource_preparation_test.cpp" /Fe:native-resource-preparation-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-resource-preparation-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_buffer_preparation_test.cpp" /Fe:native-buffer-preparation-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-buffer-preparation-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_metadata_attempt_test.cpp" /Fe:native-metadata-attempt-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-metadata-attempt-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\sync_sharing_wire_test.cpp" /Fe:sync-sharing-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
sync-sharing-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_worker_protocol_test.cpp" /Fe:native-worker-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-worker-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_worker_process_test.cpp" /Fe:native-worker-process-test.exe /link d3d12.lib dxgi.lib gdi32.lib user32.lib dwmapi.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-worker-process-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\cpu_aperture_test.cpp" /Fe:cpu-aperture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cpu-aperture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\shared_buffer_profile_test.cpp" /Fe:shared-buffer-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
shared-buffer-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\imported_gpuva_profile_test.cpp" /Fe:imported-gpuva-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
imported-gpuva-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DWDDM_BRIDGE_4K_LAB=1 "$PSScriptRoot\cpu_capacity_test.cpp" /Fe:cpu-capacity-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cpu-capacity-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qmp_protocol_test.cpp" /Fe:qmp-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
qmp-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\bounded_input_channel_test.cpp" /Fe:bounded-input-channel-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
bounded-input-channel-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\staging_reclaim_test.cpp" /Fe:staging-reclaim-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
staging-reclaim-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\tcp_peer_owner_test.cpp" /Fe:tcp-peer-owner-test.exe /link ws2_32.lib iphlpapi.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
tcp-peer-owner-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\compositor_audit_tiles_test.cpp" /Fe:compositor-audit-tiles-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
compositor-audit-tiles-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\sdl-relay-output\control_test.cpp" /Fe:sdl-output-control-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
sdl-output-control-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine /DUNICODE /D_UNICODE "$PSScriptRoot\driver_bridge.cpp" /Fe:driver-bridge.exe /link d3d12.lib dxgi.lib gdi32.lib ws2_32.lib bcrypt.lib user32.lib dwmapi.lib iphlpapi.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\rgba_oracle.cpp" /Fe:rgba-oracle.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\native_stream_probe.cpp" /Fe:native-stream-probe.exe /link d3d12.lib dxgi.lib user32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\diagnostic_profile_test.cpp" /Fe:diagnostic-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
diagnostic-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\binary_capture_test.cpp" /Fe:binary-capture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
set "WDDM_BRIDGE_CAPTURE_DIRECTORY=$captureTestDirectory"
binary-capture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\context_native_probe.cpp" /Fe:context-native-probe.exe /link dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\query_native_probe.cpp" /Fe:query-native-probe.exe /link dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\queue_dependency_probe.cpp" /Fe:queue-dependency-probe.exe /link dxgi.lib gdi32.lib WinHvPlatform.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\vendor_lock_probe.cpp" /Fe:vendor-lock-probe.exe /link dxgi.lib gdi32.lib WinHvPlatform.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\sync_sharing_probe.cpp" /Fe:sync-sharing-probe.exe /link d3d12.lib dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\shared_reference_alias_probe.cpp" /Fe:shared-reference-alias-probe.exe /link d3d12.lib dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE "$PSScriptRoot\native_gpu.cpp" /Fe:native-gpu.exe /link d3d12.lib dxgi.lib user32.lib gdi32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw "Native GPU build failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party/nlohmann-json/LICENSE.MIT') -Destination (Join-Path $output 'nlohmann-json-LICENSE.MIT') -Force
