param(
    [Parameter(Mandatory=$false)]
    [string]$BuildDir = "build"
)

$fullPath = Resolve-Path $BuildDir
Get-ChildItem $fullPath -Recurse -Filter *.vcxproj | ForEach-Object {
    $content = Get-Content $_.FullName
    $content = $content -replace 'MultiThreadedDebugDLL','MultiThreadedDebug'
    $content = $content -replace 'MultiThreadedDLL','MultiThreaded'
    Set-Content $_.FullName $content
}
