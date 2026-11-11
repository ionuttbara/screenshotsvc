# ScreenshotService

**Version 0.8.3.0**
Copyright © 2026 Rekow IT

A small native Win32 replacement for the Windows 10 Snipping Tool: a tray-resident
C17 program with no runtime dependencies that takes over `Win+Shift+S` and `PrtScn`,
freezes and dims the desktop, and puts the result on the clipboard and by default
in `C:\Screenshots` (the destination is configurable). On an HDR display it
captures in floating point and saves real HDR files.

**Formerly BetterSnippingTool.** Renamed before the first release. On first start,
ScreenshotService moves `%APPDATA%\BetterSnippingTool` to `%APPDATA%\ScreenshotService`, carries the
settings once from the legacy INI into HKLM\Software\Gallery Inc\ScreenshotSvc and re-registers autostart under the new name.
Snips already saved in `Pictures\BetterSnippingTool` stay where they are; new images and recordings go
to `C:\Screenshots` by default (configurable in Settings).

**User guide:** [English](docs/UserGuide.en.md) · [Deutsch](docs/Benutzerhandbuch.de.md). This README is the technical reference; what changed in each version is in the [changelog](CHANGELOG.md).

## What it does

| | |
|---|---|
| Capture | rectangle, freeform, window, full screen — over a frozen, dimmed desktop |
| HDR stills | fp16 capture to JPEG XR; SDR screenshots to JXR, preserving DXGI 10-bit data when exposed |
| Recording | MP4 with system sound, microphone or both mixed together; optional three embedded audio tracks; animated GIF clips up to 15 seconds |
| SDR/HDR recording | HEVC Main10 SDR/BT.709 or 10-bit BT.2020 PQ HDR, subject to encoder capability; H.264 fallback |
| Destinations | clipboard and disk in one step |
| Interface | tray icon, settings window, light/dark, English, German, Romanian, Spanish and Hungarian; ScreenshotService branding |
| Annotation | PrintScreen editor: red pen, rectangle, circle, text, colour picker and soft clone/heal eraser |
| In-recording overlay | Draw toggle with freehand pen and cursor composed into the captured region |
| Colour picker | `Win+Shift+X`, live RGB/HEX tooltip and nearest named colour; click copies the value |

Press the hotkey, the desktop freezes and dims, and the toolbar offers
**rectangle · full-screen record · freeform · window · full-screen snip · record area · GIF · close**.
With the Print Screen hook enabled, **Shift+PrtScn** opens the area-recording selection; the dedicated full-screen
record button starts recording the entire monitor. Full-monitor recording opens the themed monitor picker when multiple displays are connected; a single available display is selected implicitly. Settings > Hotkeys can switch to forcing the last selected physical display; if that saved physical display disconnects, the picker opens again rather than silently switching targets. The GIF button records silently for up to 15 seconds at 15 FPS with a maximum output width of 1920 px. `Alt+PrtScn` toggles full-monitor video recording; `Win+Shift+X` opens the colour picker. On a display with
advanced colour on, stills are floating point end to end and are written as JPEG XR.
Everything is reachable from the tray icon, including a settings window.

Measured on a 3840×2160 desktop: **grab 7–16 ms**, PNG encode ~200 ms for the full
screen and under 1 ms for a typical snip. The grab is what gates the overlay, so
the encode being the slow half does not matter — it happens after the user already
has their selection.

## The overlay

One borderless window across the whole virtual desktop, drawn with D3D11 so that
the swap chain can be floating point and show HDR content as it really looks.
There is no vertex buffer and no input layout anywhere: every draw is a single
quad built from `SV_VertexID`, with the destination and source rectangles coming
from a constant buffer. Four small shaders cover the entire UI.

**The overlay owns no device.** It draws with the one the capture machinery
already keeps warm for Desktop Duplication, and its shaders are compiled once per
process, off the UI thread at startup, into plain bytecode; the compiler DLL is
released afterwards. Creating the shader objects on the current device then
takes under a millisecond. Before, every snip created a device and compiled the
shaders on the path between the hotkey and the overlay appearing - measured at
about 200 ms, nearly all of it the device. A device of its own kept warm instead
would have cost 12 MB and a few dozen driver threads at idle; borrowing costs
nothing. Only with nothing to borrow (GDI capture) does the overlay create one,
and it gives it back when it closes.

| Mode | Behaviour |
|---|---|
| Rectangle | drag; captures on release |
| Freeform | draw a closed shape; the area outside the outline is transparent |
| Window | highlights the window under the cursor, click to take it |
| Fullscreen | takes everything immediately |
| Record | drag an area to record instead of capture; see below |

`Esc`, right-click or the toolbar's ✕ cancels; `1`–`4` pick a capture mode directly. The
mode is remembered for next time, the way the Snipping Tool does it — except
fullscreen, which is a one-off action rather than a mode worth returning to.

**Freeform** shares one rasteriser between the live preview and the finished cut
(`poly.c`): the preview uploads the coverage mask to the GPU as the outline grows,
and the saved image multiplies its alpha by the very same mask. A second
implementation would eventually disagree with the first about an edge pixel. The
alpha is premultiplied, so pasting a cut-out onto a light background shows no dark
fringe.

**Window mode** walks the top-level windows in z-order rather than calling
`WindowFromPoint`, which would only ever return our own overlay, and highlights
`DWMWA_EXTENDED_FRAME_BOUNDS` so the selection matches what the user sees instead
of including the invisible resize border. Cloaked (background UWP) windows are
skipped.

**The toolbar** is rendered on the CPU into a premultiplied BGRA texture. GDI has
no anti-aliasing, so each layer is drawn white-on-black at four times the final
size and box-filtered down. That is what gives the icons clean edges without
dragging in Direct2D — which the Windows SDK does not expose to C at all, its
headers declaring the interfaces as opaque types with no vtables — or a font.

