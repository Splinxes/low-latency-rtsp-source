# Changelog

## v0.5.1 (development)

- Brand-new sources now show the RTSP URL field in plain text so a pasted URL can be corrected before closing Properties. Once a URL has been saved, future Properties sessions return to the masked password-style field.
- Disabled libobs async video buffering for this low-latency source with `obs_source_set_async_unbuffered(..., true)`. OBS now keeps the newest delivered frame instead of allowing its async source queue to retain older frames, targeting the small first-add latency that disappears only after a full OBS restart.
- Moved `Copy Diagnostics`, `Refresh Stats`, and `Reset Stats` under Advanced Settings to keep the default Properties view cleaner.
- Renamed `GitHub / Updates` to `Update Plugin`; it still opens the project's GitHub Releases page.
- The UniFi guidance now latches once a Protect secure URL is detected, so temporary partial edits (such as deleting a port digit) do not make the warning disappear. It clears only when the URL is fully converted or the field is cleared.
- Keeps the UniFi guidance visible through partial conversions and hides it only after all three fixes are complete: `rtsps://` -> `rtsp://`, `:7441` -> `:7447`, and removal of `enableSrtp`.
- Added a `GitHub / Updates` button to the source Properties window that opens the project's GitHub Releases page in the default browser.
- Added inline UniFi Protect URL guidance when a secure `rtsps://` link using port 7441 or `enableSrtp` is entered in the RTSP URL field.
- The guidance appears inside the existing top information area of the Properties window instead of using a modal popup, so editing the URL is never interrupted.
- The hint explains the standard Protect conversion: `rtsps://` to `rtsp://`, port `7441` to `7447`, and removal of `?enableSrtp`, while keeping the stream ID/path unchanged.
- The inline warning never displays or logs the pasted RTSP URL or credentials.
- The plugin does not rewrite the URL automatically.
- No changes to the streaming, decoder, audio, reconnect, watchdog, or no-signal engine.

## v0.5.0 (development)

- Aligned the Windows build target with the OBS Studio 32.2.2 SDK.
- Updated the pinned OBS dependency and Qt packages to the versions used by OBS Studio 32.2.2.
- Changed the Windows bootstrap to use the verified OBS 32.2.2 tag tarball instead of the plugin template's Windows-only source ZIP convention.
- Added an SDK target stamp so old libobs/Qt build caches are removed automatically when the SDK target changes.
- Build output now prints the OBS SDK target explicitly.
- Updated plugin/diagnostic version reporting to v0.5.0.
- No changes to the v0.4.0 RTSP video, audio, reconnect, watchdog, decoder, or no-signal behavior.

## v0.4.0

- Added an adaptive stalled-video watchdog with a 3-second minimum timeout and a 10-second initial-frame timeout.
- Added automatic reconnect backoff. The default 1000 ms base delay produces 1s, 2s, 5s, and then 10s retries; a successful video frame resets the backoff.
- Added live reconnect countdown text.
- Renamed Reconnect Delay to Reconnect Base Delay and documented the backoff behavior.
- Added privacy-safe Copy Diagnostics output with plugin, OBS, GStreamer, codec/decoder, audio, preset, telemetry, watchdog, and reconnect information. RTSP URLs and credentials are never copied.
- Added a version line under Advanced Settings.
- Simplified timeout/refusal/network errors to Camera unreachable.
- Preserved the v0.3.9 low-latency delivery, audio, decoder fallback/reporting, live telemetry, and no-signal renderer.

## v0.3.9

- Moved `No Signal Display` out of Advanced Settings and into the normal source controls directly below `Preset`.
- Advanced Settings now remains focused on transport/reconnect tuning and decoder diagnostics.
- No changes to signal-loss rendering, decoder selection/fallback, audio, reconnect handling, telemetry, or the low-latency delivery path.

## v0.3.8

- Added configurable signal-loss presentation under Advanced Settings: `Transparent`, `Black`, `Reconnecting...`, and `No Signal`.
- `Transparent` remains the default for backward-compatible behavior.
- Black/text modes output a synthetic BGRA frame instead of retaining the last live camera frame.
- Reconnecting and No Signal slates are rendered with a small built-in bitmap font and require no additional runtime dependency.
- Signal-loss frames use the last known video dimensions when available and fall back to 1280x720 before the first successful frame.
- Fresh live video replaces the signal-loss frame immediately when the stream returns.
- Preserved v0.3.7 decoder reporting/fallback, v0.3.6 reconnect behavior, live stats, optional audio, and the low-latency delivery path.

## v0.3.7

