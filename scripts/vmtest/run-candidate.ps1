# Runs a launcher build inside the Windows test VM as an interactive scheduled
# task and waits for the guest to report ready. Run it inside the VM through
# winps.sh. Interactive tasks are the only way a session started over SSH can
# open windows on the console desktop.
#
#   run-candidate.ps1 -Exe C:\path\TryOmarchy.exe -Dir C:\Users\me\AppData\Local\TryOmarchy `
#       -Release http://172.30.0.1:18080/assets -Sums <sha256 of SHA256SUMS> [-ExtraArgs '-ssh 2223']
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Dir,
  [string]$Release = '',
  [string]$Sums = '',
  [string]$ExtraArgs = '',
  [int]$TimeoutSeconds = 300,
  [string]$TaskName = 'TryOmarchyCandidateRun'
)
$launchArgs = "-dir `"$Dir`" -no-update $ExtraArgs"
if ($Release -ne '') { $launchArgs += " -release $Release -sums-sha256 $Sums -runtime-release $Release -runtime-sums-sha256 $Sums" }
$runner = Join-Path $env:TEMP "$TaskName.cmd"
$exitFile = Join-Path $env:TEMP "$TaskName.exit.txt"
Set-Content -Encoding ascii $runner "@echo off`r`n`"$Exe`" $launchArgs > `"$exitFile`" 2>&1`r`necho exit %ERRORLEVEL% >> `"$exitFile`""
Remove-Item $exitFile -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$runner`""
$principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$env:USERNAME" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 6)
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
$log = Join-Path $Dir 'vm\shell.log'
$before = 0; if (Test-Path $log) { $before = (Get-Content $log).Count }
Start-ScheduledTask -TaskName $TaskName
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $log) { $all = Get-Content $log; if ($all.Count -gt $before -and ($all[$before..($all.Count-1)] -match 'userspace announced ready|---- exiting|FATAL')) { break } }
  Start-Sleep 5
}
if (Test-Path $log) { $all = Get-Content $log; if ($all.Count -gt $before) { $all[$before..($all.Count-1)] } }
Get-Content $exitFile -ErrorAction SilentlyContinue
