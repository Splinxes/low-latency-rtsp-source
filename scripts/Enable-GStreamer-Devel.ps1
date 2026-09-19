$ErrorActionPreference = 'Stop'

$version = '1.28.7'
$url = "https://gstreamer.freedesktop.org/data/pkg/windows/$version/msvc/gstreamer-1.0-msvc-x86_64-$version.exe"
$installer = Join-Path $env:TEMP "gstreamer-1.0-msvc-x86_64-$version.exe"

Write-Host "Downloading the official GStreamer $version MSVC x64 installer..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $url -OutFile $installer

Write-Host "Opening GStreamer installer in Development mode." -ForegroundColor Cyan
Write-Host "This adds headers/import libraries to the runtime you already installed." -ForegroundColor Gray
Start-Process -FilePath $installer -ArgumentList '/TYPE=devel' -Wait

Write-Host "Done. Close and reopen PowerShell/Visual Studio before building." -ForegroundColor Green
