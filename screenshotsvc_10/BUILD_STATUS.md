## Recovery bundle patch

Static source checks and filename/path checks were performed in this
environment. MSVC/Windows SDK is not installed here, so a Windows build and
Media Foundation recovery test could not be executed.

Known limitation: a partial MP4 without a complete `moov`/sample table or a
video sample readable by Media Foundation is preserved unchanged. This patch
now reports that exact failure instead of silently skipping the bundle.

# Build status — ScreenshotService

## Findings from the user-provided runtime log (2026-11-05)

- Repeated startup failures use Windows exception `0xC00000FD` (`STATUS_STACK_OVERFLOW`) immediately after RAW/FLAC recovery begins. The recovery source had a 1 MiB automatic stack buffer in `CopyRawToRecoveredWav`; it is now heap-allocated. This is the likely stack-overflow trigger indicated by the code path and log sequence; symbol-level minidump analysis was not available.
- The same log shows a 17-minute recording whose FLAC-track mux failed because the 32-bit `mdat` box would exceed 4 GiB. The muxer now promotes that box to 64-bit `largesize` and rebases existing `stco`/`co64` offsets; it fails closed and preserves the source MP4 if the tables cannot be safely adjusted.
- The log's 20-second hang warning occurred during offline audio processing after the live capture had ended. The watchdog now stops before that post-processing begins.
- The log reports older RAW files without `.meta` sidecars. Those files are not self-describing, so their sample rate/channel/bit-depth cannot be recovered reliably; the original files are kept unchanged.

## Recovery and large-recording fixes (2026-11-05)

- The startup RAW recovery path no longer places a 1 MiB copy buffer on the thread stack. `CopyRawToRecoveredWav` allocates this streaming buffer on the process heap and frees it on every exit path; this addresses the stack-overflow pattern (`0xC00000FD`) seen immediately after startup recovery began processing a RAW file with metadata.
- Startup scanning does not open, parse, or decode `.partial.mp4` files. Video recovery remains deferred so a malformed MP4 or decoder/MFT cannot crash the recorder during startup.
- `mp4multi.c` now promotes a 32-bit `mdat` header to the 64-bit ISO-BMFF largesize form when appending the two FLAC tracks would push the box beyond 4 GiB. Existing `stco`/`co64` chunk offsets are rebased by the 8-byte header growth, and new FLAC chunk offsets begin after the expanded header. If the existing chunk tables cannot be safely rebased, the original MP4 is retained unchanged.
- The 20-second capture watchdog is stopped immediately after the live frame loop ends, before the offline WAV/AAC/FLAC post-processing stages. Long save operations are no longer mislabeled as a stalled capture.
- MultiTrack remains enabled by default. No settings migration changes the saved user preference.

## Current recording backend

- Recording prefers the selected adapter's native hardware API (NVIDIA NVENC, AMD AMF, or modern Intel oneVPL) when enabled and supported; Media Foundation hardware MFT remains the fallback. The experimental FFmpeg/D3D12VA recording backend and its settings/capability paths have been removed.
- Encoder availability is determined by a hardware-MFT probe which creates, writes, and finalizes an MP4 using the same Media Foundation configuration used by recording. The cache key is `ScreenshotService-probe-v29 MFT-only` and invalidates old D3D12-era capability results.
- The MFT encoder selector and Auto ranking use only profiles that passed the probe. Exact hardware URL, CLSID, candidate ordinal, and vendor pinning remain in place. If native encoding fails before the first compressed packet, the failed attempt is removed and the same request is retried through the hardware MFT. If MFT setup fails with both GPU-texture and CPU-fed input paths, recording stops with an explicit error; there is no software-video encoder fallback.
- HDR desktop capture and conversion remain on the D3D11 shader path. The HDR screenshot copy bug is also fixed: row copies use `img->bpp` (8 bytes per pixel for half-float HDR, 4 for SDR).
- The build does not compile the experimental `d3d12encode.c` backend or link `d3d12.lib`. The native vendor encoders use D3D11 textures; FFmpeg is optional for the smoke-test script and not required for video recording.

## AMD AMF follow-up

- The capture-adapter diagnostic uses only fields present in `DXGI_ADAPTER_DESC` (`Description`, `VendorId`, `DeviceId`). Adapter flags are not part of that structure; this avoids an MSVC C2039 build failure.

- H.264 B-picture configuration now sets `AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES` before `AMF_VIDEO_ENCODER_B_PIC_PATTERN`; otherwise runtimes can advertise B-frame support yet reject the configuration when the reference-frame limit remains at its default.
- AMF initialization gets additional conservative retries (no lookahead, no B-frames, no VBAQ, simpler preset) before falling back to the hardware Media Foundation transform. HEVC Main10/HDR is not silently downgraded by this retry ladder.
- The log now includes the DXGI capture adapter description and PCI vendor/device IDs, so a selected Media Foundation vendor cannot be mistaken for the native encoder actually picked from the capture adapter.

## Latest MSVC build-log fixes

- Declared and initialized the `mp4multi.c` elapsed-time origin used by the final mux diagnostic.
- Earlier MP4-recovery code used explicit `DWORD` casts for Media Foundation stream indices; startup MP4 decoding is now deferred and those Media Foundation calls were removed from `recovery.c`.
- Removed the invalid boolean test of `PathRemoveExtensionW`; the Windows API edits the path in place and returns `void`.
- Multi-track remains enabled by default (`GetBool(L"RecordMultiTrack", TRUE)`); no setting migration or default change was introduced.

## Recovered multi-track MP4 assembly

- The recovered-RAW worker now matches `*.recovered.mic.raw` and
  `*.recovered.system.raw` to the original `*.partial.mp4` name; it no longer
  incorrectly requires `<stem>.mp4` and a pre-existing combined `<stem>.wav`.
