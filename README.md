# Nitshot

**Version 0.8.6.8**
Copyright © 2026 Rekow IT · Licensed under the [GNU GPL, version 3](LICENSE)

A small native Win32 replacement for the Windows 10 Snipping Tool: a tray-resident
C17 program with no runtime dependencies that takes over `Win+Shift+S` and `PrtScn`,
freezes and dims the desktop, and puts the result on the clipboard *and* in
`Pictures\Nitshot`. On an HDR display it captures in floating point and
saves real HDR files.

**Formerly BetterSnippingTool.** Renamed before the first release. On first start,
Nitshot moves `%APPDATA%\BetterSnippingTool` to `%APPDATA%\Nitshot`, carries the
settings over from the old INI section and re-registers autostart under the new name.
Snips already saved in `Pictures\BetterSnippingTool` stay where they are; new ones go
to `Pictures\Nitshot`.

**User guide:** [English](docs/UserGuide.en.md) · [Deutsch](docs/Benutzerhandbuch.de.md). This README is the technical reference; what changed in each version is in the [changelog](CHANGELOG.md).

## What it does

| | |
|---|---|
| Capture | rectangle, freeform, window, full screen — over a frozen, dimmed desktop |
| HDR stills | fp16 capture, JPEG XR, tone-mapped PNG beside it |
| Recording | MP4 with the sound of any playback or recording device, optional lossless WAV |
| HDR recording | 10-bit BT.2020 PQ, HEVC Main10, encoder chosen by probing |
| Destinations | clipboard and disk in one step |
| Interface | tray icon, settings window, light/dark, English and German |

Press the hotkey, the desktop freezes and dims, and the toolbar offers
**rectangle · freeform · window · fullscreen · record · close**. On a display with
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
  right way round. `ForceGdi=1` in the INI pins this path for testing.

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
area and a small bar shows the elapsed time with a stop button. **Win+Shift+S or
PrtScn also stops it** - while a recording runs, that is what the hotkey means.
The result is an H.264 + AAC **MP4 in `Videos\Nitshot`**, or in the folder set under Settings → General → Videos.

Verified by decoding a recording back: 960×540 at exactly 30.0 fps, 168 frames over
5.567 s, with the audio timeline matching the video to within a millisecond, and a
captured test tone coming back at 5838 against the 6000 it was played at.

- **Sound** comes from any active endpoint: a playback device is recorded as what
  it plays (a WASAPI loopback of that endpoint), a recording device directly. Two
  entries follow whatever Windows currently has as its default playback and
  recording device. The INI stores the endpoint id, so the choice survives the
  device being renamed or moved to another USB port; a device that is unplugged
  when a recording starts is skipped, and the notification says so.
- **A second source** can be recorded alongside, either as a **second AAC track**
  in the MP4 or **mixed with the first into one track**. Each source runs on its
  own device clock, so each keeps its own timeline - counted in the samples it
  delivered - and its own silence padding; the two never share a timestamp
  base, and a source that pauses does not shift the other. The two tracks of a
  4-second test recording came out 3.78 and 3.80 s against 3.80 s of video.
- **Mixing** sums each source into a 4-second ring at *its own* timeline
  position, and hands the mix on only up to where both have arrived. Summing
  packets as they come would put one device's sound against the wrong instant
  of the other's, since the two deliver at their own moments in packets of
  their own size. A source that goes quiet is padded like any other, so the mix
  never waits on it for long; one that stalls a whole ring behind has its share
  of the oldest stretch dropped rather than holding everything up. The sum is
  clamped, not halved - attenuating both would put a voice over quiet game
  sound at half its level to guard against a clip that needs both loud at
  once. `rectest a` checks the mixer sample by sample: out-of-step packets,
  a padded stretch, and a clipping pair.
- Each audio track is **named after its device** - in `mdia/hdlr` (what
  ffmpeg, LAV and MPC show) and in `udta/name` (what VLC shows) - so a player's
  track menu says *Mikrofon (EPOS GSX 300)*, or *A + B* for a mix. Written the
  same way as the colour boxes: only into a `moov` that is the file's last box.
