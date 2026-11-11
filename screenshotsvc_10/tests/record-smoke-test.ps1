[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string] $ExePath = (Join-Path $PSScriptRoot '..\ScreenshotService.exe'),
    [ValidateRange(2, 15)]
    [int] $Seconds = 4,
    [ValidateSet('Auto', 'MFT', 'Native')]
    [string] $Backend = 'Auto',
    [string] $FFprobePath = '',
    [ValidateRange(20, 180)]
    [int] $TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$resolvedExe = (Resolve-Path -LiteralPath $ExePath).Path
$processName = [IO.Path]::GetFileNameWithoutExtension($resolvedExe)
$logPath = Join-Path $env:APPDATA 'ScreenshotService\ScreenshotService.log'

# Don't interrupt an already running tray instance. Its mutex intentionally
# makes the smoke-test invocation exit with code 5.
$existing = Get-Process -Name $processName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Error "Close the existing $processName tray instance before running the recording smoke test."
    exit 5
}

$modeArg = switch ($Backend) {
    'MFT'    { '--record-smoke-test-mft' }
    'Native' { '--record-smoke-test-native' }
    default  { '--record-smoke-test' }
}
Write-Host "Running $Seconds-second $Backend recording smoke test on the primary monitor..."
$p = Start-Process -FilePath $resolvedExe -ArgumentList @($modeArg, "$Seconds") -PassThru
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    try { $p.Kill() } catch { }
    Write-Error "The smoke test exceeded $TimeoutSeconds seconds. Check $logPath for the last recording step / watchdog dump."
    exit 124
}

if ($p.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $logPath) {
        Write-Host 'Last diagnostic lines:'
        Get-Content -LiteralPath $logPath -Tail 50 | Write-Host
    }
    Write-Error "Recording smoke test failed with exit code $($p.ExitCode)."
    exit $p.ExitCode
}

$resultLine = $null
if (Test-Path -LiteralPath $logPath) {
    $result = Select-String -LiteralPath $logPath -Pattern 'smoke: result .*mp4_valid=1 .*exit=0' | Select-Object -Last 1
    if (-not $result) {
        Get-Content -LiteralPath $logPath -Tail 50 | Write-Host
        Write-Error "The process exited successfully but the log has no valid-MP4 result: $logPath"
        exit 2
    }
    $resultLine = $result.Line
    Write-Host $resultLine
}

# The application has already checked top-level MP4 box structure. If FFprobe
# is available, additionally verify a video stream, reasonable dimensions/FPS,
# and if FFmpeg is available decode the whole stream to a null output.
$mp4 = $null
if ($resultLine -and $resultLine -match 'file=(?<file>.+?) size=\d+ exit=0') {
    $mp4 = $Matches['file'].Trim()
}
if ($mp4 -and (Test-Path -LiteralPath $mp4)) {
    $ffprobe = $FFprobePath
    if (-not $ffprobe) { $ffprobe = Join-Path $env:WINDIR 'ffprobe.exe' }
    if (Test-Path -LiteralPath $ffprobe) {
        $json = (& $ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,pix_fmt,color_transfer,color_primaries,color_space -of json $mp4 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) {
            Write-Error "ffprobe could not read the recorded MP4: $json"
            exit 7
        }
        try { $probe = $json | ConvertFrom-Json } catch { Write-Error 'ffprobe returned invalid JSON.'; exit 7 }
        if (-not $probe.streams -or $probe.streams.Count -lt 1) {
            Write-Error 'The MP4 contains no readable video stream.'
            exit 7
        }
        $stream = $probe.streams[0]
        $fps = 0.0
        if ($stream.avg_frame_rate -match '^(?<num>\d+)/(?<den>\d+)$' -and [double]$Matches['den'] -gt 0) {
            $fps = [double]$Matches['num'] / [double]$Matches['den']
        }
        if ($stream.width -lt 16 -or $stream.height -lt 16 -or
            $stream.codec_name -notin @('h264','hevc','av1') -or $fps -lt 5) {
            Write-Error "Unexpected video stream: $($stream.codec_name) $($stream.width)x$($stream.height) at $($stream.avg_frame_rate)."
            exit 7
        }
        Write-Host ("Video stream: {0}, {1}x{2}, {3} fps, pixel format {4}, transfer {5}" -f $stream.codec_name,$stream.width,$stream.height,[math]::Round($fps,2),$stream.pix_fmt,$stream.color_transfer)
    }
    $ffmpeg = Join-Path $env:WINDIR 'ffmpeg.exe'
    if (Test-Path -LiteralPath $ffmpeg) {
        & $ffmpeg -nostdin -v error -i $mp4 -map 0:v:0 -f null NUL 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Error 'FFmpeg could not decode the recorded video stream.'
            exit 7
        }
    }
}

Write-Host 'PASS: a valid, non-empty MP4 was recorded and finalized.' -ForegroundColor Green
exit 0
