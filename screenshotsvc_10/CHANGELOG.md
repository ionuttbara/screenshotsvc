## Recovery bundle matching correction

- Match `*.recovered.mic.raw` and `*.recovered.system.raw` to the actual
  `*.partial.mp4` filename; do not require a finalized `*.mp4` name.
- Rebuild the combined 48 kHz WAV directly from both RAW streams and their
  persisted format/QPC metadata; no pre-existing combined WAV sidecar is
  required. Duration is derived from the latest audio end timestamp.
- Validate the partial MP4's top-level boxes and a readable video sample on a
  low-priority worker before promoting it. An incomplete MP4 is left untouched
  and the precise reason is logged.
- Add AAC only when no readable audio track already exists, then add both FLAC
  tracks. FLAC-track detection now requires both labels, not either one.
- Keep original RAW and META files; MultiTrack remains enabled by default.

# Changelog

All notable changes to ScreenshotService. Versions before 0.8.0.0's rename were released as
*BetterSnippingTool*; settings carry over automatically (see the README).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed
- Prevented a startup recovery stack overflow by moving the 1 MiB RAW-to-WAV copy buffer from the thread stack to the process heap.
- Multi-track MP4 muxing now upgrades an overflowing 32-bit `mdat` box to a 64-bit largesize box and rebases existing `stco`/`co64` chunk offsets, instead of refusing FLAC-track embedding above 4 GiB.
- Startup recovery leaves `.partial.mp4` files unopened and unchanged; malformed video containers cannot enter Media Foundation during application initialization.
- The live-capture watchdog is stopped before offline audio mixing/encoding, preventing long but progressing saves from generating false hang dumps.

### Fixed
- Fixed MSVC build errors in the recovery and multi-track MP4 modules: initialize the MP4 mux timing origin, pass Media Foundation's first-video-stream sentinel as a `DWORD`, and call `PathRemoveExtensionW` as its documented `void` API.

### Improved
- Three-track recording remains enabled by default when system sound and microphone are selected; upgrades preserve the saved setting and no longer apply a migration that silently disables it.
- Multi-track stop processing encodes the microphone and system FLAC tracks concurrently with a monotonic combined progress indicator. The offline mixer now bypasses interpolation for endpoints already at the 48 kHz output rate, and large RAW/FLAC/MP4 file paths use sequential-I/O hints and larger mux copy blocks. Stage timings are written to the log to make remaining save delays measurable.

### Added
- Startup recovery scans the configured recording folder for interrupted MP4, WAV, FLAC, GIF and RAW audio artifacts, promoting only validated outputs and preserving original RAW data when a WAV recovery copy is created.


### Fixed
- AMD AMF H.264 setup now configures `MaxNumRefFrames` before requesting B-pictures, as required by the VCE encoder's reference-frame constraints. If AMF still refuses the requested configuration, recording retries without B-frames, then without VBAQ, and with a simpler quality preset before falling back to the hardware Media Foundation MFT. Codec, bit depth and HDR colour signalling are not downgraded by these retries.
- Recording logs the actual DXGI capture adapter name and PCI vendor/device IDs before selecting a native encoder, making it possible to distinguish an AMF failure from a recording device running on a different GPU.
### Fixed
- Declared `Encoder_ConfigureVBR` in `encoder.h` with the definition-matching signature so `record.c` sees its prototype and MSVC does not issue C4013/C2220 at the VBR setup call.
- Automatic hardware encoder selection now tries every matching MFT candidate for NVIDIA before testing AMD or Intel, and then tries later candidates within those vendors if an earlier transform cannot complete the one-frame MP4 probe. The cache stores the successful vendor and candidate ordinal, so the recording path uses the candidate that was actually tested rather than re-selecting a different transform from the OS enumeration order.
- Vendor classification treats the MFT vendor-ID string or a PCI token in the hardware URL as authoritative; friendly-name matching is used only if both stronger identity sources are absent. The MFT-only cache key is `v29`, and the cache stores the exact hardware activation URL plus MFT CLSID so recording can pin the tested transform even if enumeration order changes.

### Removed
- Removed the experimental FFmpeg/D3D12VA recording backend and its settings/capability paths. Native NVIDIA NVENC, AMD AMF and modern Intel oneVPL use D3D11 textures; Media Foundation hardware MFT remains the fallback. The build does not depend on `d3d12.lib` or FFmpeg for video encoding.