**The overlay defends its activation for its first 800 ms.** It used to close
the instant it lost activation, and occasionally did so 140–250 ms after
appearing — with no key and no second hotkey in between, per the log. That is
the moment the Windows key is released, and the shell reacting to the release
took activation away. To the user it looked like a phantom Escape. Losing
activation that early is never a deliberate switch, so the overlay now takes it
back (a bounded four times, posted rather than done inside `WM_ACTIVATE`, where
the two windows would trade activation). Reproduced on demand with a window that
steals activation at a chosen delay: at 125 ms the overlay stays; at 1250 ms it
closes as before. The log now names the window that took over, by class and
process — `WM_ACTIVATE` does not, when that window belongs to another process.

Two things guard against the worst way this program could fail, an overlay stuck
on screen over everything:

- a 250 ms watchdog closes it if it is ever not the foreground window, because
  another process can take the foreground without our window seeing `WM_ACTIVATE`;
- the hotkeys stay hooked while the overlay is up but do nothing, so a second
  Win+Shift+S cannot fall through to the shell and launch Snip & Sketch on top of
  our own overlay.

## Capture

`Capture_VirtualDesktop()` composites every attached output into one
virtual-desktop image, then falls back wholesale if any output fails — a
half-black desktop would be worse than a slower correct one.

- **DXGI Desktop Duplication** is the fast path. The D3D11 device and the
  duplication objects are built at startup and kept warm, so a hotkey press pays
  for none of that. A static desktop produces no new frames, so the first
  `AcquireNextFrame` usually times out; the duplication object is then rebuilt,
  because a fresh one always delivers the current desktop as its first frame.
- **GDI `BitBlt`** is the fallback, for indirect and virtual displays where
  duplication is unavailable, and for **rotated** outputs — duplication hands
  those out in the panel's native orientation, and GDI simply reads them the
  right way round. `ForceGdi=1` under `HKLM\Software\Gallery Inc\ScreenshotSvc` pins this path for testing.

**Memory while waiting.** A grab needs a full desktop of CPU-readable staging
memory - 66 MB at 4K HDR - and the display driver does not hand freed textures
back to the system by itself: it keeps them in a cache of its own, even past the
release of the device that made them. Measured at 4K HDR, the waiting program
held 115 MB after its first snip, and every capture rebuild (a recording, an HDR
switch, a display change) added another 64 MB until it levelled off around
180 MB. Not a leak - repeated snips stayed flat - but memory nobody was using.

- Two seconds after a snip, the staging texture is released and the device is
  trimmed with `IDXGIDevice3::Trim`, the call Windows provides for an app going
  quiet; two seconds later it is trimmed once more, because the driver frees
  asynchronously and the first trim finds the texture still alive. The
  duplication stays warm, so the next grab is exactly as fast (~21 ms).
- A capture rebuild trims before it releases, so rebuilds no longer accumulate.
- After `IdleReleaseMinutes` (default 5) without a snip, the capture machinery
  and the overlay's borrowed device are let go entirely; the next snip rebuilds
  them, which costs about a tenth of a second once.

| | idle memory, 4K HDR |
|---|---|
| warm, after a snip, before | 115 MB, growing to ~180 MB with rebuilds |
| warm, after a snip, now | 53 MB - the same as before the first snip |
| after the long idle | 17 MB |

Measured on an HDR display; an SDR display needs a quarter of the staging memory.

## HDR

