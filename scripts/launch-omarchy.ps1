# Try Omarchy for Windows - launcher (developer preview).
# Boots Omarchy in an SDL window. GPU-accelerated (WINQ-EMU: virgl + Venus Vulkan)
# when WINQ-EMU is installed at C:\WINQ-EMU, CPU rendering (llvmpipe) otherwise.
#
# Supervises the VM so users never deal with WHPX's rough edges:
#   - launch watchdog: WHPX sometimes wedges QEMU ~1.3s into kernel boot (main loop
#     dead, window Not Responding). Detected via QMP handshake; kill + retry.
#   - guest poweroff wedges stock QEMU at the final ACPI transition; the supervisor
#     sees the SHUTDOWN event and reaps the husk so the app exits cleanly.
#   - guest reboot (SHUTDOWN reason guest-reset under -no-reboot) relaunches the VM.
# Uses the windowless qemu-system-x86_64w.exe so no console window ever appears;
# QEMU's own messages go to vm\qemu.log. The winkey-forwarder (hidden) scopes the
# Windows key to the VM window and keeps the window titled "Try Omarchy".
#
#   powershell -ExecutionPolicy Bypass -File launch-omarchy.ps1 [-Fullscreen] [-Fresh] [-NoGpu]
param(
    [string]$Dir = "$env:LOCALAPPDATA\TryOmarchy",
    [string]$WinqEmu = 'C:\WINQ-EMU',
    [switch]$Fullscreen,
    [switch]$Fresh,    # discard the writable disk and start over
    [switch]$NoGpu,    # force CPU rendering even if WINQ-EMU is installed
    [string]$Share = ''  # host folder shared into the guest over virtio-9p (GPU mode
                         # only - WINQ-EMU ships 9p, stock QEMU for Windows does not).
                         # In-guest: sudo mount -t 9p -o trans=virtio hostshare /mnt/host
)
$ErrorActionPreference = 'Stop'
$QmpToolsPort = 4445   # free for qmp.ps1 / provisioning tooling
$QmpFwdPort   = 4446   # winkey-forwarder
$QmpSupPort   = 4447   # this script's watchdog + lifecycle supervision
$ClipPushPort = 4448   # clipboard bridge: guest -> host
$ClipPullPort = 4449   # clipboard bridge: host -> guest

$g = Join-Path $Dir 'guest'
$vm = Join-Path $Dir 'vm'
foreach ($f in 'build-spec.json', 'vmlinuz-linux', 'initramfs-linux.img', 'rootfs.ext4') {
    if (-not (Test-Path (Join-Path $g $f))) { throw "Missing $g\$f - run bootstrap.ps1 first." }
}

$useGpu = $false
$gpuQemu = Join-Path $WinqEmu 'bin\qemu-system-x86_64w.exe'
if ((-not $NoGpu) -and (Test-Path $gpuQemu)) { $useGpu = $true }
if ($useGpu) { $qemu = $gpuQemu } else { $qemu = 'C:\Program Files\qemu\qemu-system-x86_64w.exe' }
if (-not (Test-Path $qemu)) { throw "QEMU not found at $qemu - run bootstrap.ps1 first." }
New-Item -ItemType Directory -Path $vm -Force | Out-Null

$spec = Get-Content (Join-Path $g 'build-spec.json') | ConvertFrom-Json
$cmdline = $spec.runtime.kernelCommandLine -replace 'console=hvc0', 'console=ttyS0 console=tty1'
$expandedMiB = $spec.runtime.storage.expandedSizeMiB

$disk = Join-Path $vm 'disk.raw'
if ($Fresh -and (Test-Path $disk)) { Remove-Item $disk -Force }
if (-not (Test-Path $disk)) {
    Write-Host 'Preparing writable disk (sparse copy)...'
    Copy-Item (Join-Path $g 'rootfs.ext4') $disk
    fsutil sparse setflag $disk | Out-Null
    $fs = [IO.File]::Open($disk, 'Open', 'ReadWrite')
    try { $fs.SetLength([int64]$expandedMiB * 1MB) } finally { $fs.Dispose() }
}

