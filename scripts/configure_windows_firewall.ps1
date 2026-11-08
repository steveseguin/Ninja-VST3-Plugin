# Run elevated to change rules. -PlanOnly and -WhatIf never change the firewall.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostPath,
    [ValidateSet('Private', 'Public', 'Domain')]
    [string]$Profile = 'Private',
    [switch]$Remove,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
# Use a literal absolute executable path. Removal also works after uninstalling
# the DAW, provided the same path/profile is supplied.
if ($HostPath -notmatch '^(?:[A-Za-z]:[\\/]|[\\/]{2}[^\\/]+[\\/][^\\/]+[\\/])') {
    throw 'HostPath must be an absolute path to the DAW or plugin-host executable.'
}
$program = [System.IO.Path]::GetFullPath($HostPath)
if ([System.IO.Path]::GetExtension($program) -ine '.exe' -or $program.Contains('*') -or $program.Contains('?')) {
    throw 'Choose the .exe hosting the plugin, such as reaper.exe, not the VST3 bundle.'
}
if (-not $Remove -and -not (Test-Path -LiteralPath $program -PathType Leaf)) {
    throw "Host executable does not exist: $program"
}
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $key = [System.Text.Encoding]::UTF8.GetBytes($program.ToLowerInvariant() + '|' + $Profile.ToLowerInvariant())
    $digest = ([System.BitConverter]::ToString($sha.ComputeHash($key))).Replace('-', '').Substring(0, 24)
} finally { $sha.Dispose() }
$rule = @{
    Name = "VDO.Ninja-VST-UDP-$digest"
    DisplayName = "VDO.Ninja VST - $([System.IO.Path]::GetFileName($program)) - $Profile UDP"
    Group = 'VDO.Ninja VST host access'
    Description = 'Optional host-specific WebRTC audio access. Covers this executable, including its other plugins.'
    Program = $program
    Direction = 'Inbound'
    Protocol = 'UDP'
    LocalPort = 'Any'
    RemoteAddress = 'Any'
    Profile = $Profile
    Action = 'Allow'
    Enabled = 'True'
    PolicyStore = 'PersistentStore'
}
if ($PlanOnly) { [pscustomobject]$rule; return }
$operation = if ($Remove) { 'Remove VDO.Ninja UDP firewall rule' } else { 'Allow inbound UDP for this host' }
if (-not $PSCmdlet.ShouldProcess("$program ($Profile networks)", $operation)) { return }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try { $admin = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
finally { $identity.Dispose() }
if (-not $admin) { throw 'Run this script in an administrator PowerShell window to change firewall rules.' }

# Only touch our deterministic rule; never remove pre-existing host/block rules.
$existing = Get-NetFirewallRule -PolicyStore PersistentStore -Name $rule.Name -ErrorAction SilentlyContinue
if ($Remove) {
    if ($existing) { $existing | Remove-NetFirewallRule -ErrorAction Stop }
} elseif ($existing) {
    $update = $rule.Clone()
    $update.Remove('DisplayName')
    $update.Remove('Group')
    Set-NetFirewallRule @update -ErrorAction Stop | Out-Null
} else {
    New-NetFirewallRule @rule -ErrorAction Stop | Out-Null
}
