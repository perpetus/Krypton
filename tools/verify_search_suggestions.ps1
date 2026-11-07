param([ValidateRange(1000, 30000)][int]$Milliseconds = 2000)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$repository = Split-Path $PSScriptRoot -Parent
$suffix = [Guid]::NewGuid().ToString('N')
$suggestionProfile = Join-Path $repository "build/profiles/suggestions-$suffix"
$directory = Join-Path $suggestionProfile 'Default'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$preferences = Join-Path $directory 'Preferences'
@{ browser = @{ check_default_browser = $false } } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $preferences -Encoding utf8
$browserPath = Join-Path $repository 'build/src/out/Default/chrome.exe'
$log = Join-Path $repository "build/suggestions-$suffix.log"
$resultPath = Join-Path $repository "build/suggestions-$suffix.json"
$browser = $null
function Find-Control([string]$Name) {
  $condition = [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $browser.Id)
  $all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $condition)
  foreach ($element in $all) {
    if ($element.Current.Name -ne $Name) { continue }
    $pattern = if ($Name -eq 'Online search suggestions') {
      [System.Windows.Automation.TogglePattern]::Pattern
    } elseif ($Name -eq 'Privacy and security') {
      [System.Windows.Automation.SelectionItemPattern]::Pattern
    } else { [System.Windows.Automation.InvokePattern]::Pattern }
    $available = $null
    if ($element.TryGetCurrentPattern($pattern, [ref]$available)) { return $element }
  }
  return $null
}
function Wait-Control([string]$Name) {
  $deadline = [DateTime]::UtcNow.AddSeconds(12)
  do {
    $control = Find-Control $Name
    if ($control) { return $control }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  $processCondition = [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $browser.Id)
  [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $processCondition) |
      Select-Object @{Name='name';Expression={$_.Current.Name}},@{Name='type';Expression={$_.Current.ControlType.ProgrammaticName}} |
      ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $suggestionProfile 'ui-controls.json')
  throw "The control '$Name' was not exposed. See $log"
}
function Start-TestBrowser {
  $script:browser = Start-Process -FilePath $browserPath -ArgumentList @(
      "--user-data-dir=$suggestionProfile", '--no-first-run',
      '--no-default-browser-check', '--disable-background-networking',
      '--enable-logging', "--log-file=$log", '--window-size=1100,760',
      'about:blank'
  ) -WindowStyle Normal -PassThru
  $null = $browser.WaitForInputIdle(15000)
  $menu = Wait-Control 'Krypton menu'
  $menu.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $settings = Wait-Control 'Settings'
  $settings.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $privacy = Wait-Control 'Privacy and security'
  $privacy.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}
function Close-TestBrowser {
  if (-not $browser.CloseMainWindow() -or -not $browser.WaitForExit(20000)) {
    throw 'The isolated test browser did not close normally.'
  }
  if ($browser.ExitCode -ne 0) { throw "Browser exit code: $($browser.ExitCode)" }
}
function Toggle-State($Control) {
  return $Control.GetCurrentPattern(
      [System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
}
try {
  Start-TestBrowser
  $toggle = Wait-Control 'Online search suggestions'
  if ((Toggle-State $toggle) -ne [System.Windows.Automation.ToggleState]::Off) {
    throw 'Online suggestions were not off in a new profile.'
  }
  $toggle.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
  $confirm = Wait-Control 'Turn on'
  $confirm.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $deadline = [DateTime]::UtcNow.AddSeconds(8)
  do {
    $toggle = Wait-Control 'Online search suggestions'
    if ((Toggle-State $toggle) -eq [System.Windows.Automation.ToggleState]::On) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  if ((Toggle-State $toggle) -ne [System.Windows.Automation.ToggleState]::On) {
    throw 'The confirmed toggle was not acknowledged by the browser.'
  }
  if ($browser.WaitForExit($Milliseconds)) { throw 'Browser exited while settings were open.' }
  Close-TestBrowser
  $enabled = Get-Content -LiteralPath $preferences -Raw | ConvertFrom-Json
  if ($enabled.search.suggest_enabled -ne $true) { throw 'The enabled profile choice was not saved.' }
  Start-TestBrowser
  $toggle = Wait-Control 'Online search suggestions'
  if ((Toggle-State $toggle) -ne [System.Windows.Automation.ToggleState]::On) {
    throw 'The enabled choice did not survive restarting the browser.'
  }
  $toggle.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
  if ($browser.WaitForExit($Milliseconds)) { throw 'Browser exited while disabling suggestions.' }
  Close-TestBrowser
  $disabled = Get-Content -LiteralPath $preferences -Raw | ConvertFrom-Json
  if ($disabled.search.suggest_enabled -ne $false) { throw 'The disabled choice was not saved.' }
  $result = [ordered]@{
    passed = $true; off_by_default = $true; native_confirmation = $true
    persisted_enabled = $true; persisted_disabled = $true
    profile = $suggestionProfile; log = $log
    note = 'No screenshots. Provider traffic and recommendation quality are checked separately.'
  }
  $result | ConvertTo-Json | Set-Content -LiteralPath $resultPath
  $result | ConvertTo-Json
} finally {
  if ($browser -and -not $browser.HasExited) { $null = $browser.CloseMainWindow() }
}
