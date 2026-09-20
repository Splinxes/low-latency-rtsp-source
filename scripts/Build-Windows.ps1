param(
    [string]$GStreamerRoot = ""
)

$ErrorActionPreference = 'Stop'
$ObsSdkVersion = '32.2.2'
$ObsSourceArchiveSha256 = '35d3cd0979d65664fada7119fdb612eca7c34b61a1623a330caec74bf72626c4'
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

# The upstream plugin template currently uses VERSION.zip for OBS sources on
# Windows. OBS 32.2.2 is pinned here to the tag tarball instead so Windows and
# macOS use the same verified source archive/hash.
$windowsBuildspec = Join-Path $templateRoot 'cmake\windows\buildspec.cmake'
$windowsBuildspecText = Get-Content $windowsBuildspec -Raw
$windowsBuildspecText = $windowsBuildspecText.Replace(
    'set(obs-studio_filename "VERSION.zip")',
    'set(obs-studio_filename "VERSION.tar.gz")'
)
if (-not $windowsBuildspecText.Contains('set(obs-studio_filename "VERSION.tar.gz")')) {
    throw 'Could not configure the OBS plugin template to use the OBS source tarball.'
}
Set-Content -Path $windowsBuildspec -Value $windowsBuildspecText -Encoding UTF8

$projectBuildspec = Get-Content (Join-Path $projectRoot 'buildspec.json') -Raw | ConvertFrom-Json
if ($projectBuildspec.dependencies.'obs-studio'.version -ne $ObsSdkVersion) {
    throw "buildspec.json OBS SDK version does not match expected target $ObsSdkVersion."
}
if ($projectBuildspec.dependencies.'obs-studio'.hashes.'windows-x64' -ne $ObsSourceArchiveSha256) {
    throw 'buildspec.json OBS source archive hash does not match the pinned OBS 32.2.2 source archive.'
}

# Never reuse an older libobs/Qt dependency tree after changing SDK targets.
$sdkStamp = Join-Path $bootstrapRoot 'obs-sdk-target.txt'
$previousSdk = ''
if (Test-Path $sdkStamp) {
    $previousSdk = (Get-Content $sdkStamp -Raw).Trim()
}
if ($previousSdk -ne $ObsSdkVersion) {
    Write-Host "Preparing clean OBS SDK $ObsSdkVersion dependency tree..." -ForegroundColor Cyan
    $depsDir = Join-Path $templateRoot '.deps'
    $buildDir = Join-Path $templateRoot 'build_x64'
    if (Test-Path $depsDir) { Remove-Item $depsDir -Recurse -Force }
    if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
    Set-Content -Path $sdkStamp -Value $ObsSdkVersion -Encoding ASCII
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
    Write-Host "Configuring against OBS Studio SDK $ObsSdkVersion (Visual Studio 2022 x64)..." -ForegroundColor Cyan
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
Write-Host "OBS SDK target: $ObsSdkVersion"
Write-Host "DLL: $outDir\low-latency-rtsp.dll"
Write-Host 'Next: run scripts\Install-Plugin.ps1'
