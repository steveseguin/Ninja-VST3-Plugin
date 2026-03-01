# Deploy WebRTC VST plugin
# -Scope User   : no admin required (default)
# -Scope System : requires Administrator

param(
    [ValidateSet("User", "System")]
    [string]$Scope = "User"
)

$ErrorActionPreference = "Stop"

$source = "build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3"
$destination = if ($Scope -eq "System") {
    "C:\Program Files\Common Files\VST3\"
} else {
    Join-Path $env:LOCALAPPDATA "Programs\Common\VST3\"
}

Write-Host "Deploying WebRTC VST Plugin..." -ForegroundColor Cyan
Write-Host "Scope: $Scope" -ForegroundColor Gray
Write-Host "Source: $source" -ForegroundColor Gray
Write-Host "Destination: $destination" -ForegroundColor Gray

if (-not (Test-Path $source)) {
    Write-Host "Error: Plugin not found at $source" -ForegroundColor Red
    Write-Host "Build it first with: cmake --build build\webrtc_vst_win --config Release --target webrtc_vst" -ForegroundColor Yellow
    exit 1
}

try {
    if (-not (Test-Path $destination)) {
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
    }
    Copy-Item -Recurse -Force $source $destination
    Write-Host "Success! Plugin deployed to $destination" -ForegroundColor Green
    Write-Host ""
    Write-Host "Restart Audacity to use the updated plugin." -ForegroundColor Yellow
} catch {
    Write-Host "Error: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Close any DAW/host currently using the plugin and retry." -ForegroundColor Yellow
    Write-Host ""
    if ($Scope -eq "System") {
        Write-Host "Either run as Administrator or use user scope:" -ForegroundColor Yellow
        Write-Host "  .\deploy_plugin.ps1 -Scope User" -ForegroundColor Gray
        Write-Host "Admin option:" -ForegroundColor Yellow
        Write-Host "  Right-click PowerShell -> Run as Administrator" -ForegroundColor Gray
        Write-Host "  Then run: .\deploy_plugin.ps1 -Scope System" -ForegroundColor Gray
    } else {
        Write-Host "If user-scope deploy is blocked, try admin/system scope:" -ForegroundColor Yellow
        Write-Host "  .\deploy_plugin.ps1 -Scope System" -ForegroundColor Gray
    }
    exit 1
}
