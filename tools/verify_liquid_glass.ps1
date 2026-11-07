param(
  [ValidateSet('Performance', 'Quality')]
  [string]$Mode = 'Quality',
  [ValidateRange(1000, 30000)]
  [int]$Milliseconds = 4000
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$browserPath = Join-Path $repository 'build/src/out/Default/chrome.exe'
$modulePath = Join-Path $repository 'build/src/out/Default/krypton_ui.dll'
if (-not (Test-Path -LiteralPath $browserPath) -or
    -not (Test-Path -LiteralPath $modulePath)) {
  throw 'Build the Release browser and stage KryptonUI before verification.'
}

$suffix = [Guid]::NewGuid().ToString('N')
$profile = Join-Path $repository "build/profiles/glass-$suffix"
$defaultProfile = Join-Path $profile 'Default'
New-Item -ItemType Directory -Path $defaultProfile -Force | Out-Null
$value = if ($Mode -eq 'Quality') { 2 } else { 1 }
@{
  krypton_ui = @{ appearance = @{ glass_mode = $value; color_scheme = 2 } }
  browser = @{ check_default_browser = $false }
} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $defaultProfile 'Preferences') -Encoding utf8

$log = Join-Path $repository "build/glass-$suffix.log"
$report = Join-Path $repository "build/glass-$suffix.json"
$previousStats = $env:KR_GLASS_STATS
$browser = $null
try {
  $env:KR_GLASS_STATS = '1'
  # This is a native integration run in its own profile. No screenshots,
  # normal-profile changes, browser extensions or page readback are involved.
  $arguments = @(
    "--user-data-dir=$profile", '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--enable-logging', "--log-file=$log",
    '--window-size=1100,760', 'about:blank'
  )
  $browser = Start-Process -FilePath $browserPath -ArgumentList $arguments `
      -WindowStyle Normal -PassThru
  $null = $browser.WaitForInputIdle(15000)
  if ($browser.WaitForExit($Milliseconds)) {
    throw "Verification browser exited early ($($browser.ExitCode)). See $log"
  }
  $browser.Refresh()
  $window = $browser.MainWindowHandle
  $responding = $browser.Responding
  if (-not $browser.CloseMainWindow() -or -not $browser.WaitForExit(20000)) {
    throw "Verification browser did not close normally. See $log"
  }
  $browser.Refresh()
  $text = Get-Content -LiteralPath $log -Raw
  $stats = [regex]::Match($text,
      'Krypton glass host: (\d+) copies, (\d+) accepted, (\d+) released, (\d+) failed copies, (\d+) adapter mismatches')
  $gpuFailure = $text -match 'GPU process exited unexpectedly|Received bad user message|VALIDATION_ERROR|FATAL|Check failed'
  $result = [ordered]@{
    mode = $Mode; profile = $profile; log = $log
    window = $window.ToInt64(); responding = $responding
    exit_code = $browser.ExitCode; gpu_failure = $gpuFailure
    copies = if ($stats.Success) { [int]$stats.Groups[1].Value } else { 0 }
    accepted = if ($stats.Success) { [int]$stats.Groups[2].Value } else { 0 }
    released = if ($stats.Success) { [int]$stats.Groups[3].Value } else { 0 }
    failed_copies = if ($stats.Success) { [int]$stats.Groups[4].Value } else { 0 }
    adapter_mismatches = if ($stats.Success) { [int]$stats.Groups[5].Value } else { 0 }
  }
  # Copies happen only while a Quality surface floats over the page (Ctrl+L,
  # menus, panels). A run without one must copy nothing; any copy must land.
  $result.passed = $browser.ExitCode -eq 0 -and $responding -and
      -not $gpuFailure -and $result.failed_copies -eq 0 -and
      ($result.copies -eq 0 -or $result.accepted -gt 0)
  $result | ConvertTo-Json | Set-Content -LiteralPath $report -Encoding utf8
  $result | ConvertTo-Json
  Write-Output "Report: $report"
  if (-not $result.passed) {
    throw "Native glass delivery did not pass. See $report"
  }
} finally {
  $env:KR_GLASS_STATS = $previousStats
  if ($browser -and -not $browser.HasExited) {
    $null = $browser.CloseMainWindow()
  }
}