- Every stream is opened at **48 kHz stereo float with
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`**, so the audio engine resamples and folds
  the channels down - a 96 kHz interface, a 5.1 headset and a mono microphone all
  arrive in the same format, which is what AAC wants and what lets tracks sit
  side by side. Only if the engine refuses is the endpoint's own mix format used,
  as before, and then only at 44.1 or 48 kHz. Checked on six devices with
  `rectest a`: all delivered 48 kHz stereo, within 0.6 % of nominal over 2 s.
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

  With two sources the sidecars are `<video>.track1.wav` and `.track2.wav`, one
  per source, plus `.track0.wav` holding the mix when the two are mixed; the
  MP4's tracks are then built from track1/track2, or from track0.

  With this on the `.wav` is an **intermediate and is deleted** once its sound is
  in the MP4 - with two sources, all of them, unless the two were mixed and
  `RecordKeepSourceWavs` asks for each source's own WAV to stay (the mix's
  `.track0.wav` goes either way: its sound is the MP4's). Keeping it looked tidy and was not: players that auto-load a
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
  video-capable, or the encoder will not take textures, the recording falls back
  to the CPU path by itself; `RecordZeroCopy=0` forces that.

- **The encoder list is probed, not enumerated.** `MFTEnumEx` says what the driver
  advertises, which is not the same as what the MP4 sink will take: an
  `NVIDIA AV1 Encoder MFT` enumerates, configures and starts, then fails at
  `Finalize` with `MF_E_SINK_HEADERS_NOT_FOUND`, because the in-box MP4 sink
  cannot mux AV1. So `encoder.c` builds the list by writing and finalising a
  one-frame file per candidate through the very same configuration code
  recording uses. On a current NVIDIA card that yields H.264 8-bit, HEVC 8-bit
  and HEVC Main10, all on hardware, in 454 ms — cached in the INI against the
  adapter and its driver version, and re-probed only when either changes.

- **The probe reads its file back.** Finalising proves that *a* file came out,
  not that it is the one asked for: NVIDIA's HEVC encoder accepts
  `eAVEncH265VProfile_Main_420_12` without complaint, takes P016 or fp16 input,
  and writes **8-bit Main**. So each probe file's codec configuration record is
  read - `hvcC` profile and `bitDepthLumaMinus8`, or the `avcC` profile - and an
  encoder whose file is not the depth and profile requested is not offered.
  Nothing on this machine fails it today; it is there for the Intel and AMD
  encoders this has not been run on, and it is the check any 12-bit mode would
  have to pass. `rectest e` checks the parser, the Main 12 request (it comes
  back profile 1, 8-bit) and a real HDR recording (profile 2, 10-bit).

  **12-bit, for the record:** the capture is fp16, so the precision exists, but
  no Media Foundation encoder here writes 12-bit HEVC or AV1, Windows ships no
  software HEVC encoder, and a 10-bit panel (`IDXGIOutput6::GetDesc1` reports
  `BitsPerColor` 10 here) could not show the difference. Not offered.

- **H.264 is asked for the High profile.** Without a profile on the output type
  the NVIDIA encoder writes Baseline, which has neither CABAC nor the 8×8
  transform and so spends more bits on the same picture. The probe reads the
  profile back too, and logs it if an encoder answers with another - still
  8-bit, still playable, so that alone does not drop it.

- **No B-frames on NVIDIA, and none to be had through Media Foundation.** The
  NVIDIA H.264 and HEVC MFTs answer `ICodecAPI::IsSupported` for
  `CODECAPI_AVEncMPVDefaultBPictureCount` with `E_NOTIMPL` and `SetValue` with
  `E_INVALIDARG` - while rate control, GOP size and low latency are supported,
  so the interface itself works - and the same value passed as encoding
  parameters to `SetInputMediaType` is accepted and ignored: byte-identical
  files, all P-frames. Only NVENC's own API gets them - see below. Microsoft's
  software H.264 encoder writes B-frames by default (42 of 90 frames), and
  other vendors' encoders may too, so the remux path is tested with them:
  `rectest` copies a B-frame video through and requires every sample, byte and
  timestamp to come out identical (60 samples, 29 stored out of display order).

- **On NVIDIA, NVENC encodes directly** (`RecordDirectEncoder`, on by
  default; `nvenc.c` behind `hwenc.h`, which the AMD path shares - see below). The driver's `nvEncodeAPI64.dll` is loaded from System32 at run
  time - nothing is linked - and needs API 12.1, drivers from 531 on. NVENC
  gets the frames where the zero-copy path has them: the BGRA capture or the
  tone-mapped BGRA of an HDR desktop (NVENC converts both as BT.709, measured:
  2.6 levels off over four colour patches, against 64 for BT.601), or P010
  rendered into a texture of its own. Each frame is copied on the GPU into a
  texture NVENC owns, because with B-frames and lookahead it holds its inputs
  for a while. The MP4 writer then only muxes: its video stream's input and
  output type are the same compressed type, carrying NVENC's parameter sets.

  Settings: preset P5 of P1..P7 with high-quality tuning
  (`RecordEncoderPreset`), 2 B-frames for H.264 and 3 for HEVC, the HEVC ones
  usable as references (`RecordEncoderBFrames`, -1 = that default), a 16-frame
  lookahead (`RecordEncoderLookahead`), VBR at the same average bitrate the
  Media Foundation encoder gets with twice that as the ceiling, and a key
  frame every two seconds. Spatial adaptive quantisation is off
  (`RecordEncoderAq`): it moves bits from busy areas to flat ones, and on a
  screen the busy areas are the text - see the measurement below. HDR10 streams describe themselves as
  BT.2020 / PQ / BT.2020 NCL, and each key frame carries a mastering-display
  SEI; the light levels, known only at the end, stay in the container.

  Where NVENC is missing, too old, on another adapter than the desktop (a
  laptop running it on the integrated GPU) or refuses the configuration, the
  recording goes through Media Foundation as before. If it stops mid-way, the
  recording ends there and is kept, and the notification says so - a file's
  encoder cannot be changed half-way through.

  Two things the API leaves unsaid shaped `nvenc.c`. With lookahead, a
  successful submission does not mean everything handed in is done: locking a
  picture still held fails with `NV_ENC_ERR_INVALID_PARAM`, so pictures are
  taken out only once they are more than B-frames + lookahead back. And the
  list calls want count-then-fetch: asked for more than it has, the driver
  answered with all-zero GUIDs.

- **Timestamps with B-frames.** A B-frame is decoded after the anchor it
  depends on but shown before it, so with presentation time = capture time
  the first few decode times would have to be negative - and Media
  Foundation's MP4 sink silently drops samples with a negative decode time;
  it dropped the first key frame, and nothing up to the next one decoded.
  Ramping the decode times up from zero in fractions of a frame kept them,
  but made ffmpeg see a 120 fps stream and write every frame four times. So,
  as Microsoft's own B-frame encoder does, every picture is presented later
  by the reorder depth - 66.7 ms with 2 B-frames at 30 fps, 100 ms with 3 -
  with every sample the same duration. Unlike Microsoft's encoder, which
  leaves the sound a frame early, the recorder moves the sound by exactly
  the same amount, live and when the sound is rebuilt from the WAV. Measured
  on real recordings: picture and sound both start at 66.7 ms (H.264) and at
  100.0 ms (HEVC).

  `rectest n` records through the real recorder on every NVENC path - H.264
  from an HDR desktop tone-mapped and from plain SDR capture, with the sound
  rebuilt from the WAV, HEVC 8-bit, and HEVC Main10 from an HDR desktop -
  and on the last checks the whole of the HDR signalling: Main10 10-bit with
  B-frames, PQ in the stream, the container's mastering-display and
  light-level boxes still written after NVENC's own, and one mastering SEI
  per key frame.

- **On AMD, AMF encodes directly** (`amfenc.c`, the same setting). The
  driver's `amfrt64.dll` is loaded from System32 at run time and needs AMF
  1.4.35 (headers in `third_party/amf`). The recorder picks the vendor by the
  PCI vendor id of the adapter its capture device is on - 10DE NVIDIA, 1002
  AMD, 8086 Intel - and everything else stays on Media Foundation.

  **Not tested on AMD hardware** - there was none to test on. It follows
  AMD's documentation and what ffmpeg and OBS do: the usage set first (it
  resets every other property), quality preset and profile, the static
  properties - frame size, rate, B-frames, IDR period, colour description -
  before `Init`, bitrates after. What the chip offers is asked for, not
  assumed: B-frames only for H.264 and only where the encoder reports them
  (`BFrames` capability; SDK 1.4.35 has none for HEVC), pre-analysis as the
  lookahead where reported, HEVC Main10 where the maximum profile allows.
  Rate control is peak-constrained VBR like NVENC's; VBAQ and pre-analysis'
  perceptual AQ stay off for the same reason AQ does there. Presets 1-3 map
  to AMF's speed, 4-5 to balanced, 6-7 to quality. Each frame is copied on
  the GPU into a surface from AMF's own pool, so nothing of the recorder's is
  held while the encoder works. The parameter sets come from the encoder's
  extradata, unpacked to Annex-B if it is a configuration record (tested in
  `rectest n` without hardware). HDR10 hands AMF the mastering display as
  `AMFHDRMetadata`; the light levels stay in the container.

  Since a user with an AMD card is the first to run this, it says a lot in
  the diagnostic log: the runtime version, each capability, every property
  set with AMF's result text where it was refused, `Init`, the extradata,
  and AMF's own warnings and errors, routed into the log through an AMF trace
  writer. A refusal of anything that matters - usage, profile, bit depth,
  `Init`, the parameter sets - leaves the recording on Media Foundation; any
  other refused property is logged and lived with.

- **On Intel Arc and Xe, oneVPL encodes directly** (`qsvenc.c`, the same
  setting). The GPU runtime, `libmfx64-gen.dll` with API 2.x, is not in
  System32 but in the driver's DriverStore folder: it is found through the
  `DriverStorePathForVPL` value of the driver key of the Intel display
  adapter with the capture device's PCI device id - what Intel's dispatcher
  reads - and loaded only if that folder is under `System32\DriverStore`
  (`rectest n` checks that a path climbing out with `..` is refused). Older
  Intel GPUs have only the legacy Media SDK runtime and stay on Media
  Foundation. Headers: oneVPL 2.13 in `third_party/vpl`.

  **Not tested on Intel hardware** either. It follows Intel's samples and
  what OBS does with Quick Sync: a session on the hardware implementation
  whose device id matches (every implementation the runtime offers is
  logged), the recorder's D3D11 device handed over, and a **frame allocator
  of Nitshot's own** - D3D11 texture arrays for decoder-target requests,
  single textures for processor and encoder targets, staging buffers for P8
  bitstream "frames" - so Nitshot always knows which texture and slice each
  surface is and copies each frame there on the GPU. oneVPL's D3D11 surface
  import would be simpler and is still experimental in 2.17, so it is not
  used; the internal surfaces' `GetNativeHandle` gives a texture but no slice.
  The configuration is queried first and every field the runtime changed is
  logged, then initialised, then read back: the B-frame delay is taken from
  what the encoder actually uses. Target usage 1-7 is the reverse of the
  preset; VBR, a 2 s closed GOP, B-pyramid, `LookAheadDepth`, MBBRC (Intel's
  AQ) off, the BT.709 or BT.2020/PQ colour description, and for HDR10 the
  mastering display in Annex D order. Intel's 90 kHz stamps are mapped back to
  the exact input times rather than converted.

- **BGRA to NV12 on the GPU** (`vpconv.c`), for encoders that do not take RGB
  - Intel's, depending on the generation: the D3D11 video processor, which
  every driver has, with the DXGI colour spaces set to full-range RGB in and
  BT.709 limited range out, and the driver's own processing switched off.
  Measured here: six colour patches within one level of BT.709 (2.6 over all
  of them), against 64 for BT.601.

- **Measured against Media Foundation** (`rectest q`): 150 frames of
  screen-like 1280x720 content - a page of text scrolling, a window dragged
  across it, a small grainy video, a smooth background - fed as identical NV12
  to both encoders at the same average bitrate, Nitshot's default and half of
  it, and compared with the source by ffmpeg's SSIM and PSNR on luma:

  | Codec, bitrate | Encoder | Size | SSIM-Y | PSNR-Y |
  |---|---|---|---|---|
  | H.264, 3317 kbit/s | Media Foundation | 1995 KB | 0.9814 | 38.56 dB |
  | | **NVENC** | 2007 KB | **0.9837** | **39.05 dB** |
  | | NVENC with AQ | 2032 KB | 0.9852 | 37.52 dB |
  | H.264, 1658 kbit/s | Media Foundation | 967 KB | 0.9692 | 33.44 dB |
  | | **NVENC** | 919 KB | **0.9741** | **35.69 dB** |
  | | NVENC with AQ | 932 KB | 0.9735 | 33.83 dB |
  | HEVC, 2156 kbit/s | Media Foundation | 1239 KB | 0.9738 | 37.29 dB |
  | | **NVENC** | 1179 KB | **0.9776** | **38.64 dB** |
  | | NVENC with AQ | 1171 KB | 0.9780 | 37.54 dB |
  | HEVC, 1078 kbit/s | Media Foundation | 665 KB | 0.9517 | 29.46 dB |
  | | **NVENC** | 595 KB | **0.9665** | **33.26 dB** |
  | | NVENC with AQ | 570 KB | 0.9645 | 31.39 dB |

  NVENC as configured is better on both measures in every case, by 0.5 to
  3.8 dB, and the files come out the same size or up to a tenth smaller; the
  gain grows as the bitrate falls, which is where B-frames and lookahead
  pay. AQ buys a little SSIM at the higher rates and costs 1.5 to 2 dB
  everywhere, so it stays off. Encoder CPU time stays under a millisecond a
  frame for both; NVENC's call takes 0.6 to 1.2 ms of the 33 ms frame, since
  it waits for each picture where Media Foundation encodes on threads of its
  own.

  Hardware encoding was already in use before any of this:
  `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` makes Media Foundation route to
  whichever vendor MFT is installed — NVIDIA's, Intel's or AMD's.
  Coding against the NVENC, AMF and Quick Sync SDKs directly would mean three
  vendor SDKs to land exactly where `MFTEnumEx` already lands.

- **"Best available" is not "newest".** On an HDR display HEVC wins outright,
  being the only way to carry HDR at all. On an SDR one H.264 is preferred
  even though it is older and larger: Windows ships **no software HEVC
  encoder**, Windows 10 needs the HEVC Video Extensions package to *play* HEVC
  back (without it playback fails with `MF_E_TOPO_CODEC_NOT_FOUND`), and plenty
  of upload targets reject it. `RecordEncoder` overrides the ranking; a choice that stops
  probing clean falls back down the list and says so in the log.

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
Nitshot installs a `WH_KEYBOARD_LL` hook instead. Low-level hooks run
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

`Alt+PrtScn` is deliberately left alone — that stays Windows' own window grab.

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
border occasionally stays on screen after the capture has ended. Nitshot uses
DXGI Desktop Duplication, which has no such overlay.

## Building

```
.\build.bat
```

MSVC 2022 Build Tools, x64, `/W4 /WX`. Falls back to MinGW-w64 if MSVC is not present.

The only third-party files are vendor headers, each with a README naming
its source and checksums: `third_party/nvenc/nvEncodeAPI.h`, NVIDIA's NVENC
API (MIT, from FFmpeg's nv-codec-headers, API 12.1), `third_party/amf`,
AMD's AMF SDK headers (MIT, v1.4.35), and `third_party/vpl`, Intel's oneVPL
API headers (MIT, v2.13.0). Nothing is linked against any of them - the
drivers' DLLs are loaded at run time.

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

Further modes, each on its own:

- `a` - sound: every device opened and its delivered rate measured, the mixer
  checked sample by sample, and recordings in every combination of sources,
  mixing, WAVs and rebuild-from-WAV, with track count, length and names;
- `e` - encoders: the bit-depth reader, a Main 12 request seen through, the
  forced probe, and real HDR and H.264 recordings read back;
- `n` - NVENC: the driver found, its API version and what it can encode, and
  the missing-driver and wrong-DLL paths; then moving test frames encoded
  through it into an MP4 (H.264 with and without B-frames and lookahead,
  HEVC with B-frames) and read back - every frame present, on its exact
  time from zero, B-frames stored out of order, profile and depth, and luma
  PSNR against the frames that went in where Windows can decode the codec -
  and which colour matrix NVENC applies to BGRA input; and HDR10 from P010:
  Main10, BT.2020/PQ, a mastering-display SEI on each key frame with the
  values asked for, constant 30 fps, and 10-bit PSNR. The last two need
  `ffprobe`/`ffmpeg` on PATH (Windows 10 has no HEVC decoder of its own
  without the paid HEVC Video Extensions) and are reported as unchecked
  without them;
- `q` - NVENC against Media Foundation: the same screen-like frames through
  both at the same bitrates, SSIM and PSNR by ffmpeg, size and time; needs
  `ffmpeg` on PATH;
- `u` - the settings dialog: a screenshot of the Recording tab and the refusal
  of one device in both sound lists.

`e` and `u` write to `settings.ini` on the way and put it back byte for byte.

It needs a desktop that is awake - a sleeping display hands Desktop Duplication
no frames at all, and the recording check then fails for want of anything to
encode.

## Language

English and German ship; the interface follows the Windows display language by
default. Everything visible lives in `langs\` beside the exe, one file per
language, and English is *also* compiled in as a last-resort fallback so a
missing or damaged folder can never leave the window blank.

```
Nitshot.exe
langs\
  en.ini      the template - copy it to add a language
  de.ini
