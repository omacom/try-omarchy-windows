param([string]$title, [string]$keys)
Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type -AssemblyName System.Windows.Forms
[Microsoft.VisualBasic.Interaction]::AppActivate($title) | Out-Null
Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait($keys)
