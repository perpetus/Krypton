param([ValidateRange(1000, 30000)][int]$Milliseconds = 2000)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$repository = Split-Path $PSScriptRoot -Parent
$suffix = [Guid]::NewGuid().ToString('N')
$folderProfile = Join-Path $repository "build/profiles/folders-$suffix"
$directory = Join-Path $folderProfile 'Default'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$preferences = Join-Path $directory 'Preferences'
@{
  krypton_ui = @{
    appearance = @{ color_scheme = 2 }
    folders = @{
      next_id = '3'
      items = @(
        @{ id = '1'; space = '1'; parent = '0'; name = 'Folder smoke root'; glyph = ''; order = '0'; collapsed = $false },
        @{ id = '2'; space = '1'; parent = '1'; name = 'Folder smoke child'; glyph = ''; order = '0'; collapsed = $true }
      )
    }
  }
  browser = @{ check_default_browser = $false }
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $preferences -Encoding utf8
$browserPath = Join-Path $repository 'build/src/out/Default/chrome.exe'
$log = Join-Path $repository "build/folders-$suffix.log"
$browser = $null
function Find-Control([string]$Name, [int]$ProcessId) {
  $processCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
  $nameCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name,
    [System.Windows.Automation.PropertyConditionFlags]::IgnoreCase)
  $condition = [System.Windows.Automation.AndCondition]::new($processCondition, $nameCondition)
  return [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
    [System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Wait-Control([string]$Name, [int]$ProcessId) {
  $deadline = [DateTime]::UtcNow.AddSeconds(8)
  do {
    $control = Find-Control $Name $ProcessId
    if ($control) { return $control }
    Start-Sleep -Milliseconds 150
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "The native control '$Name' was not exposed. See $log"
}
try {
  $browser = Start-Process -FilePath $browserPath -ArgumentList @(
    "--user-data-dir=$folderProfile", '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--enable-logging', "--log-file=$log",
    '--window-size=1100,760', 'about:blank'
  ) -WindowStyle Normal -PassThru
  $null = $browser.WaitForInputIdle(15000)
  $null = Wait-Control 'Folder smoke root' $browser.Id
  $null = Wait-Control 'Folder smoke child' $browser.Id
  $create = Wait-Control 'Create' $browser.Id
  $create.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $newFolder = Wait-Control 'New folder' $browser.Id
  $newFolder.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $null = Wait-Control 'Folder' $browser.Id
  $folderControl = Wait-Control 'Folder smoke root' $browser.Id
  $invoked = $false
  for ($level = 0; $folderControl -and $level -lt 6; ++$level) {
    $pattern = $null
    if ($folderControl.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
      $pattern.Invoke()
      $invoked = $true
      break
    }
    $folderControl = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($folderControl)
  }
  if (-not $invoked) { throw 'The folder row did not expose native invocation.' }
  if ($browser.WaitForExit($Milliseconds)) { throw 'Browser exited during folder verification.' }
  $browser.Refresh()
  $responding = $browser.Responding -and $browser.MainWindowHandle -ne [IntPtr]::Zero
  if (-not $browser.CloseMainWindow() -or -not $browser.WaitForExit(20000)) {
    throw 'Folder verification browser did not close normally.'
  }
  $saved = Get-Content -LiteralPath $preferences -Raw | ConvertFrom-Json
  $items = @($saved.krypton_ui.folders.items)
  $result = [ordered]@{
    profile = $folderProfile; log = $log; responding = $responding
    exit_code = $browser.ExitCode; folder_count = $items.Count
    created_from_native_menu = @($items | Where-Object { $_.id -eq '3' -and $_.name -eq 'Folder' }).Count -eq 1
    nested_folder_retained = @($items | Where-Object { $_.id -eq '2' -and $_.parent -eq '1' -and $_.collapsed }).Count -eq 1
    collapsed_from_native_row = @($items | Where-Object { $_.id -eq '1' -and $_.collapsed }).Count -eq 1
  }
  $result.passed = $responding -and $browser.ExitCode -eq 0 -and
    $result.created_from_native_menu -and $result.nested_folder_retained -and $result.collapsed_from_native_row
  $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $repository "build/folders-$suffix.json") -Encoding utf8
  $result | ConvertTo-Json
  if (-not $result.passed) { throw 'Native folder integration verification failed.' }
} finally {
  if ($browser -and -not $browser.HasExited) { $null = $browser.CloseMainWindow() }
}

