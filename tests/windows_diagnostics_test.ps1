$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$work = Join-Path $repo ('build/diagnostics-test-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $work -Force)
$inputFile = Join-Path $work 'input.log'
$lines = @(
    '[WebRTC] Connected to VDO.Ninja signaling server',
    '[WebRTC] ICE state for PRIVATE_UUID:PRIVATE_SESSION: connected',
    '[RTC] function: DTLS recv: Handshake timeout',
    '[WebRTC] Peer connection failed: PRIVATE_UUID',
    '[WebRTC] Remote audio track attached for peer PRIVATE_UUID',
    '[WebRTC] First decoded audio frame received via onFrame',
    '[WebRTC] Decoded audio frame count: 1200',
    '[WebRTC] <= signaling: {"password":"PRIVATE_SECRET","url":"https://PRIVATE_HOST/?token=PRIVATE_TOKEN"}',
    'arbitrary host text PRIVATE_SECRET'
)
$lines | Set-Content -LiteralPath $inputFile -Encoding Unicode
$zip = & (Join-Path $repo 'scripts/collect_windows_diagnostics.ps1') -InputLog $inputFile -OutputDirectory $work
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    if ($archive.Entries.Count -ne 2) { throw 'Unexpected export files' }
    $all = ''
    foreach ($entry in $archive.Entries) {
        $reader = New-Object IO.StreamReader($entry.Open())
        try { $all += $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    if ($all -match 'PRIVATE_|https://|"password"|arbitrary host text') { throw 'Sensitive text escaped into export' }
    if ($all -notmatch 'ICE state: connected' -or $all -notmatch 'DTLS handshake timeout' -or $all -notmatch 'Decoded audio frames: 1200') { throw 'Missing diagnostic evidence' }
} finally { $archive.Dispose() }
$lines = 1..2010 | ForEach-Object { '[WebRTC] Decoded audio frame count: ' + $_ }
$lines | Set-Content -LiteralPath $inputFile -Encoding UTF8
$boundedZip = & (Join-Path $repo 'scripts/collect_windows_diagnostics.ps1') -InputLog $inputFile -OutputDirectory $work
$archive = [IO.Compression.ZipFile]::OpenRead($boundedZip)
try {
    $reader = New-Object IO.StreamReader($archive.GetEntry('connection-events.txt').Open())
    try { $retained = $reader.ReadToEnd().Trim() -split '\r?\n' } finally { $reader.Dispose() }
    if ($retained.Count -ne 2000 -or $retained[0] -notmatch 'frames: 11$' -or $retained[-1] -notmatch 'frames: 2010$') { throw 'Export did not retain the bounded recent event window' }
} finally { $archive.Dispose() }
Write-Output 'PASS: exported diagnostic stages preserve evidence and exclude raw/private text'
