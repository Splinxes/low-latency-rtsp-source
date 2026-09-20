$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll = Join-Path $projectRoot 'dist\low-latency-rtsp.dll'
$locale = Join-Path $projectRoot 'dist\data\locale\en-US.ini'
$updater = Join-Path $projectRoot 'data\updater\Install-Update.ps1'

if (-not (Test-Path $dll)) {
    throw 'dist\low-latency-rtsp.dll does not exist. Build the plugin first.'
}
if (-not (Test-Path $updater)) {
    throw 'data\updater\Install-Update.ps1 does not exist.'
}

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw 'OBS is currently running. Close OBS completely before installing the plugin.'
}

$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\low-latency-rtsp'
$binDir = Join-Path $pluginRoot 'bin\64bit'
$dataDir = Join-Path $pluginRoot 'data\locale'
$updaterDir = Join-Path $pluginRoot 'data\updater'
$installedDll = Join-Path $binDir 'low-latency-rtsp.dll'

New-Item -ItemType Directory -Path $binDir -Force | Out-Null
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
New-Item -ItemType Directory -Path $updaterDir -Force | Out-Null

if (Test-Path $installedDll) {
    $backupDir = Join-Path $projectRoot 'dist\backup'
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    Copy-Item $installedDll (Join-Path $backupDir "low-latency-rtsp-$stamp.dll") -Force
}

Copy-Item $dll $installedDll -Force
if (Test-Path $locale) {
    Copy-Item $locale (Join-Path $dataDir 'en-US.ini') -Force
}
Copy-Item $updater (Join-Path $updaterDir 'Install-Update.ps1') -Force

# Remove the incorrect per-user location used by v0.1.0 if present.
$oldRoot = Join-Path $env:APPDATA 'obs-studio\plugins\low-latency-rtsp'
if (Test-Path $oldRoot) {
    Remove-Item $oldRoot -Recurse -Force
}

Write-Host 'Plugin installed.' -ForegroundColor Green
Write-Host "Path: $pluginRoot"
Write-Host 'Restart OBS, then Sources -> + -> Low Latency RTSP.'
