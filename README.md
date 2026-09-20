<p align="center">
  <a href="https://github.com/Splinxes/low-latency-rtsp-source/releases/latest/download/Low-Latency-RTSP-Setup.exe">
    <img src="https://img.shields.io/badge/Download%20for%20Windows-.EXE-0078D4?style=for-the-badge&logo=windows11&logoColor=white" alt="Download for Windows">
  </a>
</p>

<p align="center">
  <a href="https://github.com/Splinxes/low-latency-rtsp-source/releases/latest">Latest Release</a>
  ·
  <a href="CHANGELOG.md">Changelog</a>
  ·
  <a href="SECURITY.md">Security</a>
</p>

# Low Latency RTSP

An independent third-party RTSP source plugin for OBS Studio, built around GStreamer for very low-latency monitoring and capture.

This project is not affiliated with, endorsed by, or developed by the OBS Project.

**Current stable release: v0.6.2**

## Quick install

1. Download **Low-Latency-RTSP-Setup.exe** using the button above.
2. Close OBS Studio completely.
3. Run the installer and approve the Windows administrator prompt.
4. Reopen OBS.
5. Choose **Sources → + → Low Latency RTSP** and enter your RTSP stream URL.

That is it. **You do not need to install GStreamer separately.** Release builds include a private GStreamer 1.28.7 MSVC x64 runtime inside the plugin.

The installer is currently unsigned, so Windows may show **Unknown Publisher** or a SmartScreen warning. Download releases only from this repository and use the published SHA-256 checksum if you want to verify the file.

## Features

- H.264/AVC, H.265/HEVC, Motion JPEG, and MPEG-4 Part 2 RTSP video auto-detection
- RTSP over TCP with Low Latency, Balanced, Stable, and Custom tuning
- Hardware-decoder preference with the actual selected decoder reported in telemetry
- Automatic software-decoder fallback if hardware startup fails
- Optional RTSP audio with AAC, Opus, G.711 PCMU/PCMA, and G.726 support
- Live connection state, decoder, codec, FPS, frame, dropped-frame, uptime, audio, and reconnect telemetry
- Configurable signal-loss display: Transparent, Black, Reconnecting..., or No Signal
- Manual **Reconnect Now**
- Stalled-stream watchdog for sessions that remain connected but stop delivering video
- Intelligent reconnect backoff that resets as soon as live video returns
- Privacy-safe **Copy Diagnostics**
- Built-in GitHub release update checker and verified Windows self-updater
- Normal Windows installer and uninstall entry
- Private bundled GStreamer runtime with external-runtime fallback for development and legacy installs

The OBS source ID remains `low_latency_rtsp_gstreamer`, so existing source instances are preserved across plugin upgrades.

## Supported RTSP media

| Media | RTP / codec | Status |
| --- | --- | --- |
| Video | H.264 / AVC | Supported |
| Video | H.265 / HEVC | Supported |
| Video | Motion JPEG (`JPEG`, RFC 2435) | Supported |
| Video | MPEG-4 Part 2 (`MP4V-ES`) | Supported |
| Audio | AAC / MPEG4-GENERIC | Supported |
| Audio | AAC-LATM / MP4A-LATM | Supported |
| Audio | Opus | Supported |
| Audio | G.711 μ-law / PCMU | Supported |
| Audio | G.711 A-law / PCMA | Supported |
| Audio | G.726 16/24/32/40 kbps and AAL2 variants | Supported |

H.264/H.265 and the installer path have been exercised on real hardware. The v0.6.1 compatibility additions are also gated by CI checks against the exact bundled GStreamer runtime so a release fails if the required codec elements are missing.

## What's new in v0.6.2

v0.6.2 cleans up third-party branding and makes the updater resilient to the repository rename.

- Product branding is now simply **Low Latency RTSP**
- Removed the previous OBS-like project artwork and OBS-prefixed display names
- The installer and Windows Installed Apps entry now use **Low Latency RTSP**
- The updater now accepts release assets from both the current `low-latency-rtsp-source` repository path and the former `obs-low-latency-rtsp-source` path
- Fixed the updater's safety-limit message to correctly report the existing 512 MB limit
- Preserved the v0.6.1 update progress window and expanded RTSP codec compatibility

See [CHANGELOG.md](CHANGELOG.md) for the full version history.

## Low-latency behavior

The **Low Latency** preset uses a 0 ms RTSP jitter target and a one-frame appsink queue. Frames are delivered to OBS asynchronously and timestamped at delivery time so stale RTP timing does not recreate latency inside OBS.

The plugin also disables normal async source buffering for this source so OBS favors the newest delivered frame.

If video disappears, the plugin clears the stale live frame and switches to the configured **No Signal Display** until fresh video returns.

## Updates

The source Properties window includes **Update Plugin**.

When a newer GitHub Release is available, OBS first shows download/verification progress. After you choose **Install & Restart OBS** and approve UAC, a dedicated installation progress window remains visible while the plugin is replaced and OBS is restarted.

