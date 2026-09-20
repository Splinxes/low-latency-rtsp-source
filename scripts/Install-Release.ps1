param()

$ErrorActionPreference = 'Stop'

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $packageRoot 'low-latency-rtsp'
$dllSource = Join-Path $sourceRoot 'bin\64bit\low-latency-rtsp.dll'
$localeSource = Join-Path $sourceRoot 'data\locale\en-US.ini'

if (-not (Test-Path $dllSource)) {
    throw "Release package is missing $dllSource"
}
if (-not (Test-Path $localeSource)) {
    throw "Release package is missing $localeSource"
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

New-Item -ItemType Directory -Path $binRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localeRoot -Force | Out-Null

$existingDll = Join-Path $binRoot 'low-latency-rtsp.dll'
if (Test-Path $existingDll) {
    $backupRoot = Join-Path $env:TEMP 'low-latency-rtsp-backup'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    Copy-Item $existingDll (Join-Path $backupRoot 'low-latency-rtsp.dll') -Force
}

Copy-Item $dllSource $existingDll -Force
Copy-Item $localeSource (Join-Path $localeRoot 'en-US.ini') -Force

Write-Host ''
Write-Host 'Low Latency RTSP installed successfully.' -ForegroundColor Green
Write-Host "Plugin: $pluginRoot"
Write-Host 'You can now reopen OBS Studio.'
Read-Host 'Press Enter to close'
