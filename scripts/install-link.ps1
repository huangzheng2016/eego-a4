# Install eego-link (Windows).
#   irm https://raw.githubusercontent.com/huangzheng2016/eego-a4/main/scripts/install-link.ps1 | iex
# Start at logon as well:
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/huangzheng2016/eego-a4/main/scripts/install-link.ps1))) -Service
param([switch]$Service)
$ErrorActionPreference = 'Stop'

$base = 'https://github.com/huangzheng2016/eego-a4/releases/download/link'
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
$asset = "eego-link-windows-$arch.exe"
$dir = Join-Path $env:LOCALAPPDATA 'eego-link'
$exe = Join-Path $dir 'eego-link.exe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null

Write-Host "downloading $asset"
$tmp = Join-Path $env:TEMP $asset
Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $tmp
$sums = (Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS").Content
if ($sums -is [byte[]]) { $sums = [Text.Encoding]::UTF8.GetString($sums) }
$line = $sums -split "`n" | Where-Object { $_.Trim().EndsWith(" $asset") } | Select-Object -First 1
$want = if ($line) { ($line.Trim() -split '\s+')[0] } else { '' }
$got = (Get-FileHash -Algorithm SHA256 $tmp).Hash.ToLower()
if (-not $want -or $want -ne $got) { throw "checksum mismatch for $asset" }
Move-Item -Force $tmp $exe
Write-Host "installed $exe"

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $dir) {
  [Environment]::SetEnvironmentVariable('Path', "$userPath;$dir", 'User')
  Write-Host "added $dir to PATH (open a new terminal)"
}

if ($Service) {
  $action = New-ScheduledTaskAction -Execute $exe -Argument 'run'
  $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
  $settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
  Register-ScheduledTask -TaskName 'eego-link' -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
  Start-ScheduledTask -TaskName 'eego-link'
  Write-Host 'scheduled task "eego-link" starts at logon'
}

Write-Host 'next: pair once in Settings > Bluetooth & devices > Add device (enter the code shown on the device), then run: eego-link pair'
