param(
  [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.:-]+$')][string]$RemoteHost,
  [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.\\-]+$')][string]$RemoteUser,
  [int]$LocalPort = 18980
)
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot 'Start-Tunnel.ps1'
$existing = Get-ScheduledTask -TaskName KryptonBuildTunnel -ErrorAction SilentlyContinue
if ($existing) {
  if ($existing.Actions.Arguments -notlike "*$scriptPath*") { throw 'An unrelated task uses the KryptonBuildTunnel name.' }
  Stop-ScheduledTask -TaskName KryptonBuildTunnel
}
$currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$scriptPath`" -RemoteHost $RemoteHost -RemoteUser $RemoteUser -LocalPort $LocalPort" `
  -WorkingDirectory $PSScriptRoot
$principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
  -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName KryptonBuildTunnel -Action $action -Principal $principal `
  -Trigger (New-ScheduledTaskTrigger -AtLogOn -User $currentUser) -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName KryptonBuildTunnel
Get-ScheduledTask -TaskName KryptonBuildTunnel | Select-Object TaskName,State | ConvertTo-Json -Compress
