# Read-only checks: fail if the helper reaches any firewall command.
$ErrorActionPreference = 'Stop'
function Get-NetFirewallRule { throw 'Unexpected firewall read/mutation during dry run' }
function New-NetFirewallRule { throw 'Unexpected firewall mutation' }
function Set-NetFirewallRule { throw 'Unexpected firewall mutation' }
function Remove-NetFirewallRule { throw 'Unexpected firewall mutation' }
$helper = Join-Path $PSScriptRoot '../scripts/configure_windows_firewall.ps1'
$hostExe = Join-Path $env:WINDIR 'System32/notepad.exe'
$plan = & $helper -HostPath $hostExe -PlanOnly
if ($plan.Profile -ne 'Private' -or $plan.Direction -ne 'Inbound' -or
    $plan.Protocol -ne 'UDP' -or $plan.Program -ne $hostExe -or $plan.Action -ne 'Allow') {
    throw 'Default rule is not limited to the selected executable / Private / inbound UDP'
}
$again = & $helper -HostPath $hostExe.ToUpperInvariant() -PlanOnly
$public = & $helper -HostPath $hostExe -Profile Public -PlanOnly
$remove = & $helper -HostPath $hostExe -Remove -PlanOnly
if ($again.Name -ne $plan.Name -or $remove.Name -ne $plan.Name -or $public.Name -eq $plan.Name) {
    throw 'Rule identity is not stable across repeated add/remove calls or separate by profile'
}
foreach ($bad in @('relative.exe', 'C:relative.exe', '\Windows\System32\notepad.exe',
    'C:/missing-host-for-vst-test.exe', 'C:/plugin.vst3', 'C:/wild*.exe')) {
    $rejected = $false
    try { & $helper -HostPath $bad -PlanOnly | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Accepted invalid host path: $bad" }
}
& $helper -HostPath $hostExe -WhatIf
& $helper -HostPath 'C:/uninstalled-vst-test-host.exe' -Remove -WhatIf
Write-Output 'PASS: scoped firewall rule, repeatable add/remove identity, invalid paths, mutation-free dry runs'