### Added
- Settings and the encoder capability cache now live under
  `HKLM\Software\Gallery Inc\ScreenshotSvc`. The old `settings.ini` is
  imported once when present and is no longer written. The included
  `setup-registry-settings.bat`/`.ps1` creates the machine-wide key and grants
  standard users the minimum read/write/create-subkey rights so the app itself
  does not need to run elevated.
- A single connected display is selected implicitly, so the monitor picker is
  shown only for multiple displays; in Force-last mode, a saved physical target
  that disappeared still opens the picker for explicit replacement.
- The full-monitor picker has a custom theme-aware header/close control,
  stronger title/detail hierarchy, DPI-scaled sizing and corrected client-area
  height so the last display choice is fully visible. Monitor names and
  resolution are shown on separate lines, with compact accent-coloured number
  chips and lighter default accent borders.
- Romanian, Spanish and Hungarian interface translations, loaded from `langs/`.
- Fresh-install recording defaults to system sound + microphone, embeds the AAC mix and separate mic/system FLAC tracks, and matches monitor refresh pacing; encoder preference remains Automatic.
- Screenshot capture saves directly without opening the post-capture annotation/editor window. JXR captures can now save a matching PNG copy for both HDR and SDR captures when the sidecar option is enabled.
- The application accent colour follows the Windows accent registry value and refreshes after a system settings notification.
- Added probed AV1 8-bit, 10-bit SDR and 10-bit HDR encoder profiles; a profile is offered only when the active Media Foundation encoder and MP4 sink successfully finalise a test file. AV1 may therefore remain unavailable on Windows setups whose MP4 sink cannot mux it.
- The MP4 colour-tag writer recognises AV1 `av01` sample entries, for configurations where AV1 MP4 output is supported.
- The GPU encoder selector is populated from the installed DXGI adapters and displays their model names instead of offering hard-coded vendors. Hardware MFT matching reads Media Foundation's hardware vendor ID as its documented string form (for example, `VEN_1002` for AMD), as well as friendly names; the capability cache key includes all enumerated adapters and is bumped to `v14` to invalidate stale negative results. MFT candidate names and vendor IDs are logged for diagnosis.

### Fixed
- Fixed the Media Foundation API argument types in `encoder.c`: `MFTUnregisterLocalByCLSID` and `MFTEnumEx` take their CLSID/GUID category arguments by value in the Windows SDK, not by pointer.
- The selected settings tab now uses the same live Windows accent colour as the other accent controls; removed the separate hard-coded blue shades.
- Cursor compositing for SDR captures uses an opaque 24-bit intermediate surface and copies back only the cursor rectangle. This avoids relying on potentially unset alpha values in Desktop Duplication frames, a cause of black cursor rendering on some systems.

### Limitations
- Media Foundation fallback selection uses a process-local MFT registration and verifies the transform selected by Sink Writer. Native NVIDIA/AMD/Intel APIs have separate driver/runtime requirements and have not been runtime-verified on all vendors. Recording remains hardware-only: it is rejected rather than silently switching to software video encoding. All native and fallback paths still require validation on real NVIDIA, AMD and Intel hardware.
- Native Windows build and runtime validation are still required; this source archive was updated in a non-Windows environment.

### Added
- While live-video Draw is enabled, a low-level mouse hook routes movement into the
  pen canvas and suppresses mouse clicks, wheel events and auxiliary buttons inside
  the recording region so the underlying app/game is not accidentally operated.
  The recording-bar controls remain interactive; Ctrl+right-click still toggles Draw
  and Ctrl+Alt+right-click clears the live strokes.
- The screenshot-preview heal eraser is rectangular rather than brush-based. It scores
  candidate patches on all four sides by the surrounding seam colours, searches nearby
  offsets for a better match, and feathers the replacement into the selection. This avoids
  blindly cloning the side with the most available space, which could introduce large
  black or visibly mismatched blocks. The selected rectangle is repaired only on release.
- Automatic encoder selection now prefers HEVC Main10 SDR, then HEVC 8-bit,
  before falling back to H.264 on SDR displays. Encoder capability cache is
  versioned so the updated ranking is not blocked by older probe results; the
  recording log reports both requested and resolved encoders. GIF's temporary
  MP4 deliberately stays H.264 for broad decoder compatibility.
- HEVC Main10 SDR is handled as Rec.709 SDR, separate from the BT.2020/PQ HDR
  profile. The 12-bit display signal is not emitted as 12-bit HEVC; SDR capture
  uses a 10-bit source surface when DXGI provides one and encodes Main10/P010.
