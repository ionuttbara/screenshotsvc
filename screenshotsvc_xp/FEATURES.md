# ScreenshotService Classic feature matrix

| Feature | Classic port | Notes |
|---|---|---|
| PrintScreen capture overlay | Yes | Dims the desktop and shows Region, Full shot, Record, Full rec, GIF area, GIF full and close buttons |
| Full desktop screenshot | Yes | Overlay Full shot button or tray menu |
| Rectangular screenshot | Yes | Drag a region in the overlay; automatically saves PNG (BMP fallback) |
| Screenshot pen / rectangle / circle / text | Not in default capture flow | Editor source remains in tree; direct capture hotkeys now auto-save |
| Rectangular heal/clone eraser | Not in default capture flow | Editor source remains in tree; direct capture hotkeys now auto-save |
| Screenshot cursor | Yes | Composited with `GetCursorInfo` |
| AVI video: full desktop or selected area | Yes | Video for Windows API |
| Installed VFW compression codecs | Yes | Auto-detects common codecs without stealing fullscreen focus; falls back to uncompressed |
| AVI audio | Best effort | 16-bit PCM via default WinMM wave-in device |
| System-audio loopback | Driver-dependent | Requires Stereo Mix / What U Hear to be the default WinMM input on older Windows |
| GIF recording | Yes | Native GIF89a/LZW writer, 15 FPS average, 15 seconds maximum, 1920 px max width, 3-3-2 global palette |
| Keep AVI size fixed if desktop resolution changes | Yes | Captured current desktop is scaled back to the starting virtual-desktop dimensions |
| Tray-menu commands and hotkeys | Yes | PrtScn/Win+Shift+S opens capture overlay; Alt+PrtScn toggles full AVI; Shift+PrtScn selects AVI area |
| HDR / HEVC / MP4 / AAC+FLAC triple tracks | No | Uses APIs/codec paths unavailable on Windows XP |
| Freeform and window-only screenshots | Not yet | Current legacy overlay supports rectangular selection and full-screen actions |
| Live drawing over video | Not yet | Screenshot editor tools are supported; video annotation still requires an input-transparent overlay and frame composition |


## Autosave and recording crop indication

Full and region screenshots automatically save to `C:\Screenshots` in PNG format (BMP fallback), without opening the editor or requiring a confirmation. Region video/GIF recording shows a red/white outline around the active cropped area; the capture path restores the outline perimeter from the previous clean frame so the UI border does not appear in the saved video/GIF.