- Added live reporting of the actual GStreamer video decoder selected by `decodebin`.
- Added friendly decoder labels for NVIDIA NVDEC, Direct3D 11/12 hardware decode, Intel Quick Sync, VA-API, V4L2, AMD hardware, FFmpeg software decode, and OpenH264.
- Advanced Settings now exposes the exact selected decoder element name in telemetry.
- If a hardware decoder fails during startup before live video is established, the source immediately retries with software decoding and reports the fallback state.
- Manual reconnects and user settings changes clear the software-fallback state so hardware decoding can be tried again.
- Preserved v0.3.6 transparent signal-loss behavior, reconnect counting, live stats, optional audio, and low-latency delivery path.

## v0.3.6

- Fixed `Reconnect Now` so a manual reconnect increments the reconnect counter.
- Clear the OBS async video texture on reconnect, RTSP errors, EOS, pipeline teardown, missing URL, and RTSP URL changes.
- A source with no current video signal is now transparent instead of freezing the last valid frame.
- Clear stale resolution/FPS/codec telemetry while reconnecting or connecting to a replacement URL.
- Kept the v0.3.5 status-only UI refresh and the existing low-latency video/audio delivery paths.

## v0.3.5

- Fixed live telemetry rebuilding the entire OBS Properties UI every second.
- URL Show/Hide now stays in the state the user selected.
- Preset and Audio Mode dropdowns no longer collapse because of the stats refresh.
- Live stats still update about once per second by changing only the status label on the OBS UI thread.
- Preserved v0.3.4 diagnostics, Reconnect Now, and the v0.3.2 safety behavior.
- No RTSP video/audio pipeline changes.

## 0.3.4
- Added connection diagnostics with privacy-safe status messages for authentication failure, missing RTSP resources, timeouts, refused connections, unsupported media codecs, decode errors, and unsupported stream formats.
- Added `Connecting...` and `Reconnecting...` connection-state wording.
- Added explicit unsupported RTSP video/audio codec diagnostics while keeping automatic H.264/H.265 and AAC/Opus/G.711 detection.
- Added `Audio: No track` after a connected session has settled and no audio RTP track is present while audio is enabled.
- Added a `Reconnect Now` button that restarts only this source's RTSP pipeline and does not modify source settings or OBS audio-track routing.
- Diagnostic text never includes the configured RTSP URL, credentials, or stream token.
- Kept the v0.3.3 live telemetry system and v0.3.2 safety behavior unchanged.
- No changes to the working low-latency video/audio delivery path.

## 0.3.3
- Added automatic telemetry refresh about once per second while OBS is ticking the source.
- Live telemetry uses OBS's native `update_properties` signal; it does not restart or modify the GStreamer video/audio pipeline.
- Live refresh pauses for three seconds after a settings edit so the Properties UI is not repeatedly rebuilt while the user is actively changing controls.
- Changed uptime formatting to `MM:SS`, switching to `H:MM:SS` after one hour.
- Added a dedicated stats-session start timestamp so uptime, Video frame count, Audio Blocks, Dropped, and Reconnects share the same reset point.
- `Reset Stats` now resets the shared stats-session clock and all counters together.
- Kept `Refresh Stats` as a manual fallback.
- Kept the v0.3.2 safety behavior: no automatic source renaming and no automatic audio-track routing.

## 0.3.2
- Safety hotfix after a Windows OBS crash report.
- Removed automatic renaming of existing source instances during source creation.
- Removed automatic Track 1 assignment during source creation/update.
- Kept source type display name, Audio Blocks telemetry, codec detection, optional audio, and low-latency pipeline unchanged.
- Existing users should route audio tracks manually in OBS Advanced Audio Properties.

## 0.3.1
- Renamed the legacy default source instance name from `Low Latency RTSP (GStreamer)` to `Low Latency RTSP` without touching custom source names.
- Renamed telemetry from `Audio buffers` to `Audio Blocks` so the counter is not mistaken for queued latency.
- On the first audio enable, if the source has no output-track routing, automatically assigns Track 1. The initialization is remembered so later user routing changes are never overwritten.
- Kept the low-latency video and audio pipelines unchanged.

## 0.3.0
- Added automatic RTSP video codec detection for H.264/AVC and H.265/HEVC.
- Added optional audio with a user-controlled Enable Audio checkbox, disabled by default.
- Audio branch is not created when audio is disabled.
- Added automatic audio codec detection for AAC (MPEG4-GENERIC and MP4A-LATM), Opus, G.711 PCMU, and G.711 PCMA.
- Added Low Latency and Synchronized audio modes.
- Added manual audio delay from -500 ms to +2000 ms.
- Audio is normalized to 48 kHz stereo PCM before being handed to OBS.
- Added codec information and audio state to source telemetry.
- Renamed connected-duration telemetry from Live to Uptime.
- Kept the existing low-latency video delivery behavior unchanged.

## 0.2.2
- Generalized source UI wording to RTSP.
- Added Advanced settings visibility control and FPS telemetry.
- Fixed delivered-frame telemetry.

## 0.1.1
- Fixed GStreamer runtime DLL loading inside OBS on Windows.