- The recording bar reports `Saving recording NN%` while stop-time AAC/FLAC
  encoding, MP4 finalization and remuxing run. Active recordings use a
  `.partial.mp4` path and are published as `.mp4` only after successful processing.
- Display-mode changes reacquire Desktop Duplication and scale the new crop into
  the original fixed-size video canvas, keeping MP4 dimensions stable. A switch
  away from the original floating-point colour format during floating-point
  capture is stopped rather than silently misinterpreted.
- The selected rectangle is authoritative for MP4 and GIF; full-monitor capture
  is an explicit overlay toolbar action, not a global setting that can override
  a selected area. Controls remain below a selected region when possible and at
  top-left for full-monitor capture.
- An Exit request waits for post-processing rather than terminating the worker.
  An unhandled exception on the UI thread tries to finalize the active recording;
  hard termination or a fault on the worker itself can still leave partial files.

### Fixed
- **Memory no longer piles up while the program waits.** After a snip, the
  screen-readout buffer (66 MB at 4K HDR) stayed allocated, and the graphics
  driver kept freed copies of it cached, so every recording, HDR switch or
  display change added another 64 MB - up to about 180 MB for a program sitting
  in the tray. The buffer is now released and the driver's cache handed back a
  few seconds after each snip, and capture rebuilds trim before they release.
  Idle memory after a snip: 115 MB → 53 MB, with snips exactly as fast as
  before.

### Added
- After 5 minutes without a snip, the capture machinery is released entirely,
  bringing idle memory down to about 17 MB; the next snip takes about 0.1 s
  longer, once. `IdleReleaseMinutes` in the INI changes the delay (0 = never).

## [0.8.3.0] - 2026-10-27

### Added
- Optional native hardware encoding through NVIDIA NVENC, AMD AMF and modern Intel oneVPL, with retries for rejected lookahead/B-frame configurations and Media Foundation hardware-MFT fallback if native startup fails before the first compressed packet.
- Recording crash diagnostics: last-stage reporting, minidumps, recording stall watchdog, recent Windows crash/hang event lookup, and one-start disablement of a native encoder after a recorded crash.
- Localized hover tooltips for snip toolbar and recording stop controls.
- An end-to-end recording smoke test (`--record-smoke-test` and `tests/record-smoke-test.ps1`) that validates MP4 structure and optionally checks/decodes streams with FFprobe/FFmpeg.

### Changed
- The oneVPL path targets the modern API 2.x runtime for Intel Arc/Xe GPUs.
  Legacy Intel HD/QSV is not implemented separately and continues through a
  compatible hardware MFT when Windows exposes one.
- Third-party encoder SDK include paths are treated as external/system headers
  by the supported Windows build scripts to avoid applying project warning
  policy to vendor SDK headers.

### Fixed
- Logging used by crash/watchdog reporting now writes even when the opt-in
  diagnostic log is disabled.
- The recorder waits out the worker's final completion race before restarting
  through MFT, avoiding a failed fallback caused by `busy` still being set.

## [0.8.2.0] - 2026-10-10

### Changed
- **Recordings keep their frames on the GPU.** The capture is handed to the
  hardware encoder as GPU textures instead of making a round trip through system
  memory. On the 8-bit paths the RGB-to-NV12 conversion now runs on the GPU too;
  on the HDR path the PQ conversion renders straight into the encoder's P010
  texture. At 4K and 30 fps, the recorder's own work fell from 9.2 to 0.08 ms per
  frame for HDR and from 5.8 to 0.04 ms for 8-bit; CPU use from 35 % to 1 % and
  from 205 % to 3 % of a core. Falls back to the previous path by itself where
  the hardware does not support this; `RecordZeroCopy=0` forces the fallback.
- HDR light-level metadata (MaxCLL/MaxFALL) is now measured on the GPU, with
  results identical to the previous CPU measurement.
- The keyboard hook runs on a thread of its own, so nothing else the program does
  can delay it long enough for Windows to hand Win+Shift+S to the Snipping Tool.
- The overlay comes up about 200 ms faster on every snip: it reuses the capture's
  graphics device and compiles its shaders once, at startup, instead of building
  both each time - at no extra memory cost while idle.
- Freeform selections redraw three to five times faster while being drawn.
- Saving copies the encoded image less often, and the plain-DIB clipboard format
  is only built when the snip has transparency.
- The diagnostic log is moved aside at 4 MB instead of growing without limit.

### Fixed
- Two snips taken within the same second no longer overwrite each other, and an
  HDR snip's PNG copy can no longer overwrite another snip's file.
