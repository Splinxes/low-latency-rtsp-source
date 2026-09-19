$ErrorActionPreference = 'Stop'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw 'OBS is currently running. Close OBS completely before uninstalling the plugin.'
}

$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\low-latency-rtsp'
$legacyRoot = Join-Path $env:APPDATA 'obs-studio\plugins\low-latency-rtsp'
$removed = $false

foreach ($path in @($pluginRoot, $legacyRoot)) {
    if (Test-Path $path) {
        Remove-Item $path -Recurse -Force
        Write-Host "Removed: $path" -ForegroundColor Green
        $removed = $true
    }
}

if (-not $removed) {
    Write-Host 'Low Latency RTSP plugin is not installed.' -ForegroundColor Yellow
}
