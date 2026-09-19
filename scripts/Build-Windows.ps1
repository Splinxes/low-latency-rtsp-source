param(
    [string]$GStreamerRoot = ""
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bootstrapRoot = Join-Path $projectRoot '.bootstrap'
$templateZip = Join-Path $bootstrapRoot 'obs-plugintemplate.zip'
$templateExtract = Join-Path $bootstrapRoot 'template'
$templateRoot = Join-Path $templateExtract 'obs-plugintemplate-master'

function Find-GStreamerRoot {
    param([string]$Explicit)

    $candidates = @()
    if ($Explicit) { $candidates += $Explicit }
    if ($env:GSTREAMER_1_0_ROOT_MSVC_X86_64) { $candidates += $env:GSTREAMER_1_0_ROOT_MSVC_X86_64 }
    $candidates += "$env:LOCALAPPDATA\Programs\gstreamer\1.0\msvc_x86_64"
    $candidates += "$env:ProgramFiles\gstreamer\1.0\msvc_x86_64"

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'bin\gst-launch-1.0.exe'))) {
            return $candidate
        }
    }
    return $null
}

$cmakeCmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
    $cmakeCandidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\CMake\bin\cmake.exe'
    )
    $cmakePath = $cmakeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $cmakePath) {
        throw 'CMake was not found. Install Visual Studio 2022 C++ Build Tools with CMake support and retry.'
    }
    $env:PATH = "$(Split-Path $cmakePath);$env:PATH"
}

$gstRoot = Find-GStreamerRoot -Explicit $GStreamerRoot
if (-not $gstRoot) {
    throw 'GStreamer MSVC x64 was not found. Pass -GStreamerRoot or reinstall the official MSVC x64 package.'
}

$gstHeader = Join-Path $gstRoot 'include\gstreamer-1.0\gst\gst.h'
if (-not (Test-Path $gstHeader)) {
    throw "GStreamer development headers are missing at $gstHeader. Run scripts\Enable-GStreamer-Devel.ps1 first."
}

$pkgConfig = Join-Path $gstRoot 'bin\pkg-config.exe'
if (-not (Test-Path $pkgConfig)) {
    throw "pkg-config.exe was not found in $gstRoot\bin. Re-run the GStreamer installer with Development selected."
}

New-Item -ItemType Directory -Path $bootstrapRoot -Force | Out-Null

if (-not (Test-Path $templateRoot)) {
    Write-Host 'Downloading the official OBS plugin template...' -ForegroundColor Cyan
    Invoke-WebRequest -Uri 'https://github.com/obsproject/obs-plugintemplate/archive/refs/heads/master.zip' -OutFile $templateZip
    if (Test-Path $templateExtract) { Remove-Item $templateExtract -Recurse -Force }
    Expand-Archive -Path $templateZip -DestinationPath $templateExtract -Force
}

Write-Host 'Applying Low Latency RTSP source files...' -ForegroundColor Cyan
if (Test-Path (Join-Path $templateRoot 'src')) { Remove-Item (Join-Path $templateRoot 'src') -Recurse -Force }
if (Test-Path (Join-Path $templateRoot 'data')) { Remove-Item (Join-Path $templateRoot 'data') -Recurse -Force }
Copy-Item (Join-Path $projectRoot 'src') (Join-Path $templateRoot 'src') -Recurse -Force
Copy-Item (Join-Path $projectRoot 'data') (Join-Path $templateRoot 'data') -Recurse -Force
Copy-Item (Join-Path $projectRoot 'CMakeLists.txt') (Join-Path $templateRoot 'CMakeLists.txt') -Force
Copy-Item (Join-Path $projectRoot 'buildspec.json') (Join-Path $templateRoot 'buildspec.json') -Force

$env:PATH = "$gstRoot\bin;$env:PATH"
$env:PKG_CONFIG_PATH = "$gstRoot\lib\pkgconfig"
$env:GSTREAMER_1_0_ROOT_MSVC_X86_64 = $gstRoot

Push-Location $templateRoot
try {
    Write-Host 'Configuring for Visual Studio 2022 x64...' -ForegroundColor Cyan
    & cmake.exe -S . -B build_x64 -G 'Visual Studio 17 2022' -A x64 -DENABLE_QT=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

    Write-Host 'Building RelWithDebInfo...' -ForegroundColor Cyan
    & cmake.exe --build build_x64 --config RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$dll = Get-ChildItem -Path (Join-Path $templateRoot 'build_x64') -Filter 'low-latency-rtsp.dll' -Recurse -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1

if (-not $dll) {
    throw 'Build completed but low-latency-rtsp.dll could not be located.'
}

$outDir = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Copy-Item $dll.FullName (Join-Path $outDir 'low-latency-rtsp.dll') -Force

$localeOut = Join-Path $outDir 'data\locale'
New-Item -ItemType Directory -Path $localeOut -Force | Out-Null
Copy-Item (Join-Path $projectRoot 'data\locale\en-US.ini') (Join-Path $localeOut 'en-US.ini') -Force

Write-Host ''
Write-Host 'BUILD SUCCESS' -ForegroundColor Green
Write-Host "DLL: $outDir\low-latency-rtsp.dll"
Write-Host 'Next: run scripts\Install-Plugin.ps1'
