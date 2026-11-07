param(
  [string]$ProfileDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$browser = Join-Path $repository 'build/src/out/Default/chrome.exe'
$module = Join-Path $repository 'build/src/out/Default/krypton_ui.dll'
if (-not (Test-Path -LiteralPath $browser) -or
    -not (Test-Path -LiteralPath $module)) {
  throw 'Build the Release browser and stage KryptonUI before verification.'
}
if (-not $ProfileDirectory) {
  $ProfileDirectory = Join-Path $repository 'build/profiles/multi-page-verification'
}
$ProfileDirectory = [IO.Path]::GetFullPath($ProfileDirectory)
New-Item -ItemType Directory -Path $ProfileDirectory -Force | Out-Null
# Use the normal feature defaults, with a separate profile for verification.
Start-Process -FilePath $browser -ArgumentList @(
  "--user-data-dir=`"$ProfileDirectory`"",
  '--no-first-run'
) | Out-Null
