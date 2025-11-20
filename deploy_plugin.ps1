# Deploy WebRTC VST plugin to system VST3 folder
# Run as Administrator

$ErrorActionPreference = "Stop"

$source = "build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3"
$destination = "C:\Program Files\Common Files\VST3\"

Write-Host "Deploying WebRTC VST Plugin..." -ForegroundColor Cyan
Write-Host "Source: $source" -ForegroundColor Gray
Write-Host "Destination: $destination" -ForegroundColor Gray

if (-not (Test-Path $source)) {
    Write-Host "Error: Plugin not found at $source" -ForegroundColor Red
    Write-Host "Build it first with: cmake --build build\webrtc_vst_win --config Release --target webrtc_vst" -ForegroundColor Yellow
    exit 1
}

try {
    Copy-Item -Recurse -Force $source $destination
    Write-Host "Success! Plugin deployed to $destination" -ForegroundColor Green
    Write-Host ""
    Write-Host "Restart Audacity to use the updated plugin." -ForegroundColor Yellow
} catch {
    Write-Host "Error: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Run this script as Administrator:" -ForegroundColor Yellow
    Write-Host "  Right-click PowerShell -> Run as Administrator" -ForegroundColor Gray
    Write-Host "  Then run: .\deploy_plugin.ps1" -ForegroundColor Gray
    exit 1
}
