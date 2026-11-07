param(
  [ValidateRange(1,8)][int]$LocalJobs = 4,
  [ValidateRange(1,8)][int]$RemoteJobs = 6,
  [ValidatePattern('^[A-Za-z0-9_/:.-]+$')][string]$Target = 'chrome'
)
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot 'Run-Build.ps1'
$existing = Get-ScheduledTask -TaskName KryptonChromiumBuild -ErrorAction SilentlyContinue
if ($existing) {
  if ($existing.Actions.Arguments -notlike "*$scriptPath*") { throw 'An unrelated task uses the KryptonChromiumBuild name.' }
  if ($existing.State -eq 'Running') { throw 'The managed Chromium build is already running.' }
}
$active = Get-CimInstance Win32_Process | Where-Object {
  $_.Name -eq 'siso.exe' -and $_.CommandLine -match '(out[/\\]Default|out/Default)'
}
if ($active) { throw 'A Chromium Siso build is already active. No duplicate build was started.' }
$scheduler = New-Object -ComObject Schedule.Service
$scheduler.Connect()
$tunnel = Get-ScheduledTask -TaskName KryptonBuildTunnel -ErrorAction SilentlyContinue
if (-not $tunnel) { throw 'Install the SSH build tunnel before starting a managed build.' }
if ($tunnel.State -ne 'Running') {
  $null = $scheduler.GetFolder('\').GetTask('KryptonBuildTunnel').RunEx($null,2,0,$null)
}
$tunnelReady = $false
for ($attempt = 0; $attempt -lt 20 -and -not $tunnelReady; ++$attempt) {
  $probe = New-Object Net.Sockets.TcpClient
  try { $probe.Connect('127.0.0.1',18980); $tunnelReady = $true } catch {} finally { $probe.Dispose() }
  if (-not $tunnelReady) { Start-Sleep -Milliseconds 250 }
}
if (-not $tunnelReady) { throw 'The SSH build tunnel did not start; check the worker connection.' }
$currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$scriptPath`" -LocalJobs $LocalJobs -RemoteJobs $RemoteJobs -Target $Target" `
  -WorkingDirectory $PSScriptRoot
$principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName KryptonChromiumBuild -Action $action -Principal $principal `
  -Settings $settings -Force | Out-Null
$null = $scheduler.GetFolder('\').GetTask('KryptonChromiumBuild').RunEx($null,2,0,$null)
Get-ScheduledTask -TaskName KryptonChromiumBuild | Select-Object TaskName,State | ConvertTo-Json -Compress
