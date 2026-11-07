$ErrorActionPreference = 'Stop'
$serviceRoot = 'C:\KryptonBuild'
$task = Get-ScheduledTask -TaskName KryptonRemoteExecution -ErrorAction SilentlyContinue
if ($task) {
  if ($task.Actions.Arguments -notlike "*$serviceRoot\config\Start-Services.ps1*") {
    throw 'An unrelated task uses the KryptonRemoteExecution name.'
  }
  Disable-ScheduledTask -TaskName KryptonRemoteExecution | Out-Null
  Stop-ScheduledTask -TaskName KryptonRemoteExecution
}
$executables = @('storage','scheduler','runner','worker') | ForEach-Object { "$serviceRoot\bin\bb_$_.exe" }
$owned = Get-CimInstance Win32_Process | Where-Object {
  $_.ExecutablePath -in $executables -or
  ($_.Name -eq 'powershell.exe' -and $_.CommandLine -match '-File\s+"?C:\\KryptonBuild\\config\\Start-Services\.ps1')
}
foreach ($process in $owned) {
  $running = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
  if ($running) {
    Stop-Process -Id $running.Id -Force -ErrorAction Stop
    if (-not $running.WaitForExit(5000)) { throw "Build process $($running.Id) did not stop." }
  }
}
