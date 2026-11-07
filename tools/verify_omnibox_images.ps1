param([Parameter(Mandatory)][string]$Profile, [Parameter(Mandatory)][string]$ResultPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$repository = Split-Path $PSScriptRoot -Parent
$netlog = "$ResultPath.netlog.json"
$browser = Start-Process -FilePath (Join-Path $repository 'build/src/out/Default/chrome.exe') -ArgumentList @(
    "--user-data-dir=$Profile", '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--disable-extensions', '--window-size=1100,760',
    "--log-net-log=$netlog",
    'about:blank'
) -WindowStyle Normal -PassThru
$null = $browser.WaitForInputIdle(15000)
$processCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $browser.Id)
function Elements($Type, [string]$Name = '') {
  $all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $processCondition)
  return @($all | Where-Object { $_.Current.ControlType -eq $Type -and (-not $Name -or $_.Current.Name -eq $Name) })
}
function Wait-Control($Type, [string]$Name) {
  $deadline = [DateTime]::UtcNow.AddSeconds(12)
  do {
    $control = Elements $Type $Name | Select-Object -First 1
    if ($control) { return $control }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "Native control missing: $Name"
}
try {
  $menu = Wait-Control ([System.Windows.Automation.ControlType]::Button) 'Krypton menu'
  $menu.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $settings = Wait-Control ([System.Windows.Automation.ControlType]::MenuItem) 'Settings'
  $settings.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $search = Wait-Control ([System.Windows.Automation.ControlType]::ListItem) 'Search'
  $search.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
    $choice = Elements ([System.Windows.Automation.ControlType]::ComboBox) | Where-Object { $_.Current.IsEnabled -and -not $_.Current.IsOffscreen } | Select-Object -First 1
    if ($choice) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  if (-not $choice) { throw 'The native search-engine choice was not exposed.' }
  $choice.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
  $google = Wait-Control ([System.Windows.Automation.ControlType]::ListItem) 'Google'
  $google.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
  Start-Sleep -Milliseconds 250
  $ancestor = $choice
  for ($level = 0; $ancestor -and $level -lt 15; ++$level) {
    $window = $null
    if ($ancestor.TryGetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern, [ref]$window)) { $window.Close(); break }
    $ancestor = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($ancestor)
  }
  $address = Wait-Control ([System.Windows.Automation.ControlType]::Button) 'Search or enter address'
  $address.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $edit = Wait-Control ([System.Windows.Automation.ControlType]::Edit) 'Search or enter address'
  $edit.SetFocus()
  $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('hello')
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  do {
    $rows = Elements ([System.Windows.Automation.ControlType]::ListItem)
    $names = @($rows | ForEach-Object { $_.Current.Name })
    $imageCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Image)
    $large = @($rows | Where-Object { $_.Current.Name -match 'hello' } | ForEach-Object {
      $_.FindAll([System.Windows.Automation.TreeScope]::Descendants, $imageCondition)
    } | Where-Object { $_.Current.BoundingRectangle.Width -ge 27 })
    $remote = @($names | Where-Object { $_ -match 'hello' })
    if ($remote.Count -ge 10 -and $large.Count -ge 1) { break }
    Start-Sleep -Milliseconds 150
  } while ([DateTime]::UtcNow -lt $deadline)
  $result = [ordered]@{
    passed = $remote.Count -ge 10 -and $large.Count -ge 1
    matches = $remote; image_count = $large.Count
    image_widths = @($large | ForEach-Object { $_.Current.BoundingRectangle.Width })
    profile = $Profile; netlog = $netlog
  }
  $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ResultPath -Encoding utf8
  $result | ConvertTo-Json -Depth 5
  if (-not $result.passed) { throw 'Native rich suggestion acceptance failed.' }
} finally {
  $null = $browser.CloseMainWindow()
  if (-not $browser.WaitForExit(15000)) {
    $windows = Elements ([System.Windows.Automation.ControlType]::Window)
    foreach ($window in $windows) {
      $close = $null
      if ($window.TryGetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern, [ref]$close)) { $close.Close() }
    }
    $null = $browser.WaitForExit(5000)
  }
}