- If the overlay's graphics setup failed, the whole program quit. It now just
  cancels that snip.
- After a display change or a lost screen capture, snips no longer stay on the
  slow fallback capture until the next restart.
- Switching the language while a recording or a save was in progress could read
  freed memory.
- Stopping audio capture could release objects its thread was still using.
- Changing the save folder in the settings while a save or recording was running
  could hand it a half-written path; both now work from a copy of the settings
  taken when they start.
- Long log lines in non-Latin scripts were dropped.

### Security
- The shader compiler and other system DLLs are loaded from System32 only, and
  the executable resolves its own imports from System32: a DLL placed next to
  `ScreenshotService.exe` (for example in a Downloads folder) is no longer picked up.
- A language setting that looks like a path is refused instead of being used to
  load a file from outside the `langs` folder.
- Stricter bounds checks when patching MP4 metadata, and a cap on retries when
  the display configuration keeps changing.

## [0.8.1.0] - 2026-10-10

### Fixed
- Recording kept running on a frozen frame after a display-mode change, a UAC
  prompt or a game switching to exclusive fullscreen, and leaked graphics memory
  each time. The capture is now rebuilt on the same device; a recording whose
  graphics device is lost altogether stops and keeps what it captured.
- Stopping a recording whose file took longer than 15 seconds to finish could
  crash.
- Pressing the hotkey while a recording was starting or finishing could corrupt
  the recorder.
- When rebuilding a recording's sound from its WAV failed part-way, the truncated
  result replaced the original and the WAV - by then the only full copy of the
  sound - was deleted.
- Quitting during a slow HDR save left a truncated `.jxr`; exiting now waits for
  pending saves.
- A key held across the hook's periodic refresh could reach the foreground
  window unpaired.

## [0.8.0.0] - 2026-09-21

### Added
- English and German interface, following the Windows display language.
  Further languages can be added by copying one text file; untranslated lines
  fall back to English.
- HDR recordings carry the light-level metadata (MaxCLL/MaxFALL, mastering
  display) players need to show them at the right brightness.
- Tabbed settings window with an Apply button, and a configurable folder for
  recordings.
- User guide in English and German.

### Changed
- Renamed from BetterSnippingTool to **ScreenshotService**. Settings, autostart and
  translations move over automatically on first start.
- 8-bit recordings of an HDR display are tone-mapped from the floating-point
  capture, so they no longer come out about three times too bright and washed
  out.
- With the WAV sidecar used as the recording's sound source, the `.wav` is
  removed once its sound is in the MP4, so players do not offer the sound twice.

### Fixed
- The overlay could vanish a fraction of a second after appearing, as if Escape
  had been pressed, when Windows took focus back on releasing the Windows key.
- Under remote desktop the overlay could show a black screen with selection
  trails, because a fresh screen capture handed over an empty first frame.
- An unchanged custom snip folder was reset to the default every time the
  settings were confirmed.
- The diagnostic log setting only took effect after a restart.

## [0.7.0.0] - 2026-09-20

### Added
- **10-bit HDR recording**: on an HDR display, HEVC Main10 in BT.2020 with the
  PQ curve, so recordings keep the colours and highlights seen on screen.
- The available video encoders are found by writing a test file with each one,
  not just by asking the driver, and the best working one is chosen
  automatically; it can also be picked in the settings.
- Optional lossless WAV copy of a recording's sound, and the option to build the
  MP4's sound from it.
- The settings window opens centred and remembers where it was moved to.

### Removed
- The shutter sound after each snip.

## [0.6.0.0] - 2026-09-19

### Added
- **Screen recording** of a selected area to MP4 (H.264 + AAC), with system
  sound or microphone, a recording frame and a stop bar. The hotkey stops a
  running recording.

### Fixed
- Recordings of a quiet desktop had no sound track, and sound jumped after
  pauses: silence is now filled in so audio and video stay aligned.
- Freeform mode drew a selection from the corner of the screen before the first
  click.

## [0.5.0.0] - 2026-09-18

### Added
- Settings window, following the Windows light or dark setting.
- Start with Windows, and a notification naming the saved file.

## [0.4.0.0] - 2026-09-18

### Added
- **HDR screenshots**: on an HDR display the capture is floating point and is
  saved as JPEG XR, with a tone-mapped PNG beside it; the clipboard gets an
  accurate 8-bit copy.

## [0.3.0.0] - 2026-09-18

### Added
- The selection overlay: the frozen, dimmed desktop with a toolbar for
  rectangle, freeform, window and full-screen snips, across all monitors.

