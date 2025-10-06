# VST Plugin Play Test
# Receives audio from VDO.Ninja browser publisher

# You can change this to any stream ID you want
$streamId = "cli_loopback_tg2tg7"

Write-Host "=== VST Plugin <- Browser Publisher Test ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Starting VST plugin in play mode..."
Write-Host "Stream ID: $streamId"
Write-Host "Waiting to receive audio..."
Write-Host ""

# Set environment variables
$env:WEBRTC_VST_MODE = "play"
$env:WEBRTC_CLI_HOST_ITERATIONS = "100000"
$env:WEBRTC_VST_STREAM_ID = $streamId
$env:WEBRTC_VST_PASSWORD = "false"
$env:WEBRTC_VST_HANDSHAKE_URL = "wss://wss.vdo.ninja"
$env:WEBRTC_VST_LOG_STDOUT = "1"
$env:WEBRTC_VST_LOG_SIGNALING = "1"
$env:WEBRTC_CLI_HOST_WARMUP_MS = "2000"
$env:WEBRTC_CLI_HOST_MONITOR_OUTPUT = "1"  # Show audio levels

Write-Host "To publish audio, open this URL in your browser:" -ForegroundColor Yellow
Write-Host ""
Write-Host "  https://vdo.ninja/?push=$streamId&password=false&audioonly" -ForegroundColor White
Write-Host ""
Write-Host "Or use OBS/etc with stream ID: $streamId" -ForegroundColor Gray
Write-Host ""
Write-Host "Press Ctrl+C to stop." -ForegroundColor Gray
Write-Host ""

# Run the CLI directly
& "build/webrtc_vst_win/bin/Release/webrtc_vst_cli_host.exe"
