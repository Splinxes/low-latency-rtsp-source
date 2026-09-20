param(
    [Parameter(Mandatory = $true)]
    [string]$PackageZip,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedHash,

    [Parameter(Mandatory = $true)]
    [int]$ObsPid,

    [Parameter(Mandatory = $true)]
    [string]$ObsExe
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework

function Show-UpdateMessage {
    param(
        [string]$Message,
        [string]$Title = 'Low Latency RTSP Update',
        [string]$Icon = 'Information'
    )

    [System.Windows.MessageBox]::Show(
        $Message,
        $Title,
        'OK',
        $Icon
    ) | Out-Null
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )
}

$obsWasRunning = $false
$extractRoot = Join-Path $env:TEMP "low-latency-rtsp-extract-$PID"
$backupRoot = Join-Path $env:TEMP "low-latency-rtsp-backup-$PID"
$stagingRoot = Split-Path -Parent $PackageZip
$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\low-latency-rtsp'

try {
    if (-not (Test-IsAdministrator)) {
        throw 'The updater was not started with administrator permission.'
    }

    if (-not (Test-Path -LiteralPath $PackageZip)) {
        throw 'The downloaded update package could not be found.'
    }

    $expected = $ExpectedHash.Trim().ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') {
        throw 'The expected SHA-256 checksum is invalid.'
    }

    $actual = (Get-FileHash -LiteralPath $PackageZip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw 'The update package failed SHA-256 verification. No files were changed.'
    }

    $obs = Get-Process -Id $ObsPid -ErrorAction SilentlyContinue
    if ($obs) {
        $obsWasRunning = $true
        [void]$obs.CloseMainWindow()

        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Process -Id $ObsPid -ErrorAction SilentlyContinue) -and
               (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 250
        }

        if (Get-Process -Id $ObsPid -ErrorAction SilentlyContinue) {
            Show-UpdateMessage -Message (
                'OBS Studio is still open. Close OBS completely, then click OK. ' +
                'The update will continue after OBS exits.'
            )

            $deadline = (Get-Date).AddSeconds(60)
            while ((Get-Process -Id $ObsPid -ErrorAction SilentlyContinue) -and
                   (Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 250
            }
        }

        if (Get-Process -Id $ObsPid -ErrorAction SilentlyContinue) {
            throw 'OBS Studio did not close, so the update was cancelled. No plugin files were changed.'
        }
    }

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $PackageZip -DestinationPath $extractRoot -Force

    $sourceRoot = Join-Path $extractRoot 'low-latency-rtsp'
    $sourceDll = Join-Path $sourceRoot 'bin\64bit\low-latency-rtsp.dll'
    $sourceLocale = Join-Path $sourceRoot 'data\locale\en-US.ini'
    $sourceUpdater = Join-Path $sourceRoot 'data\updater\Install-Update.ps1'

    if (-not (Test-Path -LiteralPath $sourceDll)) {
        throw 'The update package does not contain the plugin DLL.'
    }
    if (-not (Test-Path -LiteralPath $sourceLocale)) {
        throw 'The update package does not contain the locale data.'
    }
    if (-not (Test-Path -LiteralPath $sourceUpdater)) {
        throw 'The update package does not contain the updater helper.'
    }

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }

    if (Test-Path -LiteralPath $pluginRoot) {
        Copy-Item -LiteralPath $pluginRoot -Destination $backupRoot -Recurse -Force
    }

    try {
        if (Test-Path -LiteralPath $pluginRoot) {
            Remove-Item -LiteralPath $pluginRoot -Recurse -Force
        }

        Copy-Item -LiteralPath $sourceRoot -Destination $pluginRoot -Recurse -Force
    }
    catch {
        if (Test-Path -LiteralPath $pluginRoot) {
            Remove-Item -LiteralPath $pluginRoot -Recurse -Force -ErrorAction SilentlyContinue
        }

        if (Test-Path -LiteralPath $backupRoot) {
            Copy-Item -LiteralPath $backupRoot -Destination $pluginRoot -Recurse -Force
        }

        throw
    }

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($ObsExe -and (Test-Path -LiteralPath $ObsExe)) {
        Start-Process -FilePath $ObsExe
    }

    # Best-effort cleanup. The currently executing script is staged here, so
    # Windows may defer part of this removal until PowerShell exits.
    if ($stagingRoot -and
        $stagingRoot.StartsWith($env:TEMP, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    exit 0
}
catch {
    $message = $_.Exception.Message
    Show-UpdateMessage -Message (
        "The Low Latency RTSP update could not be installed.`n`n$message"
    ) -Icon 'Error'

    if ($obsWasRunning -and
        $ObsExe -and
        (Test-Path -LiteralPath $ObsExe) -and
        -not (Get-Process obs64 -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath $ObsExe -ErrorAction SilentlyContinue
    }

    exit 1
}
