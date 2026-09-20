# OBS - Low Latency RTSP Source

A Windows OBS Studio source plugin built around GStreamer for very low-latency RTSP monitoring and capture.

**Current stable release: v0.5.2**

## What it does

- H.264 and H.265/HEVC RTSP video auto-detection
- RTSP over TCP with low-latency presets
- Hardware-decoder preference with actual decoder reporting
- Automatic software-decoder fallback when hardware startup fails
- Optional RTSP audio: AAC, Opus, G.711 PCMU and PCMA
- Live connection, decoder, FPS, frame, drop and reconnect telemetry
- Configurable signal-loss display: Transparent, Black, Reconnecting..., or No Signal
- Manual reconnect and privacy-safe Copy Diagnostics
- Stalled-stream watchdog for sessions that stay connected but stop delivering video
- Intelligent reconnect backoff that resets as soon as live video returns

The source ID remains `low_latency_rtsp_gstreamer`, so existing OBS source instances are preserved across plugin upgrades.

## v0.5.2 highlights

v0.5.2 adds public GitHub Releases, Windows release packaging, SHA-256 checksums, and an in-plugin update check while retaining the v0.5.1 low-latency and UniFi guidance behavior.

The v0.5.3 development branch extends this into a verified self-update flow: download the exact Windows release asset, verify its published SHA-256 checksum, then use an elevated helper outside OBS to replace the loaded plugin safely and reopen OBS.

See [CHANGELOG.md](CHANGELOG.md) for the full version history.

## Tested environment

The current release has been tested on:

- Windows 11 x64
- OBS Studio 32.2.2
- GStreamer 1.28.7 MSVC x86_64
- H.264 RTSP at 2688x1512 / 30 FPS
- Direct3D 12 hardware decode through GStreamer (`d3d12h264dec`)

## Requirements

To build the plugin on Windows:

- Visual Studio 2022 Build Tools with Desktop development with C++
- CMake
- Official GStreamer MSVC x86_64 Runtime + Development packages
- PowerShell

The build script automatically downloads the official OBS plugin template into the ignored `.bootstrap` directory.

## Build

From PowerShell in the repository root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\Build-Windows.ps1
```

If GStreamer is installed somewhere non-standard:

```powershell
.\scripts\Build-Windows.ps1 -GStreamerRoot "C:\path\to\gstreamer\1.0\msvc_x86_64"
```

A successful build creates:

```text
dist\low-latency-rtsp.dll
```

If the GStreamer development files are missing, `scripts\Enable-GStreamer-Devel.ps1` can launch the official GStreamer development installer.

## Install

For releases that include the Windows installer, the recommended install path is:

```text
Low-Latency-RTSP-Setup-vX.Y.Z.exe
```

Close OBS completely, run the installer, approve the Windows administrator prompt, and finish setup. The installer supports clean installs and in-place upgrades, registers the plugin in Windows Apps / Installed apps, and provides a normal uninstaller. The installer is currently unsigned, so Windows may show an **Unknown Publisher** warning.

Release builds include a private GStreamer 1.28.7 runtime inside the plugin, so end users do **not** need to install GStreamer separately. The plugin prefers this bundled runtime and falls back to an external GStreamer installation only for development or legacy installs.

The plugin files are installed under:

```text
C:\ProgramData\obs-studio\plugins\low-latency-rtsp
```

The installer metadata and uninstaller are kept separately under Program Files so the in-plugin self-updater can safely replace the plugin directory without breaking Windows uninstall support.

For local development builds, close OBS completely and run:

```powershell
.\scripts\Install-Plugin.ps1
```

Restart OBS and add **Low Latency RTSP** from the Sources menu.

Development installs can still be removed with:

```powershell
.\scripts\Uninstall-Plugin.ps1
```

## Low-latency behavior

The Low Latency preset uses a 0 ms RTSP jitter target and a one-frame appsink queue. Frames are delivered to OBS asynchronously and timestamped at delivery time so old RTP timing does not recreate latency inside OBS.

The plugin intentionally avoids allowing a dead connection to look live. When video disappears, the stale frame is cleared and the configured No Signal Display is shown until fresh frames arrive.

## Diagnostics and privacy

RTSP URLs may contain usernames, passwords, stream IDs, or access tokens. The plugin:

- shows the URL only while configuring a brand-new source, then masks the saved URL on later Properties opens
- does not put the configured RTSP URL into plugin logs or status text
- omits the RTSP URL and credentials from Copy Diagnostics

OBS scene collections can still contain source settings, so treat exported scene collections as potentially sensitive.

Do not post a live RTSP URL in an issue. See [SECURITY.md](SECURITY.md).

## Current scope

The plugin currently focuses on Windows and TCP RTSP streams. H.264 and H.265 video are supported. Common camera audio formats are supported when audio is enabled.

Release packaging, automated updates, the Windows installer, and a private bundled GStreamer runtime are part of the project. Code signing remains optional future work.

## License

This project is licensed under **GPL-2.0-or-later**. See [LICENSE](LICENSE) and [LICENSE-NOTICE.txt](LICENSE-NOTICE.txt).
