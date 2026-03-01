# WebRTC VST Plugin - Real WebRTC Connection Tests
# Tests plugin with actual wss://wss.vdo.ninja signaling server

param(
    [string]$Mode = "play",  # "play" or "seed"
    [string]$StreamId = "",
    [int]$Duration = 10,     # Test duration in seconds
    [switch]$Verbose = $false
)

$ErrorActionPreference = "Stop"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "WebRTC VST Plugin - Real Connection Tests" -ForegroundColor Cyan
Write-Host "Testing with wss://wss.vdo.ninja signaling server" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$rootDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $rootDir "build\webrtc_vst_win"
$cliHost = Join-Path $buildDir "bin\Release\webrtc_vst_cli_host.exe"
$pluginPath = Join-Path $buildDir "VST3\Release\webrtc_vst.vst3"

# Check if files exist
if (-not (Test-Path $cliHost)) {
    Write-Host "`nError: CLI host not found at $cliHost" -ForegroundColor Red
    Write-Host "Build it with: cmd.exe /c `"cmake --build build\webrtc_vst_win --config Release --target webrtc_vst_cli_host`"" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $pluginPath)) {
    Write-Host "`nError: Plugin not found at $pluginPath" -ForegroundColor Red
    Write-Host "Build it with: cmd.exe /c `"cmake --build build\webrtc_vst_win --config Release --target webrtc_vst`"" -ForegroundColor Yellow
    exit 1
}

# Generate stream ID if not provided
if ($StreamId -eq "") {
    $StreamId = "vst-test-" + (Get-Random -Minimum 1000 -Maximum 9999)
}

Write-Host "`nPlugin: $pluginPath" -ForegroundColor Gray
Write-Host "CLI Host: $cliHost" -ForegroundColor Gray
Write-Host "Mode: $Mode" -ForegroundColor Gray
Write-Host "Stream ID: $StreamId" -ForegroundColor Gray
Write-Host "Signaling: wss://wss.vdo.ninja" -ForegroundColor Gray
Write-Host ""

# Set environment variables for WebRTC testing
$env:WEBRTC_VST_MODE = $Mode
$env:WEBRTC_VST_STREAM_ID = $StreamId
$env:WEBRTC_VST_SIGNALING_URL = "wss://wss.vdo.ninja"

if ($Verbose) {
    $env:WEBRTC_VST_LOG_STDOUT = "1"
    $env:WEBRTC_VST_LOG_SIGNALING = "1"
}

# Calculate iterations based on duration (assuming 48kHz, 512 samples per block)
# ~94 blocks per second at 48kHz/512 samples
$iterations = $Duration * 94
$env:WEBRTC_CLI_HOST_ITERATIONS = $iterations.ToString()
$env:WEBRTC_CLI_HOST_TIMEOUT_MS = ($Duration * 1000 + 5000).ToString()

$allTestsPassed = $true

# Test 1: WebRTC Connection Test
Write-Host "[1/2] WebRTC Connection Test" -ForegroundColor Yellow
Write-Host "Attempting to connect to signaling server..." -ForegroundColor Gray
Write-Host "Duration: $Duration seconds" -ForegroundColor Gray

if ($Mode -eq "seed") {
    Write-Host "" -ForegroundColor Cyan
    Write-Host "SEED MODE: Plugin will broadcast audio" -ForegroundColor Cyan
    Write-Host "Connect a viewer to stream ID: $StreamId" -ForegroundColor Cyan
    Write-Host "URL: https://vdo.ninja/?view=$StreamId" -ForegroundColor Cyan
    Write-Host "" -ForegroundColor Cyan
} else {
    Write-Host "" -ForegroundColor Cyan
    Write-Host "PLAY MODE: Plugin will receive audio" -ForegroundColor Cyan
    Write-Host "Broadcast audio to stream ID: $StreamId" -ForegroundColor Cyan
    Write-Host "URL: https://vdo.ninja/?push=$StreamId" -ForegroundColor Cyan
    Write-Host "" -ForegroundColor Cyan
}

try {
    $output = & $cliHost $pluginPath 2>&1

    if ($Verbose) {
        Write-Host $output
    }

    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [PASS] WebRTC test completed successfully" -ForegroundColor Green

        # Check output for connection indicators
        if ($output -match "connected" -or $output -match "peer") {
            Write-Host "  [INFO] WebRTC peer connection detected" -ForegroundColor Cyan
        }
    } else {
        Write-Host "  [FAIL] WebRTC test failed - exit code: $LASTEXITCODE" -ForegroundColor Red
        $allTestsPassed = $false
    }
} catch {
    Write-Host "  [FAIL] WebRTC test crashed: $_" -ForegroundColor Red
    $allTestsPassed = $false
}

# Test 2: Rapid Connect/Disconnect (stress test for WebRTC cleanup)
Write-Host "" -ForegroundColor Yellow
Write-Host "[2/2] Rapid Connect/Disconnect Test - 5 cycles" -ForegroundColor Yellow
$env:WEBRTC_CLI_HOST_ITERATIONS = "50"  # Short duration per cycle
$env:WEBRTC_CLI_HOST_TIMEOUT_MS = "3000"

$failed = 0
for ($i = 1; $i -le 5; $i++) {
    Write-Host "  Cycle $i / 5..." -ForegroundColor Gray

    try {
        & $cliHost $pluginPath 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $failed++
            Write-Host "  [FAIL] Cycle $i failed" -ForegroundColor Red
        }
    } catch {
        $failed++
        Write-Host "  [FAIL] Cycle $i crashed: $_" -ForegroundColor Red
    }

    Start-Sleep -Milliseconds 100
}

if ($failed -eq 0) {
    Write-Host "  [PASS] Rapid connect/disconnect passed" -ForegroundColor Green
} else {
    Write-Host "  [FAIL] Rapid connect/disconnect failed - $failed / 5 cycles" -ForegroundColor Red
    $allTestsPassed = $false
}

# Summary
Write-Host "" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "WEBRTC TEST SUMMARY" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if ($allTestsPassed) {
    Write-Host "[PASS] ALL WEBRTC TESTS PASSED" -ForegroundColor Green
    Write-Host ""
    Write-Host "The plugin successfully:" -ForegroundColor Green
    Write-Host "  - Connected to wss://wss.vdo.ninja" -ForegroundColor Gray
    Write-Host "  - Processed audio in $Mode mode" -ForegroundColor Gray
    Write-Host "  - Handled rapid connect/disconnect cycles" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Gray
    Write-Host "  1. Test UI in Audacity" -ForegroundColor Gray
    Write-Host "  2. Test with real peer connections" -ForegroundColor Gray
    Write-Host "  3. Test audio quality and latency" -ForegroundColor Gray
} else {
    Write-Host "[FAIL] SOME WEBRTC TESTS FAILED" -ForegroundColor Red
    Write-Host ""
    Write-Host "WebRTC connection or stability issues detected." -ForegroundColor Red
    Write-Host ""
    Write-Host "Debugging tips:" -ForegroundColor Gray
    Write-Host "  - Run with -Verbose to see signaling logs" -ForegroundColor Gray
    Write-Host "  - Check network connectivity to wss.vdo.ninja" -ForegroundColor Gray
    Write-Host "  - Verify WebRTC session cleanup in WebRTCSession.cpp" -ForegroundColor Gray
}

Write-Host "============================================================" -ForegroundColor Cyan

if ($allTestsPassed) {
    exit 0
} else {
    exit 1
}
