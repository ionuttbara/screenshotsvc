# Build notes

- Project: `ScreenshotServiceClassic.vcxproj`, Win32/x86, `v141_xp` toolset, Windows subsystem 5.01.
- Build script: `build.bat` discovers MSBuild and builds Release|Win32.
- Dependencies: user32, gdi32, shell32, comdlg32, comctl32, vfw32, winmm. No FFmpeg, WIC, Media Foundation, DXGI or WASAPI.
- GIF recording uses `giflegacy.c`, which is portable C and has been tested independently against a GIF decoder.
- Validate on Windows XP SP3 x86: PrtScn toolbar, rectangular screenshot and editor, AVI start/stop with available codec, GIF 15-second cap, sound input availability and resolution change.


## Low-resource mode and recording stop UI

At <=512 MB physical RAM or one logical CPU, recordings are capped to AVI 10 FPS at 1024x768 and GIF 8 FPS at 800x600. The app captures only the selected source area directly into its final-size DIB rather than creating a full desktop bitmap and an extra crop allocation. A small topmost Stop control appears for AVI and GIF; its rectangle is patched with the preceding clean frame when encoding so it stays visible without being burned into the video. GIF holds one pending frame and uses variable frame delays to avoid repeatedly LZW-encoding duplicate frames on slow CPUs.
