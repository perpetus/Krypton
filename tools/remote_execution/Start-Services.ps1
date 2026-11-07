$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$serviceRoot = 'C:\KryptonBuild'
$env:GOMAXPROCS = '2'
$env:GOMEMLIMIT = '256MiB'
$children = @()
try {
  foreach ($service in @('storage', 'scheduler', 'runner', 'worker')) {
    $process = Start-Process -FilePath "$serviceRoot\bin\bb_$service.exe" `
      -ArgumentList @("$serviceRoot\config\$service.json") `
      -WorkingDirectory $serviceRoot -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput "$serviceRoot\logs\$service.stdout.log" `
      -RedirectStandardError "$serviceRoot\logs\$service.stderr.log"
    $children += $process
  }
  $children | Select-Object Id,ProcessName | ConvertTo-Json | Set-Content "$serviceRoot\services.json"
  while ($true) {
    foreach ($process in $children) {
      if ($process.HasExited) {
        throw "Buildbarn $($process.ProcessName) exited: $($process.ExitCode). Check $serviceRoot\logs."
      }
    }
    Start-Sleep -Seconds 5
  }
} finally {
  foreach ($process in $children) {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
  }
}
