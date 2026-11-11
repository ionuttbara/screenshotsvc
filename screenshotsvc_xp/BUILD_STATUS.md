# Build status — ScreenshotService Classic

- The modified GIF writer was compiled with Clang/GCC-compatible C compilation and tested in a portable harness. A 5-frame 64x48 animated GIF with variable delays decoded correctly with Pillow (frames and per-frame duration verified).
- Static source review confirms a visible topmost Stop button during AVI and GIF capture, plus the tray Stop command and existing hotkey fallback.
- Low-resource mode automatically activates at <=512 MB physical RAM or one logical CPU. It captures directly into output-sized buffers, caps AVI to 1024x768 at 10 FPS and GIF to 800x600 at 8 FPS.
- The stop overlay is composited out of the captured pixels using the previous clean frame, avoiding repeated hide/show flicker.
- The full Windows app has **not** been compiled with MSVC or run on Windows in this environment; verify with the included v141_xp Win32 build and test AVI/GIF and the Stop button on the target PC.
- The included v141_xp toolset targets Windows XP SP3 x86 and newer. Windows 2000 still requires a separately tested older VC9 CRT/toolchain.

- Added automatic static screenshot saving for both full-desktop and selected-region captures: PNG output with BMP fallback, no success modal.
- Region AVI/GIF captures show a red/white topmost crop boundary; the capture pipeline restores the matching perimeter from the previous clean frame so it is a UI indicator and is not burned into the recording. Full-screen recording has no boundary.
- The crop outline tracks the scaled source region when desktop metrics change and is throttled to repaint only when its bounds change, reducing overhead on Pentium 4 / 256 MB systems.
