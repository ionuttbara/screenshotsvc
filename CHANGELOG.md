# Changelog

All notable changes to Nitshot. Versions before 0.8.0.0's rename were released as
*BetterSnippingTool*; settings carry over automatically (see the README).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.8.6.8] - 2026-11-04

### Fixed
- **"No video encoder is available" although the graphics card has one**
  (reported on a Radeon RX 7900 XTX, driver 26.10.x, where the settings
  offered only "Automatic"). The encoder check let Windows pick the card's
  own encoder and gave up when that one failed; it now tries Windows'
  software encoder next. And when Windows offers no encoder at all, the
  card's own encoder (AMD, NVIDIA, Intel) still records H.264 - it needs
  nothing from Windows but the MP4 writer. The encoder check runs again
  once after the update.

## [0.8.6.7] - 2026-10-25

### Added
- **Tooltips on the snip toolbar's buttons** - Rectangle, Freeform, Window
  and Whole screen with their keys (1-4), Record, and Cancel (Esc) - after
  the pointer rests on one for half a second, and on the recording bar's
  stop button. In English and German.

### Fixed
- The recording bar's stop button said "Stop" in every language.

## [0.8.6.6] - 2026-10-25

### Changed
- **Intel: recordings start about a second sooner.** What the graphics card
  can do is asked once per run instead of twice per recording, and lookahead
  the runtime has refused once is not asked for again until Nitshot
  restarts. (Confirmed on an Arc 140V with 0.8.6.5: every recording through
  the card's own encoder, with B-frames; lookahead is refused there.)

## [0.8.6.5] - 2026-10-23

### Fixed
- **Intel: lookahead no longer costs the card's own encoder.** On an Arc 140V
  Intel's runtime refused to start with lookahead from system memory
  (MFX_ERR_UNDEFINED_BEHAVIOR), and the recording fell back to Windows'
  encoder. When the runtime refuses, Nitshot now tries again without
  lookahead, then without B-frames as well.

## [0.8.6.4] - 2026-10-23

### Fixed
- **AMD: the card's own encoder was never used.** AMF's pre-analysis takes
  NV12 frames only, and Nitshot handed it BGRA (SDR) or P010 (HDR), so Init
  failed every time and recordings quietly went through Windows' encoder
  instead (found in a tester's log on a Radeon, driver 26.20). BGRA is now
  converted to NV12 on the graphics card first, as on the Intel path; P010
  is encoded without pre-analysis; and an encoder that still refuses its
  configuration is tried once more without pre-analysis.

## [0.8.6.3] - 2026-10-20

### Changed
- **Intel: frames go to the encoder in system memory**, read back from the
  graphics card and uploaded by Intel's runtime on its own device - the way
  Intel's samples feed an encoder. Handing over textures from Nitshot's own
  allocator failed on an Arc 140V: with lookahead the program closed, without
  it the runtime reported "device failed" on the first frame. The texture
  path stays available as `RecordIntelVideoMemory=1` in settings.ini.

### Fixed
- **When the graphics card's own encoder fails before its first picture,
  Nitshot records again straight away through Windows' encoder**, switches
  the card's encoder off and says so - instead of a recording that did not
  start.
- After the card's encoder had failed, finishing the recording could hang
  for good (closing Intel's runtime did not return), and no further
  recording could start. Closing a failed encoder now gets 3 seconds.

## [0.8.6.2] - 2026-10-20

### Added
- **A recording through the graphics card's own encoder is watched more
  closely**, for deaths the crash report of 0.8.6.1 could not see: serious
  exceptions are logged as they happen; a recording that stands still for
  10 seconds is logged with a dump of every thread; the program leaving in
  the middle of one is logged; and at the next start Windows' own record of
  the crash or hang is copied into the log.

## [0.8.6.1] - 2026-10-20

### Added
- **A crash leaves a report**: a line in the log - even with the diagnostic
  log off - naming where it happened and what Nitshot was doing, and a small
  crash dump beside it to send in.
- **If Nitshot ends during a recording through the graphics card's own
  encoder, it switches that encoder off at the next start** and says so, so
  that recording works again straight away, through Windows' encoder.

### Fixed
- Recording on Intel Arc closed the program (first reported on an Arc 140V).
  The cause is not yet known - this build records each step of the first
  frames and the crash itself, to find it - but the picture the encoder
  reads is now made the way the colour conversion was tested with, and the
  encoder's surfaces say which memory they are, as oneVPL 2.x expects.

## [0.8.6.0] - 2026-10-19

### Added
- **Intel Arc and Xe graphics record through the card's own encoder too
  (oneVPL)**: B-frames, lookahead and HDR10 where the card offers them, with
  the screen converted to the encoder's format on the GPU. **Not yet tried on
  Intel hardware**; anything wrong falls back to Windows' encoder, and the
  diagnostic log records in detail what the card answered. Older Intel GPUs
  record as before.

## [0.8.5.0] - 2026-10-19

### Added
- **AMD graphics cards record through the card's own encoder too (AMF)**,
  as NVIDIA cards do through NVENC: B-frames for H.264 where the card has
  them, pre-analysis as lookahead, and HDR10 with the monitor's colour
  volume. **Not yet tried on AMD hardware**; if anything is wrong it falls
  back to Windows' encoder, and the diagnostic log says in detail what the
  card answered.

### Changed
- The setting is now *Use the graphics card's own encoder directly (NVIDIA,
  AMD)*, and its INI keys are `RecordDirectEncoder` and `RecordEncoder*`;
  the `RecordNvenc*` keys of 0.8.4.0 carry over.

## [0.8.4.0] - 2026-10-19

### Added
- **On NVIDIA graphics cards, recordings go through the card's encoder
  directly (NVENC)** instead of through Windows' video encoder, which on
  NVIDIA cannot use B-frames or look ahead - both of which NVENC now does at
  the same bitrate. Measured on screen-like content: a clearer picture in
  every case, 0.5 to 3.8 dB, at the same file size or up to 11 % smaller.
  H.264, HEVC and 10-bit HDR all work this way; HDR recordings also carry
  the monitor's colour volume inside the video stream. Without an NVIDIA
  card, or with an older driver, recordings work as before. *Use the NVIDIA
  encoder directly* in the Recording settings switches it off; quality
  preset, B-frames and lookahead are in the INI.

## [0.8.3.0] - 2026-10-19

### Added
- **Record the sound of any sound device**, not just the default speakers or
  microphone: every playback device (recorded as what it plays) and every
  recording device is listed by name in the settings. The two default entries
  remain and follow Windows' current default. An existing setting carries over.
  If the chosen device is unplugged, the video is recorded without it and the
  notification says why.
- **A second sound source**, recorded as a second sound track in the MP4 - or,
  with *Mix both into one track*, mixed with the first into a single track.
  Each source keeps its own timing, so neither drifts against the video.
- With the lossless WAV option and two sources, one WAV per source
  (`.track1.wav`, `.track2.wav`), plus `.track0.wav` with the mix when mixing.
  Building the MP4's sound from the WAVs works with two tracks too.
- Sound tracks are named after their device, so a player's track menu shows
  which is which.
- *…but keep each source's .wav when mixing*: when two mixed sources are built
  into the MP4 from their WAVs, each source's own WAV can be kept.
- After 5 minutes without a snip, the capture machinery is released entirely,
  bringing idle memory down to about 17 MB; the next snip takes about 0.1 s
  longer, once. `IdleReleaseMinutes` in the INI changes the delay (0 = never).

### Changed
- Sound is now always recorded at 48 kHz stereo, with Windows converting from
  whatever the device runs at. Devices at 96 or 192 kHz, which were refused
  before, now work, and surround devices are mixed down to stereo by Windows
  instead of keeping only the front pair.
- H.264 recordings use the High profile instead of Baseline: better picture at
  the same bitrate, and playable everywhere H.264 is.
- The encoder test made on first start now reads back what each encoder
  actually wrote. An encoder that quietly writes 8-bit when asked for 10-bit
  HDR - NVIDIA's does exactly that when asked for 12-bit - is no longer
  offered, so an "HDR" recording cannot turn out to be 8-bit. The encoders are
  tested once more after the update.

### Fixed
- **Memory no longer piles up while the program waits.** After a snip, the
  screen-readout buffer (66 MB at 4K HDR) stayed allocated, and the graphics
  driver kept freed copies of it cached, so every recording, HDR switch or
  display change added another 64 MB - up to about 180 MB for a program sitting
  in the tray. The buffer is now released and the driver's cache handed back a
  few seconds after each snip, and capture rebuilds trim before they release.
  Idle memory after a snip: 115 MB → 53 MB, with snips exactly as fast as
  before.

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
  `Nitshot.exe` (for example in a Downloads folder) is no longer picked up.
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
- Renamed from BetterSnippingTool to **Nitshot**. Settings, autostart and
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
