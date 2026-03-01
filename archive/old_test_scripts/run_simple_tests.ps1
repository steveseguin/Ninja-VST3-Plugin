# Simple test runner using existing cli_host
# This avoids compilation issues and tests with the working host

param(
    [int]$RapidCycles = 50,
    [switch]$Verbose = $false
)

$ErrorActionPreference = "Stop"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "WebRTC VST Plugin - Simple Automated Tests" -ForegroundColor Cyan
Write-Host "Using existing CLI host for validation" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$rootDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $rootDir "build\webrtc_vst_win"
$cliHost = Join-Path $buildDir "bin\Release\webrtc_vst_cli_host.exe"
$pluginPath = Join-Path $buildDir "VST3\Release\webrtc_vst.vst3"

# Check if files exist
if (-not (Test-Path $cliHost)) {
    Write-Host "`nError: CLI host not found at $cliHost" -ForegroundColor Red
    Write-Host "Build it with: cmake --build build\webrtc_vst_win --config Release --target webrtc_vst_cli_host" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $pluginPath)) {
    Write-Host "`nError: Plugin not found at $pluginPath" -ForegroundColor Red
    Write-Host "Build it with: cmake --build build\webrtc_vst_win --config Release --target webrtc_vst" -ForegroundColor Yellow
    exit 1
}

Write-Host "`nPlugin: $pluginPath" -ForegroundColor Gray
Write-Host "CLI Host: $cliHost" -ForegroundColor Gray

# Set environment for verbose logging if requested
if ($Verbose) {
    $env:WEBRTC_VST_LOG_STDOUT = "1"
    $env:WEBRTC_VST_LOG_SIGNALING = "1"
}

$allTestsPassed = $true

# Test 1: Basic Load/Process
Write-Host "`n[1/3] Basic Load and Process Test..." -ForegroundColor Yellow
$env:WEBRTC_CLI_HOST_ITERATIONS = "100"
$env:WEBRTC_CLI_HOST_TIMEOUT_MS = "10000"

try {
    & $cliHost $pluginPath 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [PASS] Basic test PASSED" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] Basic test FAILED - exit code: $LASTEXITCODE" -ForegroundColor Red
        $allTestsPassed = $false
    }
} catch {
    Write-Host "  [FAIL] Basic test FAILED: $_" -ForegroundColor Red
    $allTestsPassed = $false
}

# Test 2: Rapid Open/Close
Write-Host "" -ForegroundColor Yellow
Write-Host "[2/3] Rapid Open/Close Test - $RapidCycles iterations..." -ForegroundColor Yellow
$env:WEBRTC_CLI_HOST_ITERATIONS = "10"
$env:WEBRTC_CLI_HOST_TIMEOUT_MS = "5000"

$failed = 0
for ($i = 1; $i -le $RapidCycles; $i++) {
    if ($i % 10 -eq 0) {
        Write-Host "  Progress: $i / $RapidCycles" -ForegroundColor Gray
    }

    try {
        & $cliHost $pluginPath 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $failed++
            if ($failed -eq 1) {
                Write-Host "  First failure at cycle $i" -ForegroundColor Red
            }
        }
    } catch {
        $failed++
        if ($failed -eq 1) {
            $errMsg = $_.Exception.Message
            Write-Host "  First crash at cycle $i : $errMsg" -ForegroundColor Red
        }
    }

    Start-Sleep -Milliseconds 20
}

if ($failed -eq 0) {
    Write-Host "  [PASS] Rapid open/close PASSED - $RapidCycles iterations" -ForegroundColor Green
} else {
    Write-Host "  [FAIL] Rapid open/close FAILED - $failed of $RapidCycles failed" -ForegroundColor Red
    $allTestsPassed = $false
}

# Test 3: Long Running Session
Write-Host "" -ForegroundColor Yellow
Write-Host "[3/3] Long Running Session Test..." -ForegroundColor Yellow
$env:WEBRTC_CLI_HOST_ITERATIONS = "1000"
$env:WEBRTC_CLI_HOST_TIMEOUT_MS = "30000"

try {
    & $cliHost $pluginPath 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [PASS] Long running test PASSED" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] Long running test FAILED - exit code: $LASTEXITCODE" -ForegroundColor Red
        $allTestsPassed = $false
    }
} catch {
    Write-Host "  [FAIL] Long running test FAILED: $_" -ForegroundColor Red
    $allTestsPassed = $false
}

# Summary
Write-Host "" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "TEST SUMMARY" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if ($allTestsPassed) {
    Write-Host "[PASS] ALL TESTS PASSED" -ForegroundColor Green
    Write-Host ""
    Write-Host "The plugin successfully completed:" -ForegroundColor Green
    Write-Host "  - Basic audio processing" -ForegroundColor Gray
    Write-Host "  - $RapidCycles rapid open/close iterations" -ForegroundColor Gray
    Write-Host "  - Long running session with 1000 blocks" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Plugin is stable and ready for Audacity testing." -ForegroundColor Green
} else {
    Write-Host "[FAIL] SOME TESTS FAILED" -ForegroundColor Red
    Write-Host ""
    Write-Host "The plugin has stability issues." -ForegroundColor Red
    Write-Host ""
    Write-Host "Debugging tips:" -ForegroundColor Gray
    Write-Host "  - Run with -Verbose to see detailed logs" -ForegroundColor Gray
    Write-Host "  - Check THREADING.md for threading issues" -ForegroundColor Gray
    Write-Host "  - Review ALL_FIXES_APPLIED.md for known fixes" -ForegroundColor Gray
}

Write-Host "============================================================" -ForegroundColor Cyan

if ($allTestsPassed) {
    exit 0
} else {
    exit 1
}
