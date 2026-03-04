param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$VstBundlePath = "build\\webrtc_vst_win\\VST3\\Release\\webrtc_vst.vst3",
    [string]$OutputDir = "build\\release",
    [string]$OutputBaseName = ""
)

$ErrorActionPreference = "Stop"

function Resolve-IsccPath {
    if (-not [string]::IsNullOrWhiteSpace($env:ISCC_PATH) -and (Test-Path $env:ISCC_PATH)) {
        return (Resolve-Path $env:ISCC_PATH).Path
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\\Inno Setup 6\\ISCC.exe",
        "$env:ProgramFiles\\Inno Setup 6\\ISCC.exe",
        (Join-Path (Join-Path $projectRoot "build\\tools\\InnoSetup6") "ISCC.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "ISCC.exe not found. Install Inno Setup 6 (example: choco install innosetup -y)."
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$issPath = Join-Path $projectRoot "installer\\windows\\webrtc_vst_installer.iss"

if (-not (Test-Path $issPath)) {
    throw "Installer script not found: $issPath"
}

$bundleAbsolute = if ([System.IO.Path]::IsPathRooted($VstBundlePath)) {
    (Resolve-Path $VstBundlePath).Path
} else {
    (Resolve-Path (Join-Path $projectRoot $VstBundlePath)).Path
}

if (-not (Test-Path $bundleAbsolute)) {
    throw "VST bundle not found: $bundleAbsolute"
}

$normalizedVersion = if ($Version.StartsWith("v")) { $Version.Substring(1) } else { $Version }
$tagVersion = if ($Version.StartsWith("v")) { $Version } else { "v$Version" }

if ([string]::IsNullOrWhiteSpace($OutputBaseName)) {
    $OutputBaseName = "webrtc_vst-windows-setup"
}

$outputAbsolute = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir
} else {
    Join-Path $projectRoot $OutputDir
}

New-Item -ItemType Directory -Path $outputAbsolute -Force | Out-Null

$iscc = Resolve-IsccPath

& $iscc `
    "/DMyAppVersion=$normalizedVersion" `
    "/DSourceVstBundle=$bundleAbsolute" `
    "/DOutputDir=$outputAbsolute" `
    "/DOutputBaseFilename=$OutputBaseName" `
    $issPath

if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
}

$installerPath = Join-Path $outputAbsolute "$OutputBaseName.exe"
if (-not (Test-Path $installerPath)) {
    throw "Installer output not found: $installerPath"
}

Write-Host "Installer created: $installerPath"
