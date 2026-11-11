# Recording smoke tests

Run these from an interactive, unlocked Windows desktop after building `ScreenshotService.exe`. Close the tray instance before starting.

```powershell
.\tests\record-smoke-test.ps1 -ExePath .\ScreenshotService.exe -Backend Auto -Seconds 4
.\tests\record-smoke-test.ps1 -ExePath .\ScreenshotService.exe -Backend MFT -Seconds 4
.\tests\record-smoke-test.ps1 -ExePath .\ScreenshotService.exe -Backend Native -Seconds 4
```

- `Auto` exercises the configured recording preference and accepts the expected fallback to Media Foundation if a native encoder fails before producing a packet.
- `MFT` disables native encoding only for that test launch, to isolate the Media Foundation hardware encoder path without changing the user's saved setting.
- `Native` requires the adapter's native encoder to reach MP4 stream setup and treats MFT fallback as a test failure. If the selected adapter/profile has no supported native path, this test fails rather than reporting a misleading pass.

Each run validates top-level MP4 boxes from inside the application. If `%WINDIR%\ffprobe.exe` exists (or `-FFprobePath` is supplied), the script also checks that the video stream is readable and reports codec, dimensions, frame rate, pixel format and colour transfer. If `%WINDIR%\ffmpeg.exe` exists, it additionally decodes the video stream to a null output.

The tests do not force Windows HDR on/off; run them once in SDR and once after enabling Advanced Color to exercise both capture paths. A native-only run tests only the profile currently selected by the application for that display.
