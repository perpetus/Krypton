$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$serviceRoot = 'C:\KryptonBuild'
foreach ($directory in @('bin','config','logs','storage\cas','storage\ac','worker\build','worker\cache')) {
  New-Item -ItemType Directory -Path (Join-Path $serviceRoot $directory) -Force | Out-Null
}
$manifest = Get-Content "$serviceRoot\bin\manifest.json" -Raw | ConvertFrom-Json
foreach ($file in $manifest.files.PSObject.Properties) {
  $hash = (Get-FileHash (Join-Path "$serviceRoot\bin" $file.Name) -Algorithm SHA256).Hash
  if ($hash -ne $file.Value) { throw "Buildbarn binary hash mismatch: $($file.Name)" }
}
# The worker runs as Local Service and accepts traffic only on loopback. SSH
# provides the authenticated transport; no LAN firewall rule is installed.
& icacls.exe $serviceRoot /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not grant Local Service access to the build directory.' }
$existing = Get-ScheduledTask -TaskName KryptonRemoteExecution -ErrorAction SilentlyContinue
if ($existing) {
  $expected = "$serviceRoot\config\Start-Services.ps1"
  if ($existing.Actions.Arguments -notlike "*$expected*") { throw 'An unrelated task uses the KryptonRemoteExecution name.' }
}
& "$serviceRoot\config\Stop-Services.ps1"
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $serviceRoot\config\Start-Services.ps1" `
  -WorkingDirectory $serviceRoot
$principal = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\LOCAL SERVICE' -LogonType ServiceAccount
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
  -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$settings.RunOnlyIfIdle = $false
$settings.UseUnifiedSchedulingEngine = $false
Register-ScheduledTask -TaskName KryptonRemoteExecution -Action $action -Principal $principal `
  -Trigger (New-ScheduledTaskTrigger -AtStartup) -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName KryptonRemoteExecution
$scheduler = New-Object -ComObject Schedule.Service
$scheduler.Connect()
# This explicit request starts the worker now, even when Task Scheduler is
# deferring background tasks under its idle/maintenance constraints.
$null = $scheduler.GetFolder('\').GetTask('KryptonRemoteExecution').RunEx($null, 2, 0, $null)
Get-ScheduledTask -TaskName KryptonRemoteExecution | Select-Object TaskName,State | ConvertTo-Json -Compress
