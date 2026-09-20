param()

$ErrorActionPreference = 'Stop'

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $packageRoot 'low-latency-rtsp'
$dllSource = Join-Path $sourceRoot 'bin\64bit\low-latency-rtsp.dll'
$localeSource = Join-Path $sourceRoot 'data\locale\en-US.ini'
$updaterSource = Join-Path $sourceRoot 'data\updater\Install-Update.ps1'
$runtimeSource = Join-Path $sourceRoot 'runtime'

if (-not (Test-Path $dllSource)) {
    throw "Release package is missing $dllSource"
}
if (-not (Test-Path $localeSource)) {
    throw "Release package is missing $localeSource"
}
if (-not (Test-Path $updaterSource)) {
    throw "Release package is missing $updaterSource"
}
if (-not (Test-Path (Join-Path $runtimeSource 'bin\gstreamer-1.0-0.dll'))) {
    throw "Release package is missing the bundled GStreamer runtime."
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    $quotedScript = '"' + $PSCommandPath + '"'
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $quotedScript
    )
    exit
}

$obs = Get-Process obs64 -ErrorAction SilentlyContinue
if ($obs) {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        'Close OBS Studio completely, then run Install-Release.ps1 again.',
        'Low Latency RTSP Update',
        'OK',
        'Information'
    ) | Out-Null
    exit 1
}

$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\low-latency-rtsp'
$binRoot = Join-Path $pluginRoot 'bin\64bit'
$localeRoot = Join-Path $pluginRoot 'data\locale'
$updaterRoot = Join-Path $pluginRoot 'data\updater'
$runtimeRoot = Join-Path $pluginRoot 'runtime'

New-Item -ItemType Directory -Path $binRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $updaterRoot -Force | Out-Null

$existingDll = Join-Path $binRoot 'low-latency-rtsp.dll'
if (Test-Path $existingDll) {
    $backupRoot = Join-Path $env:TEMP 'low-latency-rtsp-backup'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    Copy-Item $existingDll (Join-Path $backupRoot 'low-latency-rtsp.dll') -Force
}

Copy-Item $dllSource $existingDll -Force
Copy-Item $localeSource (Join-Path $localeRoot 'en-US.ini') -Force
Copy-Item $updaterSource (Join-Path $updaterRoot 'Install-Update.ps1') -Force
if (Test-Path $runtimeRoot) {
    Remove-Item $runtimeRoot -Recurse -Force
}
Copy-Item $runtimeSource $pluginRoot -Recurse -Force

Write-Host ''
Write-Host 'Low Latency RTSP installed successfully.' -ForegroundColor Green
Write-Host "Plugin: $pluginRoot"
Write-Host 'You can now reopen OBS Studio.'
Read-Host 'Press Enter to close'