When any display has advanced colour on, the whole pipeline switches to
floating point: Desktop Duplication hands over `R16G16B16A16_FLOAT` via
`DuplicateOutput1`, the overlay's swap chain is float with
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`, and the snip is written as **JPEG XR**
(`64bppRGBAHalf`) — the only floating-point format the in-box WIC codecs can write
at all. Toggling HDR on or off rebuilds the capture machinery on the next snip.

Windows composes such a display in **scRGB**: linear, Rec.709 primaries, 1.0 = 80
nits by definition. SDR white does not sit at 1.0 but wherever the *SDR content
brightness* slider put it, so that level is read from `DISPLAYCONFIG_SDR_WHITE_LEVEL`
rather than assumed. Verified against a known test pattern on a 240-nit display:

| authored | captured | tone-mapped back |
|---|---|---|
| white 255 | scRGB 3.0000 | 255 |
| grey 128 | scRGB 0.6470 | 128 |
| black 0 | scRGB 0.0000 | 0 |

exactly `SrgbToLinear(128/255) × 3`, and 240 nits = scRGB 3.0 as the API said.

**The 8-bit copy forces a choice**, because keeping SDR white at 255 and
representing anything brighter are contradictory demands — once white is 255 there
are no code values left above it. The default **clips**: SDR content is bit-exact
and highlights become white, which is what someone pasting a screenshot expects,
and the `.jxr` beside it still holds the real values. `HdrSdrRollOff=1` instead
trades the top of the range for highlight detail (white lands near 231, and 1×, 2×
and 4× white become 231, 246 and 254).

**JPEG XR quality is not lossless by default.** Measured on a busy 3840×2160 frame,
lossless takes ~19 s and produces 24 MB; quality 90 takes ~2.5 s and 8 MB, with no
visible difference on screen content. `HdrJxrQuality=100` selects true lossless.

## Output

The PNG is encoded **once** and feeds both destinations; encoding a 4K screen
twice cost more than the capture itself. The clipboard gets the registered `PNG`
format plus `CF_DIBV5` and `CF_DIB`, so both modern and old Win32 apps find
something they understand. The PNG filter is pinned to `Sub` rather than WIC's
adaptive default, which on 8 megapixels dominated the whole snip for a few per
cent of file size.

`CF_DIB` has no alpha, so a freeform cut-out is composited **over white** for that
one format — otherwise anything that does not understand `CF_DIBV5` or `PNG` pastes
the shape on a black rectangle. The pixels are premultiplied, so that composite is
just adding back the part the shape does not cover.

**All of it runs on a worker thread.** That is not an optimisation; see the hotkey
note below.

## Recording

The toolbar's record button switches the overlay to picking an area to record
rather than to capture. Drag one out and recording starts: a red frame marks the
area and a small bar shows the elapsed time with a stop button. The separate full-screen
record button starts a whole-monitor recording immediately; the GIF button records the
selected area without audio and automatically stops at 15 seconds, then converts the
intermediate MP4 to an animated GIF using Media Foundation and WIC (10 fps, up to 960 px wide). **Win+Shift+S or
PrtScn also stops it** - while a recording runs, that is what the hotkey means.
The result is an H.264 + AAC **MP4 in `C:\Screenshots` by default**, or in the folder set under Settings → General → Videos. The GIF action produces a `.gif` in the same folder; if native GIF conversion fails, ScreenshotService retains and publishes the MP4 instead.

Verified by decoding a recording back: 960×540 at exactly 30.0 fps, 168 frames over
5.567 s, with the audio timeline matching the video to within a millisecond, and a
captured test tone coming back at 5838 against the 6000 it was played at.

- **Sound** can be disabled, captured from the speakers (WASAPI loopback of the
  default playback device), captured from the microphone, or recorded from both.
  Ordinary combined capture mixes both sources into 48 kHz stereo in real time.
- **Embed 3 tracks** (available with both sources selected) captures the two
  endpoint streams independently and defers the combined render until recording
  stops. It writes an AAC mix plus separate lossless FLAC tracks for microphone
  and system audio in the same MP4. The combined render is timestamp-aligned and
  uses fixed 50/50 source gain rather than a time-varying live limiter; the
  endpoint tracks retain the negotiated sample rate and channel count, and FLAC
  precision follows the configured endpoint PCM depth when that metadata is
  available. This is native C code and Windows APIs, without FFmpeg. Raw
  intermediate files are kept if encoding or muxing fails.
- **HEVC 10-bit SDR** is an explicit encoder choice (`hevc10sdr`). It requests
  Main 4:2:0 10-bit P010 with Rec.709 SDR tags written to the finalized MP4.
  Some vendor MFTs reject SDR colour tags during their probe even while their
  Main10 HDR path works, so the app also exposes the SDR profile against that
  same tested Main10 transform. QP 0 is attempted when supported, but this is
  not bit-exact desktop RGB: the video is still 4:2:0 YUV and chroma is subsampled.
- **Saving progress** stays visible in the recording bar after Stop while the
  MP4 is finalised, the offline AAC/FLAC tracks are encoded, and the tracks are
  muxed. The active container is named `.partial.mp4`; it is renamed to `.mp4`
  only after processing succeeds. An unhandled UI-thread exception triggers a
  best-effort stop/finalize, and failed/incomplete files plus RAW audio remain
  available instead of being deleted. Abrupt process termination, power loss,
  or a fault on the recording worker itself can still leave a partial container.
- **Startup recovery** scans the configured video folder for `.partial.*`
  artifacts. Interrupted `.partial.mp4` files are preserved byte-for-byte and
  are not opened, parsed, or decoded during startup; video recovery is deferred
  to an isolated recovery operation so a malformed MP4 cannot crash a decoder
  or prevent the recorder from opening. Interrupted PCM WAV headers are
  repaired; valid partial FLAC/GIF files are promoted; and RAW microphone/system
  PCM is wrapped in WAV when its persisted format metadata is available. New
  recordings persist this metadata before accepting RAW packets. Legacy RAW
  files created before that metadata existed are retained unchanged because
  their sample rate/channel layout cannot be inferred safely. Original RAW
  streams are kept under `.recovered.*.raw`.
- **Three-track recording:** enabled by default when both system sound and
  microphone are selected. It stores separate RAW microphone/system streams,
  creates an aligned AAC mix, and embeds microphone/system FLAC tracks after
  Stop. The setting remains user-configurable; upgrades preserve the user's
  saved choice and never silently disable multitrack recording.
- **Borderless fullscreen windows** are captured as part of the normal desktop
  through DXGI Desktop Duplication. If the display resolution/mode changes while
  recording, ScreenshotService stops and finalises at the original negotiated dimensions;
  the next recording opens a new duplication for the changed mode. Exclusive
  fullscreen behavior remains driver-dependent because DXGI may invalidate the
  duplication when the application takes exclusive control.
- **High-precision SDR stills** can be saved as JPEG XR when DXGI actually
  supplies a 10-bit `R10G10B10A2` desktop surface. Desktop Duplication does not
  expose a true 12-bit-per-channel SDR buffer: a display configured for a
  12-bit output signal does not guarantee that Windows' desktop capture surface
  is 12-bit. HDR stills continue to use the existing 16-bit-float scRGB JXR path.
  If the SDR capture surface is only BGRA8, ScreenshotService can still save JXR when that
  setting is enabled, but the source precision remains 8-bit; encoding cannot
  recover precision Windows did not expose.
- A loopback capture delivers *nothing at all* while the machine is silent - not
  zeroed packets, no packets. The video loop therefore tops the audio timeline up
  with real silence whenever it falls behind, or a recording of a quiet desktop
  would have no audio track and a recording whose sound paused would jump.
- **A loopback capture is a tap on the *processed* signal**, and on a device with
  enhancements switched on that is audible. Measured against a flat ten-tone
  reference played through one such endpoint, what came back was bent into a smile - `+17.9 dB` at 94 Hz, `+8.3` at 246,
  `+4.2` at 504, `0` at 996, `−1.9` at 2004, `+4.7` at 3996, `+9.8` at 8004,
  `+8.9` at 12000, `+10.4` at 15996, `+7.6` at 19922 - before the recorder ever
  saw it. Playing such a recording back through the same endpoint applies the
  curve a second time, which is what makes it sound damp. The codec is not at
  fault: content still reaches 19.9 kHz and nothing clips (peak `−2.3 dBFS`,
  zero samples at full scale).

  **`RecordWavSidecar`** writes the same PCM untouched to a `.wav` beside the
  `.mp4`. Media Foundation's AAC encoder stops at 192 kbps — measured, 96 / 128 /
  160 / 192 are the only rates it offers for 48 kHz stereo — and stream copy is
  not possible in principle here, because a loopback capture hands over decoded
  PCM and there is no source bitstream to remux. The sidecar is fed from the same
  place as the encoder, silence padding included, so it stays sample-aligned with
  the video. Measured on the same recording: the sidecar's tone levels match the
  raw loopback capture to the second decimal, while the AAC track drifts by up to
  0.05 dB and picks up a 9 ms gap from encoder priming that the WAV does not have.

  **`RecordMuxFromWav`** goes one step further: nothing is encoded to AAC while
  recording at all. The capture writes PCM to the WAV, the MP4 gets video only,
  and afterwards the WAV is encoded once and muxed in (`remux.c`, video copied
  through compressed — never re-encoded). This is not a quality trick and does not
  pretend to be: measured against its own sidecar the result matches within
  0.05 dB and still carries AAC's 9 ms priming gap, because it is the same encoder
  over the same samples. What it buys is that no part of the audio path has to keep
  up with anything while recording — no encoder running against the capture clock,
  no timeline to pad — so that whole class of live-timing fault cannot arise, and
  the WAV is the authoritative copy. It cost 15–47 ms on the recordings tested.

  With this on the `.wav` is an **intermediate and is deleted** once its sound is
  in the MP4. Keeping it looked tidy and was not: players that auto-load a
  same-named sidecar — MPC does — then offer the same audio twice, as two
  selectable tracks. If the mux fails the `.wav` is kept, because at that point
  it is the only place the sound still exists.

  Two Media Foundation defects had to be worked around to make it produce a valid
  file, both found by reading the bytes that came out:

  - A source reader's video media type reports the **coded** frame size, which for
    HEVC is padded up to whole coding units — a 720-line recording reads back as
    736. Handing that type to the sink writes 736 into `tkhd`, and players show
    sixteen rows of padding. The real size is in `MF_MT_MINIMUM_DISPLAY_APERTURE`.
  - An MP4 source offers its codec private data as
    `MF_MT_MPEG4_SAMPLE_DESCRIPTION` containing the sample *entry* (the `hvc1`
    box), while the MP4 sink writes that blob into the place the **`stsd` box**
    belongs. Pass it back unaltered and the file ends up with `hvc1` sitting
    directly inside `stbl` — a video track Media Foundation itself can no longer
    find. Wrapping the entry in an `stsd` box before handing it over is the fix.

  `RecordRawAudio` asks for `AUDCLNT_STREAMOPTIONS_RAW`, which bypasses the
  endpoint's effects on drivers that support it. This one does not - it answers
  `AUDCLNT_E_RAW_MODE_UNSUPPORTED` (`0x88890027`) and the two captures come back
  identical to the second decimal. On a device like that the only fix is outside
  this tool: turn the endpoint's enhancements off, or flatten its EQ preset. The
  log says which of the two happened on every recording.
- **Only the front pair is kept** when the endpoint's mix is multi-channel. That
  endpoint presents 7.1 (`8 ch`, mask `0x63F`), and a stereo source measured
  `−11.4 dBFS` in channels 0 and 1 with the other six at digital zero, so nothing
  is lost in practice; a genuine 5.1 source would lose its centre and surrounds.
  A correct fold-down needs the channel mask and matrix coefficients and is left
  out deliberately.
- **Plain `DuplicateOutput` is not a usable SDR capture on an HDR display.** It
  hands the scRGB desktop back as 8-bit *without* dividing by the SDR white
  level, so on a 240-nit white everything arrives three times too bright and
  everything above a third of white clips. Measured against a known ramp played
  back full screen: authored 32 came out at 10.5 nits instead of 3.5, and every
  bar from 160 upwards sat at 239.7 - flat white. That is why an 8-bit
  recording of an HDR desktop looked washed out.

  So the capture is **always floating point when the display is in HDR**,
  whatever gets written. An 8-bit recording is then tone-mapped on the GPU with
  the same curve the still-image path uses - divide by SDR white, optional
  highlight roll-off, sRGB encode. Re-measured the same way: every bar within
  one code value of where it should be, 0 to 255, and the primaries come back
  at exactly scRGB 3.0 in their own channel with no crosstalk.

- **Recording follows the display into HDR.** On a display in advanced colour the
  recorder asks `DuplicateOutput1` for scRGB half-float, converts it on the GPU to
  10-bit BT.2020 PQ (`hdrvideo.c`: one full-resolution pass for luma, one
  half-resolution pass that averages 2×2 chroma after the transfer function, as
  4:2:0 is defined to) and encodes P010 through HEVC Main10. Verified against PQ
  computed independently on the CPU — SDR white (80 nits) lands on luma code 489,
  400 nits on 635, 1000 nits on 722, 10000 nits on 940, every one within a code of
  the curve, with neutral chroma sitting exactly on 512.

  Before this, recording used plain `DuplicateOutput`, which on an HDR display
  hands over the desktop the compositor has *already* flattened to 8-bit. That is
  where the washed-out look came from; it was never the codec.
- **Video samples come from a pool.** A 4K frame is a 25–33 MB buffer, and
  allocating one per frame meant the system faulting in and zero-filling every
  page of it, every frame - over six thousand page faults per frame. Each pooled
  sample is a Media Foundation *tracked sample*: when the writer drops its last
  reference, it comes back to the pool instead of being destroyed, so a buffer is
  never reused while the encoder can still read it. The pool holds at most six;
  should all of them be in flight - the writer queues, because throttling is off
  so a slow encoder never stalls capture - a frame gets a one-off allocation as
  before. Measured at 3840×2160, 30 fps, interleaved against the unpooled build:

  | | per frame | CPU | page faults per frame |
  |---|---|---|---|
  | HDR 10-bit | 26.8 → 17.1 ms | 96 % → 62 % of a core | 6,100 → ~20 |
  | 8-bit | 21.8 → 10.2 ms | 256 % → 223 % | 8,150 → ~20–80 |

  The 8-bit recording was decoded back frame for frame to check that no buffer
  went out again too early. The pool now mostly serves the CPU path below.

- **Frames stay on the GPU** (`RecordZeroCopy`, on by default). The recorder's
  device is created video-capable and handed to the sink writer through a DXGI
  device manager, so the hardware encoder reads our textures in place. The
  captured frame is copied into a pooled texture on the GPU; on the 8-bit paths
  Media Foundation's video processor then converts it to NV12 on the GPU too -
  the RGB-to-NV12 conversion that used to run on the CPU in front of the
  encoder. On the HDR path our own PQ shaders render straight into the planes of
  a pooled P010 texture. MaxCLL and MaxFALL, which used to be measured on the
  CPU while the luma plane was copied out, now come from a compute-shader
  reduction whose results are read back a frame or two late, so measuring never
  stalls a frame. Media Foundation drives the device from its own threads, so
  every multi-call sequence of ours holds the device's multithread lock.

  Measured at 3840×2160, 30 fps, against the CPU path in the same run:

  | | GPU path | CPU path |
  |---|---|---|
  | HDR 10-bit | 0.08 ms/frame, 1 % CPU | 9.2 ms/frame, 35 % |
  | 8-bit, tone-mapped from HDR | 0.04 ms/frame, 3 % | 5.8 ms/frame, 205 % |
  | 8-bit, plain SDR capture | 0.02 ms/frame, 1 % | |

  (The per-frame figures are the recorder thread's own work; encoding runs on
  the GPU alongside.) Checked, not assumed: every file decodes frame for frame;
  a corner square painted in two known colours comes back within 8 levels in
  every frame on both paths, so the GPU converter uses the same matrix and range
  as the CPU one; and the HDR light statistics match the CPU's exactly (MaxCLL
  240 vs 240 nits, MaxFALL 2.24 vs 2.24). If the device cannot be made
  video-capable, or the encoder will not take textures, frame delivery falls back
  to CPU-fed input; the video encoder itself remains hardware-only. `RecordZeroCopy=0` forces that.

- **The encoder list is probed, not enumerated.** `MFTEnumEx` says what the driver
  advertises, which is not the same as what the MP4 sink will take: an
  `NVIDIA AV1 Encoder MFT` enumerates, configures and starts, then fails at
  `Finalize` with `MF_E_SINK_HEADERS_NOT_FOUND`, because the in-box MP4 sink
  cannot mux AV1 on many Windows configurations. So `encoder.c` builds the list
  by writing and finalising a one-frame file per candidate through the same
  configuration code recording uses. It now probes H.264 8-bit, HEVC 8-bit and
  Main10, plus AV1 8-bit, 10-bit SDR and 10-bit HDR. AV1 choices appear only if
  the installed Media Foundation encoder and MP4 sink both finalise a test file;
  on systems where AV1 muxing fails, they stay hidden rather than producing a
  broken recording. Results are cached under `HKLM\Software\Gallery Inc\ScreenshotSvc\EncoderCache` against the adapter and driver
  version, and re-probed when either changes.

  Hardware encoding uses `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`, plus a strict
  hardware-only enumeration via `MFT_ENUM_FLAG_HARDWARE`. For each codec probe,
  a hardware MFT is registered locally; after the Sink Writer begins, the selected
  encoder must expose the same hardware device URL as the activation object, and
  its vendor must match the preference. Auto mode uses the same rule: **a software
  encoder is never accepted**.
  The GPU preference list is populated from all physical DXGI adapters on the PC
  and shows each detected vendor's adapter model. Vendor-specific probing matches
  hardware vendor IDs as well as friendly names. If the driver does not expose a
  compatible hardware MFT, or the MP4 probe fails, that profile is unavailable and
  recording reports an encoder error rather than silently using software encoding.
  Results are cached against all adapter IDs and driver versions. This is the
  Media Foundation hardware-MFT path and remains the fallback when native vendor
  encoding is disabled, unavailable, or fails before its first compressed packet.
  The experimental FFmpeg/D3D12VA recording backend is removed; native NVIDIA
  NVENC, AMD AMF and modern Intel oneVPL paths use D3D11 textures instead. If no
  compatible hardware path works, recording fails explicitly. **There is no
  software-video fallback.**

- **"Best available" is not "newest".** Automatic prefers HEVC Main10 PQ for
  HDR displays, followed by AV1 Main10 HDR only when AV1 survives the full MP4
  probe. On SDR it prefers HEVC Main10 SDR, then HEVC 8-bit, then verified AV1
  profiles, and H.264 as the broad-compatibility fallback. Windows ships
  **no software HEVC encoder**, and some Windows 10 installations need the HEVC
  Video Extensions package to *play* HEVC back (otherwise playback can fail with
  `MF_E_TOPO_CODEC_NOT_FOUND`). `RecordEncoder` overrides the ranking; a choice
  that stops probing clean falls back down the list and says so in the log.

- **The container is stamped afterwards.** Media Foundation's MP4 sink writes no
  `colr` box, so nothing outside the HEVC bitstream said what the picture was, and
  a player trusting the container reads BT.2020 PQ as Rec.709 — the washed-out
  look again, by another route. `mp4tag.c` inserts the box after `Finalize`. That
  is only safe because the sink puts `moov` last, so growing it leaves every
  `stco` chunk offset pointing into the earlier `mdat` still correct; the code
  checks that layout holds and declines rather than corrupting a recording.

  `mdcv` and `clli` go in beside it. They are not decoration: handed HDR with
  no light-level metadata a player must assume the content might be graded for
  a very bright display and tone-maps defensively, which on a screen recording
  that never exceeds SDR white costs real brightness. MaxCLL and MaxFALL are
  **measured**, not guessed - the luma plane is already being copied out of the
  staging texture every frame, so the peak and the mean are taken during that
  copy while the data is still in cache. The mastering display comes from
  `DXGI_OUTPUT_DESC1`, which is the honest answer to what the content was made
  on.

  A finished HDR recording now reads: `hvcC` profile_idc 2 (Main 10), 4:2:0,
  luma and chroma 10-bit; `colr` primaries 9, transfer 16, matrix 9, limited
  range; `mdcv` with the panel's real primaries and 426 nits peak; `clli` with
  the measured MaxCLL and MaxFALL; `tkhd` and `hvc1` both the requested size.
  The HEVC VUI agrees with all of it - `video_full_range_flag` 0, primaries 9,
  transfer 16, matrix 9 - which was checked by parsing the SPS, because
  decoders believe the bitstream over the container.

- The region is **clipped to one monitor** - the one containing its centre.
  Recording a composite of several outputs would need one duplication each and a
  shared clock between them.
- Starting a recording releases the still-capture machinery, because DXGI will not
  hand a second duplication for the same output to the same process; it fails the
  second one with `E_INVALIDARG`. It is re-created when the recording ends.

## How the hotkeys work

`Win+Shift+S` belongs to the shell, which hands it to the `Microsoft.ScreenSketch`
package. `RegisterHotKey` cannot claim it while that package is installed, so
ScreenshotService installs a `WH_KEYBOARD_LL` hook instead. Low-level hooks run
most-recently-installed first, so ours sees the chord before the shell's does and
swallows it. **Nothing is uninstalled and nothing is permanent** — quit the program
and Snip & Sketch has its shortcut back.

Two details that matter:

- Swallowing the `S` would leave the shell thinking the Windows key was tapped alone,
  which opens the Start menu on release. The hook injects a reserved `VK_NONAME`
  keystroke so the chord counts as consumed.
- The hook does **not** filter on `LLKHF_INJECTED`. Remote-desktop hosts deliver every
  real keypress as injected, and the hotkey has to keep working over CRD/RDP. Instead,
  our own filler keystroke carries a tag in `dwExtraInfo` and only that is skipped.

`Alt+PrtScn` starts full-monitor video recording. The monitor picker appears when multiple displays are connected; with one display the choice is implicit. The optional force-last setting reuses the last selected physical display while it remains connected and opens the picker if that display disappears.

The hook cannot see keys while an elevated window has focus. That is a Windows
restriction on non-elevated hooks, not something the program can work around.

**A low-level hook is dispatched on the thread that installed it**, so that thread
must never be busy. Getting that wrong once cost the hotkey outright: a 4K HDR
snip spent seconds in the encoder on the message loop, the hook could not answer
within `LowLevelHooksTimeout`, and Windows passed Win+Shift+S to the shell — which
opened Snip & Sketch, the one thing this program exists to prevent. The hook now
lives on a **thread of its own** that does nothing else, at raised priority, so
nothing the UI does - a cold capture, a message box, a modal dialog - can delay
its answer. Saving still runs on worker threads (`Save_SnipAsync`), and the hook
is still reinstalled every 10 seconds as a safety net, since there is no way to
ask whether it is still alive.

## Why not Windows.Graphics.Capture

WGC is what draws the yellow "your screen is being captured" border, and that
border occasionally stays on screen after the capture has ended. ScreenshotService uses
DXGI Desktop Duplication, which has no such overlay.

## Building

```
.\build.bat
```

MSVC 2022 Build Tools, x64, `/W4 /WX`. Falls back to MinGW-w64 if MSVC is not present.

```
tests\build.bat
```

builds and runs `tests\rectest.c`, which includes `record.c`, `overlay.c` and
`poly.c` directly so it can check their own state:

- the duplication rebuild (same device, staging texture still valid);
- the recorder's start/stop lifecycle, including the stop timeout, and a
  recording end to end;
- the remux failure path and the wait for pending encodes at exit;
- the freeform rasteriser against the original implementation, and its speed;
- that the overlay borrows the capture device and follows it when capture is
  rebuilt, and what that saves;
- that snips taken in the same second all keep their own files;
- that a path-like language tag is refused.

`rectest.exe b` runs only the recording benchmark instead: full-screen
recordings on the GPU path and on the CPU path for reference - HDR, 8-bit
tone-mapped and plain SDR capture - reporting per-frame cost, CPU use and page
faults. The 8-bit files are decoded back to check every frame is there and that
a square painted in two known colours survives the colour conversion; the HDR
runs check the GPU's light statistics against the CPU's. They capture the real
desktop, so they are deleted afterwards.

`rectest.exe g` checks that the zero-copy path's prerequisites exist on the
machine: a video-capable device, render targets on the planes of P010 and NV12
textures, and encoders that take GPU textures and still finalise a file.

It needs a desktop that is awake - a sleeping display hands Desktop Duplication
no frames at all, and the recording check then fails for want of anything to
encode.

## Language

English, German, Romanian, Spanish and Hungarian ship; the interface follows the Windows display language by
default. Everything visible lives in `langs\` beside the exe, one file per
language, and English is *also* compiled in as a last-resort fallback so a
missing or damaged folder can never leave the window blank.

```
ScreenshotService.exe
langs\
  en.ini      the template - copy it to add a language
  de.ini
  ro.ini
  es.ini
  hu.ini
