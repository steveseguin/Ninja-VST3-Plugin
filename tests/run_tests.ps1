# PowerShell script to build and run all tests
# Usage: .\tests\run_tests.ps1 [-BuildOnly] [-TestOnly] [-Verbose]

param(
    [switch]$BuildOnly = $false,
    [switch]$TestOnly = $false,
    [switch]$Verbose = $false,
    [switch]$StressOnly = $false
)

$ErrorActionPreference = "Stop"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "WebRTC VST Plugin - Test Runner" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$rootDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $rootDir "build\webrtc_vst_win"
$pluginPath = Join-Path $buildDir "VST3\Release\webrtc_vst.vst3"
$integrationTest = Join-Path $buildDir "tests\Release\webrtc_vst_integration_test.exe"
$stressTest = Join-Path $buildDir "tests\Release\webrtc_vst_stress_test.exe"

# Build
if (-not $TestOnly) {
    Write-Host "`n[1/3] Building plugin..." -ForegroundColor Yellow

    if (-not (Test-Path $buildDir)) {
        Write-Host "Build directory not found. Please run CMake configure first." -ForegroundColor Red
        Write-Host "Example:" -ForegroundColor Gray
        Write-Host "  cmake -B build/webrtc_vst_win -S webrtc_vst -G `"Visual Studio 17 2022`" -DVST3_SDK_ROOT=vst3sdk" -ForegroundColor Gray
        exit 1
    }

    Write-Host "  Building webrtc_vst..." -ForegroundColor Gray
    cmake --build $buildDir --config Release --target webrtc_vst
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ✗ Plugin build failed" -ForegroundColor Red
        exit 1
    }
    Write-Host "  ✓ Plugin built successfully" -ForegroundColor Green

    Write-Host "`n[2/3] Building tests..." -ForegroundColor Yellow

    Write-Host "  Building integration tests..." -ForegroundColor Gray
    cmake --build $buildDir --config Release --target webrtc_vst_integration_test
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ✗ Integration test build failed" -ForegroundColor Red
        exit 1
    }

    Write-Host "  Building stress tests..." -ForegroundColor Gray
    cmake --build $buildDir --config Release --target webrtc_vst_stress_test
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ✗ Stress test build failed" -ForegroundColor Red
        exit 1
    }

    Write-Host "  ✓ All tests built successfully" -ForegroundColor Green
} else {
    Write-Host "`n[Skipping build]" -ForegroundColor Yellow
}

if ($BuildOnly) {
    Write-Host "`n✓ Build complete (test execution skipped)" -ForegroundColor Green
    exit 0
}

# Verify files exist
if (-not (Test-Path $pluginPath)) {
    Write-Host "`nError: Plugin not found at $pluginPath" -ForegroundColor Red
    Write-Host "Run without -TestOnly to build first" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $integrationTest)) {
    Write-Host "`nError: Integration test not found at $integrationTest" -ForegroundColor Red
    Write-Host "Run without -TestOnly to build first" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $stressTest)) {
    Write-Host "`nError: Stress test not found at $stressTest" -ForegroundColor Red
    Write-Host "Run without -TestOnly to build first" -ForegroundColor Yellow
    exit 1
}

# Set environment variables for verbose logging if requested
if ($Verbose) {
    $env:WEBRTC_VST_LOG_STDOUT = "1"
    $env:WEBRTC_VST_LOG_SIGNALING = "1"
}

$allTestsPassed = $true

# Run integration tests
if (-not $StressOnly) {
    Write-Host "`n[3a/3] Running integration tests..." -ForegroundColor Yellow
    Write-Host "  Plugin: $pluginPath" -ForegroundColor Gray
    Write-Host ""

    & $integrationTest $pluginPath
    $integrationResult = $LASTEXITCODE

    if ($integrationResult -eq 0) {
        Write-Host "`n  ✓ Integration tests PASSED" -ForegroundColor Green
    } else {
        Write-Host "`n  ✗ Integration tests FAILED" -ForegroundColor Red
        $allTestsPassed = $false
    }
}

# Run stress tests
Write-Host "`n[3b/3] Running stress tests..." -ForegroundColor Yellow
Write-Host "  Plugin: $pluginPath" -ForegroundColor Gray
Write-Host ""

& $stressTest $pluginPath
$stressResult = $LASTEXITCODE

if ($stressResult -eq 0) {
    Write-Host "`n  ✓ Stress tests PASSED" -ForegroundColor Green
} else {
    Write-Host "`n  ✗ Stress tests FAILED" -ForegroundColor Red
    $allTestsPassed = $false
}

# Final summary
Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "FINAL SUMMARY" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if ($allTestsPassed) {
    Write-Host "✓ ALL TESTS PASSED" -ForegroundColor Green
    Write-Host ""
    Write-Host "The plugin is stable and ready for deployment." -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Gray
    Write-Host "  1. Deploy to VST3 directory" -ForegroundColor Gray
    Write-Host "  2. Test in Audacity manually" -ForegroundColor Gray
    Write-Host "  3. Monitor for any crashes" -ForegroundColor Gray
} else {
    Write-Host "✗ SOME TESTS FAILED" -ForegroundColor Red
    Write-Host ""
    Write-Host "The plugin has stability issues that need to be fixed." -ForegroundColor Red
    Write-Host ""
    Write-Host "Debugging tips:" -ForegroundColor Gray
    Write-Host "  - Run with -Verbose to see detailed logs" -ForegroundColor Gray
    Write-Host "  - Check THREADING.md for threading issues" -ForegroundColor Gray
    Write-Host "  - Review ALL_FIXES_APPLIED.md for known issues" -ForegroundColor Gray
    Write-Host "  - Run under debugger to catch crashes" -ForegroundColor Gray
}

Write-Host "============================================================" -ForegroundColor Cyan

exit ($allTestsPassed ? 0 : 1)