## [0.2.0.0] - 2026-09-18

### Added
- Screen capture via Desktop Duplication with a GDI fallback; snips go to the
  clipboard and to a PNG file at the same time.

## [0.1.0.0] - 2026-09-18

### Added
- Tray program that takes over Win+Shift+S and PrtScn while it runs, leaving
  the Windows Snipping Tool installed and untouched.

## UI follow-up
- Keep the screenshot annotation toolbar visible during canvas repaints by clipping parent painting from child controls; bring the editor to the top and focus it explicitly, and log editor creation failures.
- Open the annotation editor for both PrintScreen and Win+Shift+S still captures.
- Right-click while live video drawing is enabled now turns drawing off; it no longer opens the color picker. The record bar reflects the disabled state.

## Preview drawing and video-pen gestures

- Added Pen and heal-eraser tools to the pre-crop selection preview; edited pixels are refreshed live in the preview and copied into the selected image, including its packed 10-bit SDR buffer when present.
- PrintScreen now opens the preview even when full-screen mode is preferred; in that case the preview starts in full-screen mode so the user can draw before confirming. The explicit tray Fullscreen command remains direct.
- During recording, Ctrl+right-click toggles live Draw, while Ctrl+Alt+right-click clears its existing strokes. A low-level mouse hook keeps these gestures available while the live-pen overlay is click-through.
## Hardware encoder selection — Media Foundation
- Cache key is now `v21`. For a global Auto setting, encoder resolution now prefers NVIDIA across supported profiles, then AMD, then Intel; the established codec/profile ranking applies within a vendor. This fixes the case where a higher-ranked AMD Main10 profile could beat a valid NVIDIA HEVC8 MFT and force the user to select NVIDIA manually. Runtime fallback also tries profiles on the chosen vendor before crossing to another vendor.
- Fixed Auto selection to try each enumerated matching hardware MFT for NVIDIA before moving to AMD/Intel, resolve codec profiles vendor-first (NVIDIA profile rank before AMD/Intel), and cache/pin the exact successful candidate ordinal per codec profile. Vendor ID and hardware URL are authoritative; friendly-name matching is only used when those identifiers are absent. Vendor ID matching now tolerates a PnP prefix/suffix around `VEN_xxxx`. The probe cache key was bumped to v21; recording and cached-probe logs show resolved vendor and candidate ordinal, and the exact hardware URL plus CLSID are now the primary selector during recording.

- In `Auto`, each codec profile is now probed against NVIDIA, AMD and Intel hardware MFTs in deterministic order rather than trusting the order returned by `MFTEnumEx`. The actual successful vendor is cached per profile and reused when recording starts, so probing one MFT and recording with a different one is avoided.
- Vendor matching can use the PCI token in `MFT_ENUM_HARDWARE_URL_Attribute` when the driver's separate vendor-ID attribute is missing.

## Build fix — 2026-10-20

- Added a forward declaration for `TeardownGpu()` before `StartD3D12Backend()` uses it, resolving the reported MSVC C4013/C2371 errors treated as C2220.

## 2026-11-05 — Recovered MultiTrack MP4 assembly

- Added a low-priority startup worker that pairs already recovered microphone/system RAW streams with their completed MP4 and combined WAV sidecar.
- Rebuilds indexed FLAC tracks from the original PCM using the WAV's exact sample duration, then adds the microphone and system FLAC tracks to the existing MP4 without re-encoding video or AAC.
- Uses a 64-bit `mdat` box promotion when the appended tracks cross the 4 GiB boundary. Original files and recovered sidecars remain available; incomplete `.partial.mp4` files are never opened by this startup path.

## 2026-11-05 — Startup recovery hotfix

- Replaced synchronous startup bundle reconstruction with a dedicated low-priority worker and 2 MiB stack.
- The worker only handles completed MP4 files with matching recovered RAW streams, exact `.meta` files and the combined WAV sidecar; it never opens or decodes `.partial.mp4` files.
- Original RAW sidecars and recovered audio files are retained after successful assembly.

## Startup video recovery isolation (2026-11-05)
- Disabled automatic Media Foundation opening/decoding of `.partial.mp4` files during startup.
- A structurally complete or incomplete partial MP4 is now preserved byte-for-byte and reported for later recovery; startup only performs a bounded top-level atom scan.
- WAV/RAW/FLAC/GIF recovery remains enabled. MultiTrack remains enabled by default for new recordings.
- This avoids letting malformed MP4 sample tables or decoder/MFT/driver faults prevent ScreenshotService from opening.