```

**Adding one needs no rebuild and no restart.** Copy `en.ini` to `fr.ini`,
translate the text after each `=`, and the language appears in Settings by
itself, named the way Windows names it ("Français", not "French"). Choosing it
switches the open window there and then, because being asked to accept a
language you cannot read yet is a poor way to choose one. `%APPDATA%\ScreenshotService\langs\`
is searched first, so a shipped translation can be replaced where the program
folder is read-only.

Four decisions worth knowing about:

- **The fallback is per string, not per file.** A key that is missing, empty or
  misspelt falls back to English on its own, so a half-finished translation is
  usable from the first line. Verified with a deliberately broken `fr.ini`:
  6 of 79 keys translated, one unknown key, one malformed line and a run of
  invalid UTF-8 — the six appeared in French, everything else in English, the
  bad bytes rendered as replacement glyphs, and nothing else was affected.
- **The language files are parsed here, not by `GetPrivateProfileString`.**
  The language files are UTF-8 with or without a BOM; parsing them directly
  prevents translated umlauts from being corrupted by the system code page.
- **`%1`, `%2` are substituted positionally**, so a translation may reorder
  them — the German `TOAST_RECORDED` does exactly that. This is deliberately
  not `FormatMessage`, which treats `%n` as a hard line break and would let a
  user-supplied file break the formatter rather than merely look wrong.
- **Product vocabulary stays English in every language**: ScreenshotService,
  snip, HDR, PrtScn, Win+Shift+S, the file extensions, the codec names.
  Somebody looking for `.jxr` should find `.jxr`.

Log lines are diagnostics rather than interface and are always English.

## Settings

**Tray icon → Settings…** covers all of it. The dialog follows the Windows
light/dark setting, shows whether advanced colour is actually on and what SDR white
is currently at, and greys out the options that depend on one another rather than
letting them be set to combinations that do nothing.

It is split into five tabs — **General · HDR · Recording · Hotkeys · Advanced** —
switched by click, the arrow keys on the strip, or `Ctrl+Tab` / `Ctrl+Shift+Tab`
(`Ctrl+PgDn` / `Ctrl+PgUp`) from anywhere in the dialog, and it reopens on the tab
it was closed on. The strip is drawn by the app rather than being a stock tab
control, which has no dark mode and paints its page in a colour that does not
match the dialog.

**Apply** saves and takes effect immediately without closing, and is only enabled
while there is something to apply. **Cancel** then undoes only what changed since
the last Apply. Choosing a language switches the dialog at once so you can read
what you are accepting; Cancel switches it back.

It opens **centred on whichever monitor the pointer is on**, and if you drag it
somewhere else it opens there next time — including after Cancel, since where a
window sits is not one of the settings being edited. A remembered position is only
honoured while it still lands on a monitor; unplug that display and the dialog goes
back to being centred rather than opening somewhere you cannot reach it. Leaving it
where it is keeps it following the centre as the desktop changes.

Settings are stored in `HKLM\Software\Gallery Inc\ScreenshotSvc`; encoder
capabilities are cached in its `EncoderCache` subkey. On first run after this
change, ScreenshotService imports settings once from the old
`%APPDATA%\ScreenshotService\settings.ini` (if present) and does not write
that file again. Run `setup-registry-settings.bat` as Administrator once so
standard users can update the machine-wide key without running the app elevated.

| Key | Default | Meaning |
|---|---|---|
| `CopyToClipboard` | 1 | put every snip on the clipboard |
| `SaveToDisk` | 1 | also write a file |
| `SaveFolder` | *(empty)* | empty means `C:\Screenshots` |
| `VideoFolder` | *(empty)* | where recordings go; empty means `C:\Screenshots`. Recordings are always saved, so this does not depend on `SaveToDisk` |
| `HookWinShiftS` | 1 | take over `Win+Shift+S` |
| `HookPrintScreen` | 1 | take over `PrtScn` |
| `PrtScnFullscreen` | 0 | `PrtScn` grabs everything without showing the overlay |
| `RecordFullscreen` | 0 | Video recordings capture the entire monitor that contains the selected region |
| `RecordVsync` | 1 | Pace recording frames to the selected monitor's reported refresh rate |
| `DefaultMode` | 0 | 0 rectangle, 1 freeform, 2 window, 3 fullscreen |
| `DimPercent` | 40 | how far the overlay dims the desktop |
| `ShowToast` | 1 | tray balloon naming the saved file |
| `HdrSaveJxr` | 1 | HDR snips go to `.jxr` |
| `SdrSaveJxr` | 1 | save SDR stills as JXR; preserve packed DXGI 10-bit data when available, otherwise wrap the captured BGRA8 pixels in JXR |
| `HdrSdrSidecar` | 1 | …plus an 8-bit `.png` copy beside any saved JXR (tone-mapped for HDR) |
| `HdrSdrRollOff` | 0 | keep highlight detail in the 8-bit copy, at the cost of white landing near 231 |
| `HdrJxrQuality` | 90 | 1–100; 100 is true lossless and much slower |
| `ForceSdr` | 0 | ignore advanced colour and capture 8-bit |
| `IdleReleaseMinutes` | 5 | minutes without a snip before the capture machinery is released entirely (0 = keep it warm); the next snip then takes about 0.1 s longer, once |
| `RecordAudio` | 1 | 0 none, 1 system sound, 2 microphone, 3 system sound + microphone |
| `RecordFps` | 30 | 5–240 |
| `RecordRawAudio` | 1 | ask the endpoint to bypass its effects; many drivers refuse |
| `RecordEncoder` | `auto` | `auto`, `h264`, `hevc8`, `hevc10`, `hevc10sdr`, `av1_8`, `av1_10` or `av1_10_hdr`; only probed hardware profiles are offered |
| `RecordHdr` | 1 | capture 10-bit HDR when the display is in it |
| `RecordWavSidecar` | 0 | also write the untouched PCM as a `.wav` beside the `.mp4` |
| `RecordMuxFromWav` | 0 | with the above: no live AAC; build the `.mp4`'s sound from the `.wav` afterwards |
| `RecordMultiTrack` | 0 | with both sources selected: combined AAC + microphone FLAC + system FLAC, rendered after recording stops |
| `RecordZeroCopy` | 1 | frames stay on the GPU into the encoder; 0 forces the CPU path, for a driver that misbehaves |
| `MonitorForceLast` | 0 | 0 asks which display to record when multiple displays are connected; a single display is implicit. 1 forces the last selected physical display and asks again if it is disconnected |
| `MonitorTargetPath`, `MonitorDeviceName` | *(empty)* | stable physical display identity for forced selection; the GDI name is diagnostic only, and the picker reappears if no stable identity can be matched |
| `SettingsX`, `SettingsY` | *unset* | where the settings window was dragged; unset means centred |
| `Language` | `auto` | `auto` follows Windows; otherwise a tag with a `langs\<tag>.ini` |
| `ForceGdi` | 0 | skip Desktop Duplication and always use GDI |
| `DebugLog` | 0 | append diagnostics to `ScreenshotService.log` beside this file; at 4 MB it is moved to `ScreenshotService.old.log` and started afresh |

`DebugLog` exists because everything interesting here happens with no window on
screen: a hotkey that never arrived and a capture that failed look identical from
the outside. With it on, **F12 inside the overlay** writes the swap chain's back
buffer to `overlay-debug.png` in the screenshots folder — a flip-model swap chain
is invisible to every ordinary screen-capture route, `BitBlt` and `PrintWindow`
both returning the desktop underneath it, so that dump is the only way to look at
what the overlay actually rendered.

## A note on desktops

The hook, and capture, live on the `Default` desktop. When the screensaver or the
lock screen takes over, input goes to a different desktop entirely — the hotkeys
do nothing there and a GDI screen DC is invalid. That is Windows working as
designed, not a fault, and it is worth remembering while testing.

## Optional: remove the Snipping Tool

Not required — the hook works with it installed. If you would rather it were gone:

```bash
powershell.exe -NoProfile -Command "Get-AppxPackage Microsoft.ScreenSketch | Remove-AppxPackage"
```

Reinstallable from the Microsoft Store.


### Multi-track recording

When **Embed 3 tracks (AAC mix + mic/system FLAC)** is enabled with **System sound + Microphone**, ScreenshotService captures each endpoint to temporary RAW files. After Stop, it renders the combined 48 kHz stereo mix and encodes it to AAC, encodes the separate microphone and system sources to FLAC concurrently using each endpoint's negotiated channel/rate and selected PCM precision, then adds all three labelled tracks to the MP4 using the native muxer. The offline mixer skips resampling when an endpoint already matches the 48 kHz output rate, and large sequential file operations use Windows sequential-I/O hints. This post-processing avoids live mixing and encoding work on the capture threads. No FFmpeg dependency is used. The microphone has slightly lower gain in the combined mix to give loud speech more headroom; the separate FLAC source is not altered.


## Current implementation boundaries

- When Desktop Duplication is invalidated by a resolution change, the capture is reacquired and each new frame is scaled to the original encoded canvas. Changing colour space from an HDR floating-point source to a different SDR mode is not silently reinterpreted as HDR; that transition may require finalising the current recording.
- PrintScreen opens the still-image editor with a red pen, rectangle, circle, text, pipette/colour picker, and a feathered clone/heal eraser. These edits are burned into the still image. During video, the Draw control provides a red freehand pen and a cursor overlay; screenshot-only text/shapes/heal operations are not yet available as live-video tools.
- HEVC Main10 SDR is exposed separately from HDR and no longer depends on QP 0 being
  supported during probing. QP 0, when a transform exposes it, is not equivalent to
  uncompressed RGB because the current path encodes 4:2:0 YUV. HEVC 4:2:2 is not yet
  enabled; it requires runtime profile detection and a matching P210/P216 input path.
- This source archive has not been compiled with MSVC in this environment. The ZIP
  integrity is checked, but the Windows build and device-level tests must be run on
  Windows.


### Preview drawing and live video pen

In the screenshot selection preview, use the pen and heal-eraser toolbar icons before dragging the crop. During video recording, `Ctrl+right-click` toggles the live pen on/off and `Ctrl+Alt+right-click` clears all live pen marks. These gestures are handled globally so they also work when the pen overlay is click-through.


## Native hardware encoders and recording smoke test

Recording can use the selected graphics adapter's native hardware API when the
adapter and driver expose the requested profile: NVIDIA NVENC, AMD AMF, or Intel
oneVPL. Media Foundation hardware MFT remains the fallback. If a native encoder
fails before it emits its first usable packet, ScreenshotService removes that
empty attempt and retries the same recording through the hardware MFT; a failure
after packets have been written is preserved as a partial recording for diagnosis.

The Intel oneVPL path in this code targets the modern `libmfx64-gen.dll` API 2.x
runtime for Arc/Xe GPUs. Older Intel HD GPUs are not wired to a separate legacy
Media SDK implementation here; they continue through Media Foundation if a
compatible hardware MFT is available.

For an interactive Windows desktop, a short end-to-end recording test can be run
from PowerShell after building:

```powershell
.\tests\record-smoke-test.ps1 -ExePath .\ScreenshotService.exe -Seconds 4
```

It launches a separate smoke-test mode, records the primary display without
audio, stops automatically, and verifies the final MP4's top-level `ftyp`, `moov`
and non-empty `mdat` boxes. `-Backend MFT` forces the Media Foundation path;
`-Backend Native` requires the native vendor encoder to have actually been used;
`-Backend Auto` uses the configured preference and allows a pre-first-packet
fallback to MFT. When `ffprobe.exe` or `ffmpeg.exe` is available in `%WINDIR%`,
the script also checks stream metadata or decodes the result. Close the tray
instance first; the test intentionally fails rather than interrupting an
already-running instance. It requires a logged-in, unlocked desktop session and
cannot run reliably as a Windows service or in Session 0.
