param(
  [ValidateRange(1,8)][int]$LocalJobs = 4,
  [ValidateRange(1,8)][int]$RemoteJobs = 6,
  [ValidatePattern('^[A-Za-z0-9_/:.-]+$')][string]$Target = 'chrome'
)
$ErrorActionPreference = 'Stop'
$buildRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logRoot = Join-Path $buildRoot 'build\remote-execution'
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$state = [ordered]@{
  state = 'running'; pid = $PID; local_jobs = $LocalJobs; remote_jobs = $RemoteJobs
  coordinator_memory = $(if ($env:GOMEMLIMIT) { $env:GOMEMLIMIT } else { '4GiB' })
  target = $Target; started_utc = [DateTime]::UtcNow.ToString('o'); exit_code = $null
}
$state | ConvertTo-Json | Set-Content (Join-Path $logRoot 'chrome-build-state.json')
$buildExit = 1
Push-Location $buildRoot
try {
  # Windows PowerShell otherwise converts the first native stderr line into
  # a terminating error before the command's actual exit status is available.
  $ErrorActionPreference = 'Continue'
  & python tools/remote_execution/build.py --local-jobs $LocalJobs --remote-jobs $RemoteJobs $Target `
    *> (Join-Path $logRoot 'chrome-build.log')
  $buildExit = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
} catch {
  $_ | Out-String | Add-Content (Join-Path $logRoot 'chrome-build.log')
} finally {
  Pop-Location
  $state.state = if ($buildExit -eq 0) { 'succeeded' } else { 'failed' }
  $state.exit_code = $buildExit
  $state.finished_utc = [DateTime]::UtcNow.ToString('o')
  $state | ConvertTo-Json | Set-Content (Join-Path $logRoot 'chrome-build-state.json')
}
exit $buildExit