$qemuArgs = @(
    '-drive', "file=$disk,format=raw,if=virtio",
    '-kernel', (Join-Path $g 'vmlinuz-linux'),
    '-initrd', (Join-Path $g 'initramfs-linux.img'),
    '-append', $cmdline,
    '-device', 'virtio-keyboard-pci', '-device', 'virtio-tablet-pci',
    '-device', 'virtio-net-pci,netdev=n0', '-netdev', 'user,id=n0',
    '-device', 'virtio-rng-pci',
    '-device', 'virtio-sound-pci',
    '-qmp', "tcp:127.0.0.1:$QmpToolsPort,server=on,wait=off",
    '-qmp', "tcp:127.0.0.1:$QmpFwdPort,server=on,wait=off",
    '-qmp', "tcp:127.0.0.1:$QmpSupPort,server=on,wait=off",
    '-D', (Join-Path $vm 'qemu.log'),
    # In-guest reboot/poweroff wedges upstream WHPX (vCPUs never return from system
    # reset). Exit instead; the supervisor loop below relaunches on guest reset.
    '-no-reboot',
    '-name', 'Try Omarchy'
)
if ($useGpu) {
    # WINQ-EMU's patched WHPX survives -cpu host (XSAVE/AVX included); virtio-vga-gl
    # IS the VGA device - no -vga none, no two-display trap. See docs/FINDINGS.md.
    $mode = 'GPU accelerated (virgl + Venus Vulkan)'
    $qemuArgs = @(
        '-machine', 'q35,accel=whpx', '-cpu', 'host', '-smp', '6', '-m', '6G',
        '-device', 'virtio-vga-gl,blob=on,hostmem=4G,venus=on',
        '-display', 'sdl,gl=on',
        '-serial', "file:$vm\serial-gpu.log"
    ) + $qemuArgs
} else {
    # CPU: qemu64 + every feature upstream WHPX survives. Do NOT add AVX/XSAVE
    # features with stock QEMU - the guest kernel panics in fpstate_reset.
    # -vga none is mandatory with virtio-gpu-pci (invisible-second-display trap).
    $mode = 'CPU rendering (llvmpipe)'
    $qemuArgs = @(
        '-machine', 'q35,accel=whpx', '-cpu', 'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes',
        '-smp', '6', '-m', '4096',
        '-vga', 'none', '-device', 'virtio-gpu-pci,id=gpu0',
        '-display', 'sdl,gl=off',
        '-serial', "file:$vm\serial.log"
    ) + $qemuArgs
}
if ($Share) {
    if (-not (Test-Path $Share -PathType Container)) { throw "Share folder not found: $Share" }
    if ($useGpu) {
        $qemuArgs += @('-virtfs', "local,path=$Share,mount_tag=hostshare,security_model=none")
    } else {
        Write-Host 'Folder sharing needs the WINQ-EMU build (stock QEMU for Windows has no virtio-9p) - ignoring -Share.' -ForegroundColor Yellow
    }
}
if ($Fullscreen) { $qemuArgs += '-full-screen' }
$argStr = ($qemuArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '

function Connect-Qmp([int]$port, [int]$readTimeoutMs) {
    # Full handshake (greeting + qmp_capabilities). Returns $null unless QEMU's main
    # loop actually answers - a wedged QEMU accepts the TCP connect but never talks.
    $tcp = New-Object Net.Sockets.TcpClient
    try {
        $iar = $tcp.BeginConnect('127.0.0.1', $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(3000)) { $tcp.Close(); return $null }
        $tcp.EndConnect($iar)
        $s = $tcp.GetStream(); $s.ReadTimeout = $readTimeoutMs
        $r = New-Object IO.StreamReader($s)
        $w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
        if ($null -eq $r.ReadLine()) { $tcp.Close(); return $null }   # greeting
        $w.WriteLine('{"execute":"qmp_capabilities"}')
        if ($null -eq $r.ReadLine()) { $tcp.Close(); return $null }   # {"return":{}}
        return @{ Tcp = $tcp; Reader = $r; Writer = $w }
    } catch { $tcp.Close(); return $null }
}

# SDL's keyboard grab installs a system-wide Win-key hook that leaks past window
# focus. Disable it; winkey-forwarder.ps1 does it right (focus-scoped, forwards
# Super to the guest over QMP) and keeps the window title clean.
$env:SDL_GRAB_KEYBOARD = '0'
$fwd = Join-Path $PSScriptRoot 'winkey-forwarder.ps1'
$fwdProc = Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$fwd`" -QmpPort $QmpFwdPort" -WindowStyle Hidden -PassThru

# Two-way text clipboard sync; the guest-side daemon (scripts/guest/clipboard-bridge.sh)
# connects out to these ports over user-net. Harmless if the guest lacks the daemon.
$clip = Join-Path $PSScriptRoot 'clipboard-bridge.ps1'
$clipProc = Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$clip`" -PushPort $ClipPushPort -PullPort $ClipPullPort" -WindowStyle Hidden -PassThru

$proc = $null
try {
    do {
        $relaunch = $false

        # --- launch, with wedge watchdog ---
        $qmp = $null
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            Write-Host "Booting Omarchy - $mode (Ctrl+Alt+G toggles mouse grab, Ctrl+Alt+F fullscreen)..."
            $proc = Start-Process -FilePath $qemu -ArgumentList $argStr -PassThru
            $deadline = (Get-Date).AddSeconds(30)
            while ($null -eq $qmp -and (Get-Date) -lt $deadline -and -not $proc.HasExited) {
                Start-Sleep -Milliseconds 1500
                $qmp = Connect-Qmp $QmpSupPort 8000
            }
            if ($qmp) { break }
            if ($proc.HasExited) { throw "QEMU exited at startup (code $($proc.ExitCode)) - see $vm\qemu.log." }
            Write-Host 'QEMU is not answering (known WHPX launch wedge) - killing and retrying...' -ForegroundColor Yellow
            Stop-Process -Id $proc.Id -Force
            Start-Sleep -Seconds 2
        }
        if ($null -eq $qmp) { throw 'QEMU failed to come up healthy after 4 attempts.' }

        # --- supervise until the guest goes down ---
        # Two hard-won subtleties here (see docs/FINDINGS.md):
        # 1. A QEMU wedged at guest poweroff cannot deliver its SHUTDOWN event (the
        #    main loop dies first), so liveness is probed: query-status every ~5s;
        #    any line back proves the main loop is alive; ~45s of silence despite
        #    pings means it is gone. Generous on purpose - never kill a healthy VM.
        # 2. With -no-reboot, QEMU can exit so fast after a guest reset that an
        #    abortive socket close DISCARDS an unread SHUTDOWN event. Keep an async
        #    read permanently pending so the event is consumed the moment it
        #    arrives - a sleep-then-read loop loses that race.
        $reason = ''
        $silent = 0
        $tick = 0
        $pending = $qmp.Reader.ReadLineAsync()
        while (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 1000
            $tick++
            while ($pending -and $pending.IsCompleted) {
                $line = $null
                try { $line = $pending.Result } catch { }
                if ($null -eq $line) { $pending = $null; break }   # stream ended
                $silent = 0
                if ($line -match '"event":\s*"SHUTDOWN"') {
                    if ($line -match 'guest-reset') { $reason = 'reboot' } else { $reason = 'poweroff' }
                }
                $pending = $qmp.Reader.ReadLineAsync()
            }
            if ($reason) { break }
            if ($null -eq $pending) { break }   # connection gone; QEMU is exiting
            if ($tick % 5 -eq 0) {
                try { $qmp.Writer.WriteLine('{"execute":"query-status"}') } catch { break }
                $silent++
                if ($silent -ge 9) { Write-Host 'QEMU main loop stopped answering - guest is down.' -ForegroundColor Yellow; break }
            }
        }
        # Collect anything the pending read completed with after the loop ended.
        while ((-not $reason) -and $pending -and $pending.IsCompleted) {
            $line = $null
            try { $line = $pending.Result } catch { }
            if ($null -eq $line) { break }
            if ($line -match '"event":\s*"SHUTDOWN"') {
                if ($line -match 'guest-reset') { $reason = 'reboot' } else { $reason = 'poweroff' }
            }
            $pending = $qmp.Reader.ReadLineAsync()
        }
        if (-not $proc.HasExited) {
            # Guest is down. Stock WHPX QEMU wedges here instead of exiting - reap it.
            if (-not $proc.WaitForExit(15000)) {
                Write-Host 'QEMU wedged after guest shutdown (stock WHPX trap) - cleaning up.' -ForegroundColor Yellow
                Stop-Process -Id $proc.Id -Force
            }
        }
        $qmp.Tcp.Close()
        if ($reason -eq 'reboot') { Write-Host 'Guest rebooted - relaunching...'; $relaunch = $true }
    } while ($relaunch)
} finally {
    if ($fwdProc -and -not $fwdProc.HasExited) { Stop-Process -Id $fwdProc.Id -Force }
    if ($clipProc -and -not $clipProc.HasExited) { Stop-Process -Id $clipProc.Id -Force }
}
