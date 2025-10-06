# VST Plugin Seed Test
# Publishes audio to VDO.Ninja for browser viewing

# You can change this to any stream ID you want
$streamId = "cli_loopback_tg2tg7"
# Or use timestamp: "vst_web_test_" + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString()
$toneHz = 1000

Write-Host "=== VST Plugin -> Web Viewer Test ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Starting VST plugin in seed mode..."
Write-Host "Stream ID: $streamId"
Write-Host "Publishing ${toneHz} Hz test tone"
Write-Host ""

# Set environment variables
$env:WEBRTC_VST_MODE = "seed"
$env:WEBRTC_CLI_HOST_ITERATIONS = "100000"
$env:WEBRTC_VST_STREAM_ID = $streamId
$env:WEBRTC_VST_PASSWORD = "false"
$env:WEBRTC_VST_HANDSHAKE_URL = "wss://wss.vdo.ninja"
$env:WEBRTC_VST_LOG_STDOUT = "1"
$env:WEBRTC_VST_LOG_SIGNALING = "1"
$env:WEBRTC_CLI_HOST_WARMUP_MS = "2000"
$env:WEBRTC_CLI_HOST_TONE_HZ = $toneHz.ToString()

Write-Host "When you see 'Sent seed request', open this URL in your browser:" -ForegroundColor Yellow
Write-Host ""
Write-Host "  https://vdo.ninja/?view=$streamId&password=false" -ForegroundColor White
Write-Host ""
Write-Host "Press Ctrl+C to stop." -ForegroundColor Gray
Write-Host ""

# Run the CLI directly (not in background job)
& "build/webrtc_vst_win/bin/Release/webrtc_vst_cli_host.exe"