```

**Adding one needs no rebuild and no restart.** Copy `en.ini` to `fr.ini`,
translate the text after each `=`, and the language appears in Settings by
itself, named the way Windows names it ("Français", not "French"). Choosing it
switches the open window there and then, because being asked to accept a
language you cannot read yet is a poor way to choose one. `%APPDATA%\Nitshot\langs\`
is searched first, so a shipped translation can be replaced where the program
folder is read-only.

Four decisions worth knowing about:

- **The fallback is per string, not per file.** A key that is missing, empty or
  misspelt falls back to English on its own, so a half-finished translation is
  usable from the first line. Verified with a deliberately broken `fr.ini`:
  6 of 79 keys translated, one unknown key, one malformed line and a run of
  invalid UTF-8 — the six appeared in French, everything else in English, the
  bad bytes rendered as replacement glyphs, and nothing else was affected.
- **The files are parsed here, not by `GetPrivateProfileString`.** That API
  reads a BOM-less file in the system code page, and `settings.ini` is exactly
  that, so a translator saving UTF-8 out of Notepad would have got mojibake
  umlauts. `langs\*.ini` are UTF-8 with or without a BOM.
- **`%1`, `%2` are substituted positionally**, so a translation may reorder
  them — the German `TOAST_RECORDED` does exactly that. This is deliberately
  not `FormatMessage`, which treats `%n` as a hard line break and would let a
  user-supplied file break the formatter rather than merely look wrong.
- **Product vocabulary stays English in every language**: Nitshot,
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

Everything is also in `%APPDATA%\Nitshot\settings.ini`, written with
defaults on first run.

| Key | Default | Meaning |
|---|---|---|
| `CopyToClipboard` | 1 | put every snip on the clipboard |
| `SaveToDisk` | 1 | also write a file |
| `SaveFolder` | *(empty)* | empty means `Pictures\Nitshot` |
| `VideoFolder` | *(empty)* | where recordings go; empty means `Videos\Nitshot`. Recordings are always saved, so this does not depend on `SaveToDisk` |
| `HookWinShiftS` | 1 | take over `Win+Shift+S` |
| `HookPrintScreen` | 1 | take over `PrtScn` |
| `PrtScnFullscreen` | 0 | `PrtScn` grabs everything without showing the overlay |
| `DefaultMode` | 0 | 0 rectangle, 1 freeform, 2 window, 3 fullscreen |
| `DimPercent` | 40 | how far the overlay dims the desktop |
| `ShowToast` | 1 | tray balloon naming the saved file |
| `HdrSaveJxr` | 1 | HDR snips go to `.jxr` |
| `HdrSdrSidecar` | 1 | …plus a tone-mapped `.png` beside it |
| `HdrSdrRollOff` | 0 | keep highlight detail in the 8-bit copy, at the cost of white landing near 231 |
| `HdrJxrQuality` | 90 | 1–100; 100 is true lossless and much slower |
| `ForceSdr` | 0 | ignore advanced colour and capture 8-bit |
| `IdleReleaseMinutes` | 5 | minutes without a snip before the capture machinery is released entirely (0 = keep it warm); the next snip then takes about 0.1 s longer, once |
| `RecordAudioDevice` | `default-playback` | `none`, `default-playback`, `default-input`, or an endpoint id; an older `RecordAudio=0/1/2` is converted |
| `RecordAudioName` | | the device's name when it was chosen, shown while it is unplugged |
| `RecordAudio2Device` | `none` | the second source, same values |
| `RecordAudio2Name` | | as `RecordAudioName`, for the second source |
| `RecordAudioMix` | 0 | mix the two sources into one track instead of recording two |
| `RecordFps` | 30 | 5–60 |
| `RecordRawAudio` | 1 | ask the endpoint to bypass its effects; many drivers refuse |
| `RecordEncoder` | `auto` | `auto`, `h264`, `hevc8` or `hevc10`; only what probes clean is offered |
| `RecordHdr` | 1 | capture 10-bit HDR when the display is in it |
| `RecordWavSidecar` | 0 | also write the untouched PCM as a `.wav` beside the `.mp4` |
| `RecordMuxFromWav` | 0 | with the above: no live AAC; build the `.mp4`'s sound from the `.wav` afterwards |
| `RecordKeepSourceWavs` | 0 | with the above and two mixed sources: keep `.track1.wav` and `.track2.wav` |
| `RecordZeroCopy` | 1 | frames stay on the GPU into the encoder; 0 forces the CPU path, for a driver that misbehaves |
| `RecordDirectEncoder` | 1 | encode through the graphics card's own encoder API - NVENC on NVIDIA, AMF on AMD, oneVPL on Intel Arc/Xe; 0 = always Media Foundation |
| `RecordEncoderPreset` | 5 | quality preset, 1 (fastest) .. 7 (best): NVENC's P1..P7; on AMD 1-3 speed, 4-5 balanced, 6-7 quality; on Intel target usage 7..1 |
| `RecordEncoderBFrames` | -1 | B-frames, 0..5; -1 = 2 for H.264, 3 for HEVC; capped at what the GPU offers (AMD: H.264 only, up to 3; Intel: as the runtime accepts) |
| `RecordEncoderLookahead` | 16 | frames the encoder looks ahead for its rate decisions, 0..32; 0 = off (AMD: pre-analysis; Intel: `LookAheadDepth`) |
| `RecordEncoderAq` | 0 | spatial adaptive quantisation (AMD: VBAQ, Intel: MBBRC); off because it takes bits from text (measured: 1.5-2 dB less luma PSNR) |

The `RecordNvenc*` keys of 0.8.4.0 are read once and replaced by these.
| `SettingsX`, `SettingsY` | *unset* | where the settings window was dragged; unset means centred |
| `Language` | `auto` | `auto` follows Windows; otherwise a tag with a `langs\<tag>.ini` |
| `ForceGdi` | 0 | skip Desktop Duplication and always use GDI |
| `DebugLog` | 0 | append diagnostics to `Nitshot.log` beside this file; at 4 MB it is moved to `Nitshot.old.log` and started afresh |

`DebugLog` exists because everything interesting here happens with no window on
screen: a hotkey that never arrived and a capture that failed look identical from
the outside. With it on, **F12 inside the overlay** writes the swap chain's back
buffer to `overlay-debug.png` in the screenshots folder — a flip-model swap chain
is invisible to every ordinary screen-capture route, `BitBlt` and `PrintWindow`
both returning the desktop underneath it, so that dump is the only way to look at
what the overlay actually rendered.

## When it crashes

`crash.c` installs an unhandled-exception filter at start. A crash writes a
line into `Nitshot.log` - whether the diagnostic log is on or not - with the
exception code, the module and offset it happened in, for an access
violation the address and whether it was read or written, and the last
step the code announced through `Crash_Step` before it died; and it saves a
minidump (`crash-<date>-<time>.dmp`, about 100 KB: threads, stacks, modules)
beside the log in `%APPDATA%\Nitshot`. The encoder paths announce their
risky steps - the copy into the encoder's surface, the vendor runtime's
encode and sync calls, the MP4 write - so a crash inside a vendor runtime
names the call it was in.

Not every death reaches that filter: a fail-fast, a heap a runtime finds
corrupt, a runtime that installs its own filter, a hang, a runtime calling
`ExitProcess`. So a recording through the card's own encoder is watched
more closely. Serious first-chance exceptions are logged the moment they
are raised (a vectored handler, ahead of anything a runtime installed).
The filter is put back, and the fact logged, if the runtime replaced it.
A watch thread logs a recording that has not moved for 10 seconds and
writes `hang-<date>-<time>.dmp` of every thread. A TLS callback logs the
process leaving through `ExitProcess` mid-recording. When the next start
finds a recording that never ended, it copies Windows' own record of the
crash or hang (Application Error 1000 / Application Hang 1002 in the
Application event log, readable without admin rights) into the log; that
record also catches what no handler inside the process can. These lines
are written whether the diagnostic log is on or not (`Log_Always`).
`rectest k` crashes, hangs and exits a child on purpose and checks each.

A recording through the graphics card's own encoder also marks itself in
the INI (`DirectRecordingRunning`) while it runs and clears the mark when it
ends. A mark found at the next start means the process died mid-recording:
`RecordDirectEncoder` is switched off - recording works again at once,
through Media Foundation - and a notification says so, even with
notifications switched off. For the AMD and Intel paths, which were written
without the hardware, that turns a crash into one lost recording rather
than a program that cannot record.

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
