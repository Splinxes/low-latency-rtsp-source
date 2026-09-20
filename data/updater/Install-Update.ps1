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

function New-UpdateProgressWindow {
    $window = New-Object System.Windows.Window
    $window.Title = 'Low Latency RTSP Update'
    $window.Width = 460
    $window.Height = 185
    $window.ResizeMode = [System.Windows.ResizeMode]::NoResize
    $window.WindowStartupLocation = [System.Windows.WindowStartupLocation]::CenterScreen
    $window.ShowInTaskbar = $true
    $window.Topmost = $true

    $panel = New-Object System.Windows.Controls.StackPanel
    $panel.Margin = New-Object System.Windows.Thickness(22)

    $title = New-Object System.Windows.Controls.TextBlock
    $title.Text = 'Installing Low Latency RTSP'
    $title.FontSize = 18
    $title.FontWeight = [System.Windows.FontWeights]::SemiBold
    $title.Margin = New-Object System.Windows.Thickness(0, 0, 0, 12)

    $status = New-Object System.Windows.Controls.TextBlock
    $status.Text = 'Preparing update...'
    $status.FontSize = 13
    $status.Margin = New-Object System.Windows.Thickness(0, 0, 0, 10)

    $bar = New-Object System.Windows.Controls.ProgressBar
    $bar.Minimum = 0
    $bar.Maximum = 100
    $bar.Value = 2
    $bar.Height = 20
    $bar.Margin = New-Object System.Windows.Thickness(0, 0, 0, 10)

    $detail = New-Object System.Windows.Controls.TextBlock
    $detail.Text = 'OBS Studio will close and reopen automatically.'
    $detail.FontSize = 11
    $detail.Opacity = 0.72

    [void]$panel.Children.Add($title)
    [void]$panel.Children.Add($status)
    [void]$panel.Children.Add($bar)
    [void]$panel.Children.Add($detail)
    $window.Content = $panel

    $window.Show()
    [void]$window.Dispatcher.Invoke(
        [System.Action]{},
        [System.Windows.Threading.DispatcherPriority]::Render
    )

    return [pscustomobject]@{
        Window = $window
        Status = $status
        Bar = $bar
    }
}

function Set-UpdateProgress {
    param(
        [object]$Ui,
        [int]$Percent,
        [string]$Status
    )

    if (-not $Ui) {
        return
    }

    $Ui.Bar.Value = [Math]::Max(0, [Math]::Min(100, $Percent))
    $Ui.Status.Text = $Status
    [void]$Ui.Window.Dispatcher.Invoke(
        [System.Action]{},
        [System.Windows.Threading.DispatcherPriority]::Render
    )
}

function Close-UpdateProgress {
    param([object]$Ui)

    if (-not $Ui) {
        return
    }

    try {
        $Ui.Window.Close()
    }
    catch {
        # The progress window is best-effort; never mask the update result.
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )
}

$obsWasRunning = $false
$progressUi = $null
$extractRoot = Join-Path $env:TEMP "low-latency-rtsp-extract-$PID"
$backupRoot = Join-Path $env:TEMP "low-latency-rtsp-backup-$PID"
$stagingRoot = Split-Path -Parent $PackageZip
$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\low-latency-rtsp'

try {
    if (-not (Test-IsAdministrator)) {
        throw 'The updater was not started with administrator permission.'
    }

    $progressUi = New-UpdateProgressWindow
    Set-UpdateProgress -Ui $progressUi -Percent 5 -Status 'Verifying downloaded package...'

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

    Set-UpdateProgress -Ui $progressUi -Percent 15 -Status 'Closing OBS Studio safely...'

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
            Set-UpdateProgress -Ui $progressUi -Percent 18 -Status 'Waiting for OBS Studio to close...'
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

    Set-UpdateProgress -Ui $progressUi -Percent 30 -Status 'Extracting update files...'

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $PackageZip -DestinationPath $extractRoot -Force

    Set-UpdateProgress -Ui $progressUi -Percent 45 -Status 'Validating plugin package...'

    $sourceRoot = Join-Path $extractRoot 'low-latency-rtsp'
    $sourceDll = Join-Path $sourceRoot 'bin\64bit\low-latency-rtsp.dll'
    $sourceLocale = Join-Path $sourceRoot 'data\locale\en-US.ini'
    $sourceUpdater = Join-Path $sourceRoot 'data\updater\Install-Update.ps1'
    $sourceRuntime = Join-Path $sourceRoot 'runtime\bin\gstreamer-1.0-0.dll'

    if (-not (Test-Path -LiteralPath $sourceDll)) {
        throw 'The update package does not contain the plugin DLL.'
    }
    if (-not (Test-Path -LiteralPath $sourceLocale)) {
        throw 'The update package does not contain the locale data.'
    }
    if (-not (Test-Path -LiteralPath $sourceUpdater)) {
        throw 'The update package does not contain the updater helper.'
    }
    if (-not (Test-Path -LiteralPath $sourceRuntime)) {
        throw 'The update package does not contain the bundled GStreamer runtime.'
    }

    Set-UpdateProgress -Ui $progressUi -Percent 55 -Status 'Backing up current plugin...'

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }

    if (Test-Path -LiteralPath $pluginRoot) {
        Copy-Item -LiteralPath $pluginRoot -Destination $backupRoot -Recurse -Force
    }

    Set-UpdateProgress -Ui $progressUi -Percent 70 -Status 'Installing new plugin files...'

    try {
        if (Test-Path -LiteralPath $pluginRoot) {
            Remove-Item -LiteralPath $pluginRoot -Recurse -Force
        }

        Copy-Item -LiteralPath $sourceRoot -Destination $pluginRoot -Recurse -Force
    }
    catch {
        Set-UpdateProgress -Ui $progressUi -Percent 75 -Status 'Restoring previous plugin...'

        if (Test-Path -LiteralPath $pluginRoot) {
            Remove-Item -LiteralPath $pluginRoot -Recurse -Force -ErrorAction SilentlyContinue
        }

        if (Test-Path -LiteralPath $backupRoot) {
            Copy-Item -LiteralPath $backupRoot -Destination $pluginRoot -Recurse -Force
        }

        throw
    }

    Set-UpdateProgress -Ui $progressUi -Percent 90 -Status 'Cleaning up temporary files...'

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    # Best-effort cleanup. The currently executing script is staged here, so
    # Windows may defer part of this removal until PowerShell exits.
    if ($stagingRoot -and
        $stagingRoot.StartsWith($env:TEMP, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    Set-UpdateProgress -Ui $progressUi -Percent 100 -Status 'Update complete. Reopening OBS Studio...'
    Start-Sleep -Milliseconds 450
    Close-UpdateProgress -Ui $progressUi
    $progressUi = $null

    if ($ObsExe -and (Test-Path -LiteralPath $ObsExe)) {
        Start-Process -FilePath $ObsExe
    }

    exit 0
}
catch {
    $message = $_.Exception.Message
    Close-UpdateProgress -Ui $progressUi
    $progressUi = $null

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
