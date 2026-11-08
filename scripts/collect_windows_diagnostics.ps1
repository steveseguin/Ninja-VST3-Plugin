# Launch a fresh host for one reproduction, or export a previously captured log.
# Only recognized connection events are retained; no raw host/signaling text is saved.
[CmdletBinding(DefaultParameterSetName='Launch')]
param(
    [Parameter(Mandatory,ParameterSetName='Launch')][string]$HostPath,
    [Parameter(ParameterSetName='Launch')][string[]]$HostArguments = @(),
    [Parameter(Mandatory,ParameterSetName='Export')][string]$InputLog,
    [string]$OutputDirectory = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'VDO.Ninja Diagnostics')
)
$ErrorActionPreference = 'Stop'
$events = New-Object 'System.Collections.Generic.Queue[string]'
$started = [DateTime]::UtcNow
$hostName = 'imported log'
$hostVersion = ''
$exitCode = $null
function Add-DiagnosticEvent([string]$Line) {
    $event = $null
    if ($Line -match '\[WebRTC\] (Connected to VDO\.Ninja signaling server|Signaling connection closed|TURN discovery unavailable; using direct/STUN connectivity|First decoded audio frame received via onFrame|Datachannel opened, sending viewer preferences)\s*$') { $event = $Matches[1] }
    elseif ($Line -match '\[WebRTC\] TURN UDP relays configured: (\d+)\s*$') { $event = 'TURN UDP relays configured: ' + $Matches[1] }
    elseif ($Line -match '\[WebRTC\] ICE state for .*: (new|checking|connected|completed|disconnected|failed|closed)\s*$') { $event = 'ICE state: ' + $Matches[1] }
    elseif ($Line -match '\[WebRTC\] Peer connection (connected|failed|disconnected|closed):') { $event = 'Peer connection: ' + $Matches[1] }
    elseif ($Line -match '\[WebRTC\] Remote audio track (attached|opened|closed)\b') { $event = 'Remote audio track: ' + $Matches[1] }
    elseif ($Line -match '\[WebRTC\] Decoded audio frame count: (\d+)\s*$') { $event = 'Decoded audio frames: ' + $Matches[1] }
    elseif ($Line -match '\[RTC\].*DTLS recv: Handshake timeout\s*$') { $event = 'DTLS handshake timeout' }
    elseif ($Line -match '\[RTC\].*DTLS handshake (finished|failed)\s*$') { $event = 'DTLS handshake: ' + $Matches[1] }
    elseif ($Line -match '\[RTC\].*DTLS alert:') { $event = 'DTLS alert received' }
    elseif ($Line -match '\[RTC\].*SRTP profile is: (SRTP_[A-Z0-9_]+)\s*$') { $event = 'SRTP profile: ' + $Matches[1] }
    elseif ($Line -match '\[WebRTC\] (WebRTCSession::stop\(\) - complete|sessionActive=true|sessionActive=false \(stopSession\))\s*$') { $event = $Matches[1] }
    if ($null -ne $event) {
        $elapsed = [int64]([DateTime]::UtcNow - $started).TotalMilliseconds
        $events.Enqueue(('t+{0}ms {1}' -f $elapsed, $event))
        while ($events.Count -gt 2000) { [void]$events.Dequeue() }
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Export') {
    $reader = [IO.File]::OpenText((Resolve-Path -LiteralPath $InputLog).Path)
    try { while ($null -ne ($line = $reader.ReadLine())) { Add-DiagnosticEvent $line } }
    finally { $reader.Dispose() }
} else {
    $resolvedHost = (Resolve-Path -LiteralPath $HostPath).Path
    if ([IO.Path]::GetExtension($resolvedHost) -ne '.exe') { throw 'Select the DAW or bridge-host .exe.' }
    $hostName = [IO.Path]::GetFileName($resolvedHost)
    $hostVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($resolvedHost).FileVersion
    if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($resolvedHost)) -ErrorAction SilentlyContinue) {
        throw 'Close the existing host first so the new process receives diagnostic settings. This tool does not close existing applications.'
    }
    # Quote individual Windows argv values, including embedded quotes/trailing backslashes.
    function Quote-Argument([string]$Value) {
        return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $resolvedHost
    $info.Arguments = (($HostArguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = Split-Path $resolvedHost -Parent
    $info.EnvironmentVariables['WEBRTC_VST_LOG_STDOUT'] = '1'
    $info.EnvironmentVariables['WEBRTC_VST_LOG_RTC'] = '1'
    $info.EnvironmentVariables['WEBRTC_VST_LOG_SIGNALING'] = '0'
    $info.EnvironmentVariables['WEBRTC_VST_LOG_SIGNALING_RAW'] = ''
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    Write-Host 'Reproduce the connection issue, then close the host normally to export the report.'
    $stdout = $process.StandardOutput.ReadLineAsync()
    $stderr = $process.StandardError.ReadLineAsync()
    try {
        while ($null -ne $stdout -or $null -ne $stderr) {
            foreach ($stream in @('stdout','stderr')) {
                $task = Get-Variable -Name $stream -ValueOnly
                if ($null -eq $task -or -not $task.IsCompleted) { continue }
                $line = $task.GetAwaiter().GetResult()
                $next = $null
                if ($null -ne $line) {
                    Add-DiagnosticEvent $line
                    $next = if ($stream -eq 'stdout') { $process.StandardOutput.ReadLineAsync() } else { $process.StandardError.ReadLineAsync() }
                }
                Set-Variable -Name $stream -Value $next
            }
            Start-Sleep -Milliseconds 20
        }
        $process.WaitForExit()
        $exitCode = $process.ExitCode
    } finally { $process.Dispose() }
}

$reportDir = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('report-' + $started.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void](New-Item -ItemType Directory -Path $reportDir -Force)
$metadata = [ordered]@{ formatVersion=1; createdUtc=$started.ToString('o'); host=$hostName; hostVersion=$hostVersion; windows=[Environment]::OSVersion.VersionString; hostExitCode=$exitCode; eventsRetained=$events.Count; note='Only recognized connection events are included. No microphone audio, stream IDs, passwords, URLs, addresses, raw signaling or arbitrary host log lines are exported. Import timestamps describe export order, not original event timing.' }
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reportDir 'report.json') -Encoding UTF8
$events.ToArray() | Set-Content -LiteralPath (Join-Path $reportDir 'connection-events.txt') -Encoding UTF8
$archive = $reportDir + '.zip'
Compress-Archive -LiteralPath @((Join-Path $reportDir 'report.json'), (Join-Path $reportDir 'connection-events.txt')) -DestinationPath $archive
if ($events.Count -eq 0) { Write-Warning 'No recognized plugin events were captured. Confirm the plugin was loaded in this host process.' }
Write-Output $archive
