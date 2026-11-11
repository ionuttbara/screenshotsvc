# ScreenshotService Classic

A separate legacy branch of ScreenshotService for Windows XP, Vista, Windows 7 and Windows 8. It uses **AVI** rather than MP4 and old Win32 APIs rather than DXGI, D3D11, WASAPI or Media Foundation. The current `v141_xp` project targets XP SP3 x86; Windows 2000 needs a separately tested VC9 build and is not guaranteed by this project configuration.

## Build

Recommended compiler for the included Visual Studio project: Visual Studio 2022 with **C++ Windows XP Support for VS 2017 (v141) tools**, building Win32/x86. Run `build.bat` from a command prompt. The project sets the Windows subsystem minimum to 5.01 (XP). For true Windows 2000 CRT compatibility, a distinct Visual Studio 2008 SP1 / VC9 x86 build is needed.

## User experience

ScreenshotService Classic runs in the notification area. Press **PrtScn** or **Win+Shift+S** to open a dimmed capture overlay with buttons across the top:

- **Region** — drag a rectangle for a screenshot; the PNG is saved automatically, with BMP fallback.
- **Full shot** — capture the full desktop and save automatically.
- **Record** — choose a rectangle and start AVI recording.
- **Full rec** — start recording the full virtual desktop to AVI.
- **GIF area** — choose a rectangle and record an animated GIF.
- **GIF full** — record the full desktop as an animated GIF.
- **X / Esc / right-click** — cancel the overlay.

While AVI or GIF recording is active, a small topmost status bar shows the mode, elapsed time and a **Stop** button. The status bar is masked from saved frames using the previous clean frame, so it does not flicker or appear in the recording.

On systems with <=512 MB RAM or one logical CPU, automatic low-resource mode uses AVI at 10 FPS and max 1024x768, GIF at 8 FPS and max 800x600, and captures directly into the output-sized frame buffer to avoid full-screen plus crop allocations.

The toolbar is also keyboard accessible: `1` Region, `2` Full shot, `3` Record, `4` Full rec, `5` GIF area, `6` GIF full. `Shift+PrtScn` opens the overlay in Record mode; `Alt+PrtScn` toggles a full-desktop AVI recording. Pressing PrtScn while a recording is active stops it.

## Screenshots

- Full desktop and rectangular captures automatically save to `C:\Screenshots` as PNG, with BMP fallback; no confirmation dialog is shown after successful save.
- The captured cursor is composited into screenshots.
- This Classic overlay currently prioritizes direct autosave; the editor source remains in the project but is not opened by the default capture actions.

## Video and GIF

- AVI recording uses Video for Windows (AVIFile) and automatically tries common installed VfW codecs (Microsoft Video 1, MJPEG, Cinepak, Indeo 5). If none is available, it writes uncompressed AVI.
- AVI audio is PCM captured from the default WinMM recording input, when available.
- GIF recording is implemented natively as GIF89a with LZW compression and a 3-3-2 palette, with per-frame delay timestamps so slow capture does not have to LZW-encode duplicate frames. Maximum duration is 15 seconds. On low-resource systems it uses 8 FPS and max 800x600; otherwise 15 FPS and max 1920x1080, preserving aspect ratio.
- If desktop resolution changes during AVI/GIF recording, the current desktop image is scaled to the recording's original dimensions and the original selected rectangle is used.
- Mouse cursor is included in video/GIF frames.

## Legacy limitations

- No HDR, HEVC, MP4, AAC/FLAC multi-track audio, or modern Media Foundation features.
- XP/2000 do not provide modern WASAPI loopback. To capture system sound, an audio driver must expose **Stereo Mix / What U Hear** and that must be chosen as the default recording source. Otherwise the AVI can contain microphone/input audio or video only.
- GDI capture is not guaranteed to work with games using exclusive fullscreen DirectDraw/Direct3D modes; borderless fullscreen is more suitable.
- GIF uses a fixed 3-3-2 palette to remain dependency-free; photographic content and gradients will show palette banding compared with modern high-quality encoders.
- Freeform/window snipping and live pen drawing over video are not yet implemented in this legacy branch; static screenshot annotation is supported.

## API basis

The project uses GDI/BitBlt for pixels, `GetCursorInfo` to composite the pointer, WinMM `waveIn` for audio, Video for Windows AVIFile for AVI output, and a small self-contained GIF89a/LZW writer for GIF output.


## Latest behavior

- Full-desktop and region screenshots are saved automatically to `C:\Screenshots` as PNG, with BMP fallback. They no longer stop at a modal editor/confirmation step.
- During region-based AVI/GIF recording, a red/white outline marks the active crop. The outline is a UI aid: capture code replaces the edge pixels with the previous clean frame so the border is not burned into the recording. Full-screen recording does not draw the crop border.
- The crop outline follows the mapped capture rectangle if display dimensions change and is removed when recording stops.


## Recording-area indication and autosave

The recording region is outlined with a red/white frame for the full duration of region-based AVI/GIF capture. The outline is clickable-through and stays out of the saved media: the edge pixels are restored from the previous clean frame. Full-screen recording has no region outline.