The Windows updater:

- requests the exact versioned Windows ZIP and its published SHA-256 file
- only accepts release assets from this repository
- verifies the package hash before touching installed files
- asks OBS to close normally and never force-kills it
- backs up the existing plugin before replacement
- restores the backup if replacement fails
- reopens OBS after a successful update
- keeps its elevated PowerShell host hidden while showing a visible installation progress bar and the normal Windows UAC prompt
- enforces a 512 MB update-download safety limit

The self-updater replaces the plugin directory, including its private GStreamer runtime.

> **Repository rename note:** builds from v0.6.0 were created before the repository was renamed and reject the renamed GitHub asset URLs during their security check. If you are still on v0.6.0, install v0.6.2 manually once using the EXE. v0.6.1 and later use the current repository path, and v0.6.2 additionally trusts both the current and former paths for future resilience.

## Install location and uninstall

The installed plugin lives under:

```text
C:\ProgramData\obs-studio\plugins\low-latency-rtsp
```

The bundled GStreamer runtime is stored privately under that plugin directory. It does not require or replace a machine-wide GStreamer installation.

The installer registers **Low Latency RTSP** in Windows **Settings → Apps → Installed apps**. Uninstalling removes the plugin and its private bundled GStreamer runtime. A separately installed system-wide GStreamer copy is left alone.

Installer metadata and the uninstaller are kept outside the plugin directory so in-plugin updates can safely replace the plugin files without breaking Windows uninstall support.

## UniFi Protect

The plugin includes inline guidance for common UniFi Protect RTSP URL conversions.

When a Protect secure URL is detected, the Properties window can guide you to:

- change `rtsps://` to `rtsp://`
- change port `:7441` to `:7447`
- remove `enableSrtp`

The stream ID/path stays unchanged. The plugin does not automatically rewrite the URL.

## Diagnostics and privacy

RTSP URLs can contain usernames, passwords, stream IDs, or access tokens. The plugin is designed not to expose them unnecessarily:

- a brand-new source shows the URL while you configure it
- saved URLs are masked on later Properties opens
- plugin logs and status text do not include the configured RTSP URL
- **Copy Diagnostics** omits the RTSP URL and credentials

OBS scene collections can still contain source settings, so exported scene collections should be treated as potentially sensitive.

Do not post a live RTSP URL in an issue. See [SECURITY.md](SECURITY.md).

## Tested environment

The current release line has been tested with:

- Windows 11 x64
- OBS Studio 32.2.2
- bundled GStreamer 1.28.7 MSVC x86_64
- H.264 RTSP at 2688×1512 / 30 FPS
- Direct3D 12 hardware decoding through GStreamer (`d3d12h264dec`)
- a clean Windows PC with only OBS installed before running the all-in-one installer

Other RTSP cameras, codecs, decoders, Windows versions, and OBS versions may work, but the list above is the currently validated environment.

## Release downloads

For normal Windows installation, use:

```text
Low-Latency-RTSP-Setup.exe
```

That stable asset name always points to the installer from the latest GitHub Release.

Each release also includes versioned assets:

```text
Low-Latency-RTSP-Setup-vX.Y.Z.exe
Low-Latency-RTSP-Setup-vX.Y.Z.exe.sha256
low-latency-rtsp-vX.Y.Z-windows-x64.zip
low-latency-rtsp-vX.Y.Z-windows-x64.zip.sha256
```

The ZIP package is primarily used by the self-updater and manual release installation. Most users should use the EXE.

## Build from source

Building locally is different from installing a release. A Windows development environment needs:

- Visual Studio 2022 Build Tools with **Desktop development with C++**
- CMake
- official GStreamer MSVC x86_64 Runtime + Development packages
- PowerShell

The build script automatically downloads the pinned OBS plugin template/dependencies into the ignored `.bootstrap` directory.

From PowerShell in the repository root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\Build-Windows.ps1
```

For a non-standard GStreamer location:

```powershell
.\scripts\Build-Windows.ps1 -GStreamerRoot "C:\path\to\gstreamer\1.0\msvc_x86_64"
```

A successful build creates:

```text
dist\low-latency-rtsp.dll
```

For local development installation, close OBS and run:

```powershell
.\scripts\Install-Plugin.ps1
```

Development installs can be removed with:

```powershell
.\scripts\Uninstall-Plugin.ps1
```

## Current scope

The project currently focuses on Windows x64 and RTSP over TCP. Supported video includes H.264, H.265/HEVC, Motion JPEG, and legacy MPEG-4 Part 2. Optional audio includes AAC, Opus, G.711, and G.726.

The Windows installer, private GStreamer runtime, GitHub release packaging, SHA-256 checksums, and verified self-updater are all part of the current release. Code signing is not currently used.

## License

This project is licensed under **GPL-2.0-or-later**. See [LICENSE](LICENSE).

Windows release packages may also redistribute the official GStreamer runtime and its component licenses/notices. See [LICENSE-NOTICE.txt](LICENSE-NOTICE.txt).
