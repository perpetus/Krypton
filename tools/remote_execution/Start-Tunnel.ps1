param(
  [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.:-]+$')][string]$RemoteHost,
  [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.\\-]+$')][string]$RemoteUser,
  [int]$LocalPort = 18980
)
$ErrorActionPreference = 'Stop'
& "$env:WINDIR\System32\OpenSSH\ssh.exe" -N -T `
  -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=yes `
  -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=3 `
  -L "127.0.0.1:${LocalPort}:127.0.0.1:8980" `
  -L '127.0.0.1:18901:127.0.0.1:9001' `
  -L '127.0.0.1:18902:127.0.0.1:9002' `
  "${RemoteUser}@${RemoteHost}"
exit $LASTEXITCODE