- Audio duration is reconstructed from the two RAW sample counts, persisted
  `WAVEFORMATEX` metadata and common QPC origins. The worker rebuilds the 48 kHz
  combined WAV directly from RAW, then muxes AAC if no audio stream is present.
- The worker encodes both source RAW files to indexed FLAC and appends the two
  labelled tracks. `Mp4Multi_HasFlacTracks` now reports complete only when both
  microphone and system FLAC labels are present.
- The synchronous startup scan never parses video. The low-priority worker
  validates top-level `ftyp`/`moov`/`mdat` and asks Media Foundation to read a
  video sample before promoting a `.partial.mp4` to `.recovered.mp4`. If the
  MP4 lacks a complete sample table or is unreadable, it remains untouched and
  the exact reason is written to the log.
- Original RAW and META sidecars, plus the individually recovered WAV files,
  are retained. MultiTrack remains enabled by default.

## Validation

- Source inspection and reference scans were run after removing the experimental D3D12VA backend and adding native vendor API paths.
- This environment cannot compile the Windows application with the target Windows SDK or run the user's GPU driver. A native MSVC build and runtime validation of MFT recording, HDR, audio, and final MP4 output are still required.

## Monitor selection and accent theme update

- Full-monitor recording asks which display to use when multiple displays are connected; a single display is selected implicitly. If Force-last has a saved stable target that disappeared, the picker reopens even if only one other monitor remains.
- Settings > Hotkeys includes a mode selector: ask when multiple displays are connected (default), or force the last selected physical display. In force mode, if the stored display target is no longer connected, the picker reappears and stores the replacement selection; an inline hint explains this. DisplayConfig target paths are used for stable identity; the GDI device name is retained for diagnostics only and is never treated as a stable physical identity.
- The monitor picker uses a custom theme-painted header (avoids low-contrast native tool-window title text), an explicit close button, a two-line monitor layout, and DPI-scaled dimensions. Height is calculated in client coordinates, so the final monitor row is not clipped by a native caption frame.
- Monitor rows retain the Windows accent on their outlines, with a thin idle stroke and stronger hover/focus states; a numbered accent chip separates the shortcut from monitor name, GDI device and resolution.
- Toolbar active-selection buttons and the selection rectangle/freeform outline use the Windows accent colour rather than a hard-coded blue.
- Runtime validation requires a Windows build and testing with monitor hot-plug; this environment cannot run the native Windows UI.


## Native encoder and recording reliability work

- Added the native vendor encoders from the compatible GPL source tree: NVIDIA NVENC, AMD AMF, and Intel oneVPL. The D3D12VA/FFmpeg recording backend remains removed; native APIs use D3D11 textures, and Media Foundation remains the hardware-MFT fallback.
- Native configurations retry without lookahead and then without B-frames if the vendor runtime refuses those settings. If startup/encoding/flush fails before the first compressed packet, the empty attempt is removed and the same recording options are retried through the hardware MFT. Failures after emitted packets preserve the partial for diagnosis.
- Added crash/first-chance diagnostics, a watchdog/minidump for a stalled recording, Windows Application Error/Hang event lookup after a native encoder crash, and one-start disablement of that native encoder after a recorded crash. Diagnostic writes used by crash handling bypass the opt-in debug-log switch.
- Added localized hover tooltips for the snip toolbar and recording stop button.
- Added `--record-smoke-test [seconds]` and `tests/record-smoke-test.ps1`. The test captures a short silent MP4 on the primary monitor and validates MP4 structure, not just the filename or file size.
- Intel oneVPL currently targets the modern API 2.x runtime for Arc/Xe class GPUs. No separate legacy Media SDK/QSV implementation is included; older Intel HD systems continue to use a compatible hardware MFT when offered by Windows.

## Validation limits

- Static validation and packaging are run in this environment. The Windows application cannot be compiled against the target Windows SDK or exercised against NVIDIA/AMD/Intel drivers here. The smoke test must be run on Windows hardware before treating native encoder support as runtime-verified.

## Registry-backed settings migration

- The application reads/writes configuration under `HKLM\Software\Gallery Inc\ScreenshotSvc`; the encoder capability cache is in its `EncoderCache` subkey. The old per-user `settings.ini` is imported once only and is no longer written.
- Run `setup-registry-settings.bat` as Administrator once to create the machine-wide key and grant normal users read/write/create-subkey access. ScreenshotService remains `asInvoker`; it does not need to run as administrator.
- Native Windows build/runtime validation is still required; this environment cannot execute the MSVC build or test the registry ACL on Windows.
- Removed the unused `ok` local from `RecoverPartialWav`; fixes MSVC C4189 under `/WX`.

## Startup recovery hotfix (2026-11-05)

- Removed the synchronous bundle reconstruction from the startup scan after the user reported a startup crash. Individual RAW/WAV/FLAC recovery and MultiTrack default behavior were preserved.
- The current implementation reintroduces bundle assembly only in a dedicated low-priority worker with a 2 MiB stack. It never opens or decodes `.partial.mp4`; it only processes a completed MP4 whose paired recovered RAW/meta files and combined WAV sidecar all validate.
- MultiTrack remains enabled by default for new recordings (`GetBool(L"RecordMultiTrack", TRUE)`).
- Windows MSVC build/runtime validation is required.

## Startup video recovery isolation
- `.partial.mp4` files are not opened, parsed, or decoded during the startup scan; the original file is preserved for a later isolated recovery operation.
- WAV/RAW/FLAC/GIF startup recovery is unchanged. MultiTrack default remains enabled.
- This is a startup-safety hotfix, not a full MP4 repair implementation; automatic video recovery is deferred until it can run in an isolated helper process.
