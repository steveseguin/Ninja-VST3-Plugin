# Advanced deployment settings

Click **Advanced** in the upper-right corner of the plugin to reveal deployment
settings. Click it again to return. The normal editor and window size are unchanged;
every newly opened editor starts on the normal page.

Copy and QR are local editor actions: only an explicit button gesture copies a
link or opens a browser. Host automation, parameter randomization and state
restoration cannot trigger these external side effects.

| Setting | Default | Effect |
| --- | --- | --- |
| Web domain / URL | `https://vdo.ninja/` | Base URL for generated push/view links and QR codes. A bare domain gets `https://`; a full URL can include a self-hosted path. |
| Salt override | Blank (automatic) | Uses the web hostname's VDO.Ninja salt rules. Enter an exact, case-sensitive value to override it. |
| Signaling server (WSS) | `wss://wss.vdo.ninja` | Server used by the existing signaling client. A bare server address gets `wss://`. Paths, ports and query parameters are supported. |

Press Enter or leave a field to finish editing, then click **Apply** to commit
the three advanced fields together. Unapplied drafts do not affect links or the
running session. Applied changes reconnect the plugin's current session.
Save the DAW project/preset to keep the settings. Settings are per plugin
instance, not global; no configuration-file editing or restart is required.
Clear the web URL/server fields to restore their defaults. Clear the salt field
to return to automatic salt selection. URL fields reject credentials, fragments,
unsupported schemes, whitespace and (for the web URL) query strings; invalid text
reverts to the previous value. The editor uses VST3's 127 UTF-16-character text
limit (URLs are additionally limited to 127 ASCII bytes); use punycode for IDNs
and percent-encoded URL paths.

## Example

- Web URL: `https://studio.example.com/ninja/`
- Salt: blank, giving `studio.example.com`; or a deployment's configured salt
- WSS: `wss://signal.example.com/socket`

Changing WSS alone does **not** change the automatic salt for new instances.
Both peers must use the same salt and compatible signaling server. Salt is not a
password and is not secret: generated links include an explicit salt when it
differs from the web domain's default, and include WSS for custom deployments.
Endpoint query parameters will also be included in those links, so do not put
private credentials into shareable server URLs. Password checks in links use the
same effective salt; passwords themselves remain omitted. A password of `0`,
`off` or `false` adds `password=false` so the browser also disables encryption.

Generated links use `&wss2=` to change only the browser's signaling endpoint.
VDO.Ninja's `&wss=` parameter also switches to a different, generic-relay protocol;
that is not the protocol used by this plugin. A custom endpoint must support the
existing VDO.Ninja signaling protocol, not merely accept WebSocket connections.

For the backup service, use web URL `https://backup.vdo.ninja/` and WSS
`wss://apibackup.vdo.ninja/`. Blank salt uses `vdo.ninja`, since this is an official
subdomain. Setting salt to `SALT` instead adds `&salt=SALT` to both link types;
the browser and plugin then share that exact override. The hostname is
`backup.vdo.ninja`, not `backup.vdon.ninja`.

Automatic salt behavior follows the browser reference in `obsninja/webrtc.js`:
official subdomains share their official base-domain salt; `localhost`, IPv4
addresses and `steveseguin.github.io` use `vdo.ninja`; other hosts use their full
hostname. Unicode salts and passwords are supported: AES key phrases use the
browser's UTF-16 low-byte convention, while hashes use UTF-8. The encryption
algorithm itself is unchanged. Mac UI text edits are normalized to NFC, avoiding
Cocoa's decomposed-accent mismatch. Saved JSON/environment values retain their
exact bytes; every peer must use the same value/normalization.

Older projects without these fields retain their previous WSS-derived salt as
an explicit override. Clearing that migrated salt opts into web-domain-derived
behavior. Default VDO.Ninja projects and existing parameter IDs/order are preserved.

## Headless configuration

- `WEBRTC_VST_WEB_BASE_URL`: web base URL
- `WEBRTC_VST_SALT`: salt override (empty means automatic)
- `WEBRTC_VST_HANDSHAKE_URL`: server; legacy alias `WEBRTC_VST_SIGNALING_URL`

Environment variables initialize new instances; restored project state wins.
For compatibility, a WSS-only environment override keeps its legacy WSS-derived
salt. Set `WEBRTC_VST_SALT=` explicitly to select web-domain-derived salt instead.
`ws://` remains supported for loopback CLI tests, but is not accepted in the UI
and has no generated browser link (the browser's server override requires WSS).

State JSON stores `webBaseUrl`, `salt`, and `handshakeUrl`; `signalingUrl` is still
written for backward compatibility. Saved project/preset JSON includes the
password in plaintext, as before; protect those files. Custom WSS query tokens
are included in shared links. Debug/raw-signaling logs can contain identifying
details and should not be shared publicly. Opening a saved Seed project can
publish to its saved endpoint when the host activates it; only open trusted
projects. These settings do not alter signaling message formats.

## TURN relay fallback

For official VDO.Ninja signaling, the plugin obtains TURN servers using the
same `getTURNList` service and `servers[].urls/username/credential` response
as the web client's `webrtc.js`. Discovery runs on the signaling worker,
outside audio/session locks, with bounded HTTP timeouts and a one-hour cache.
If discovery fails, direct/STUN connections remain available.

The current libjuice backend supports **TURN over UDP only**, not TURN over
TCP/TLS. Networks blocking all UDP can still fail even when a browser works.
The plugin preserves data-channel-first negotiation: viewer preferences request
audio and disable video before media tracks are added. Direct-media offers from
native publishers remain supported.

Custom signaling deployments can set `WEBRTC_VST_TURN_SERVER_LIST_URL` to their
own HTTPS endpoint with the same response shape. HTTP is accepted only on
loopback for local testing. An empty override disables discovery.
`WEBRTC_VST_DISABLE_STUN=1` also skips automatic public TURN discovery; an
explicit server-list override still enables the chosen TURN service.
No TURN credentials are written to diagnostic logs.

`npm run test:receive-negotiation` tests encrypted and unencrypted negotiation,
native direct-media offers, real loopback TURN with direct paths blocked, and
continued direct connectivity when discovery fails. Build
`webrtc_vst_integration_test` first; it also builds the local TURN fixture.

### Connection compatibility review (2026-11-08)

Reference: local `code/vdo.ninja` revision `3ea79516`, especially
`webrtc.js` (`getTURNList`, `processTURNs`, `offerSDP`, data-channel handlers,
`processIce`/`processIce2`, reconnect handling) and `main.js` network options.
The SDK is not the protocol reference. This review covers connection setup,
relay selection and recovery; it is not an audit of every web application feature.

| Area | Plugin behavior and remaining differences |
| --- | --- |
| Negotiation | Data channel opens first; Play sends `{audio:true,video:false}`. SRTP is now initialized during the first DTLS handshake so later audio offers work on the same peer. Native direct-media offers also work. |
| ICE candidates | Retains mid information, computes the correct outgoing media-line index, isolates malformed bundle entries, tolerates null mids, and avoids applying explicitly mismatched sessions to the current UUID's peer. Pre-offer queues remain bounded. |
| Liveness | Play now returns the web client's `{pong:ping}` response. This is not a complete ICE-restart implementation. |
| Default STUN | Google `stun.l.google.com:19302` and Cloudflare `stun.cloudflare.com:3478`, matching the web defaults. The pinned libdatachannel/libjuice implementation shuffles the list and chooses one STUN server per peer; this is not multi-server failover. |
| TURN discovery | Official endpoint and timestamp convention; maps `rtc.ninja` and `vdo.socialstream.ninja` web origins as the web client does. Parses string/array URLs and per-server credentials. HTTPS trust/hostname checks, 64 KiB response bound, short timeouts, success cache of one hour, failure cache of 30 seconds. No HTTP on public endpoints. |
| Relay selection | Native allocations are capped at two UDP entries, in response order. The browser uses fresh service results directly, and timezone sorting plus two UDP/one TCP selection for cached/backup lists. Native selection has no timezone ranking or embedded backup credential list. |
| Restricted networks (open) | The pinned libjuice backend cannot provide TURN/TCP or TURN/TLS. A working browser is therefore not proof the native path can connect. A backend change and additional network tests are needed for parity. |
| Privacy/forced relay (open) | No browser-equivalent privacy mode is exposed. Merely filtering signaled candidates with libdatachannel's relay policy does not guarantee all packets use TURN. The relay regression test blocks direct paths externally and counts actual relayed traffic. |
| Recovery (open) | Signaling reconnect reannounces Play/Seed and refreshes discovery. Within a long-lived signaling connection, the relay list is not refreshed for newly created peers. Failed/closed Play peers request play again, but the 30-second cooldown can suppress the only retry; disconnected peers wait for later state transitions. No full browser `iceRestartRequest` exchange yet. |
| Overrides | Native HTTPS server-list override is supported; the full browser URL interface (`stun`, `addstun`, `turn`, `tcp`, `privacy`, speed-test selection) is not implemented. `WEBRTC_VST_DISABLE_STUN` also disables automatic discovery, except when an explicit list override is present. |

Validation includes direct and data-channel-first audio, encrypted negotiation,
mixed video/audio offers, malformed ICE bundles, ping/pong, a real local TURN
server with direct paths blocked, and a discovery outage. The unmodified local
web checkout has also passed synthetic audio in both directions against the
native plugin. Released 0.2.8 failed the forced-relay control with no audio.
The original reporter's installed version remains unconfirmed. A live Android
microphone-only stream supplied during the investigation reproduced the failure:
0.2.8 and the unpublished 0.2.9 candidate reached ICE connected, then timed out
in DTLS with zero decoded audio, while the browser received audio.

### Native Android investigation and WHEP receive

The reported microphone-only app configuration uses the same `vdo.ninja` salt
and WSS endpoint as the plugin. Explicit `:443` is equivalent to the default WSS
port. The Android application's
[`call_sample.dart`](https://github.com/steveseguin/social_ninja/blob/91f09419ef9304f279edb5ce34e3dbdc5c567414/lib/src/call_sample/call_sample.dart#L6968) treats the displayed
`un;pw;turn:turn.x.co:3478` value as a sentinel for automatic TURN discovery, not
as a literal custom relay. This source identifies itself as 5.0.109+109;
the installed app version is still unconfirmed. The user confirmed that
Meshcast is disabled; WHEP is therefore not an explanation for this report.

The confirmed compatibility bug was the plugin's invented `from` UUID. The
official signaling server supplies its own sender `UUID`; Android's
`signaling.dart:onMessage` prefers `from` when present, causing the answer and
candidates to miss the publisher's existing peer. The reference web client's
`webrtc.js:sendMsg` only injects `from` in its separate generic custom-WSS mode.
The plugin implements the server-routed VDO.Ninja protocol, including when its
server URL is overridden; it now omits `from`. Removing that field completed
DTLS and decoded more than 2,000 microphone frames on the same live source.
`receive_negotiation.test.js --sender-routing` models this native routing rule
with synthetic audio and fails against the released 0.2.8 binary.

`WEBRTC_VST_LOG_RTC=1` adds opt-in native transport diagnostics to stdout,
including DTLS handshake errors. Use it together with `WEBRTC_VST_LOG_STDOUT=1`
when reproducing a connection failure. Transport logs can contain network
addresses; avoid enabling raw signaling logs for ordinary support reports.

The Windows installer includes `VDO.Ninja Tools/collect_windows_diagnostics.ps1`.
Close the DAW first, then run this from PowerShell (or use the repository copy):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\collect_windows_diagnostics.ps1 -HostPath 'C:\Program Files\REAPER (x64)\reaper.exe'
```

It opens a fresh host with diagnostics enabled. Reproduce the problem and close
the host normally; the script exports a ZIP under Documents/VDO.Ninja Diagnostics.
Only the last 2,000 recognized connection events and basic host/Windows versions
are retained. It records no audio and exports no stream IDs, passwords, URLs,
addresses, raw signaling or arbitrary host messages. Existing hosts are never
stopped. If the DAW bridges plugins into another executable, capture that host's
output separately and use `-InputLog <log-file>` to export it. Empty captures are
reported explicitly. `tests/windows_diagnostics_test.ps1` checks the export filter.

`npm run test:native-receive` exercises direct microphone-only offers, a custom
password, ICE arriving before SDP, legacy messages without session IDs,
sendrecv audio, reordered/duplicate/stale ICE, and video-first/audio-first offers.
`npm run test:receive-failures` deliberately blocks candidate connectivity and
corrupts a DTLS fingerprint on owned loopback peers. The plugin now logs ICE
state transitions and distinguishes an ICE connection failure from a failure
after ICE connected (DTLS/media). This identifies the failed stage without
assuming every timeout is caused by Windows Firewall.

Initial WHEP receive support follows the real web client's `whepSettings`
control message and `lib.js:whepIn` HTTP exchange. It accepts settings only from
an existing publisher/session, and authenticates the whole encrypted control
envelope for WebSocket fallback. It retains the original data channel, offers
recvonly video/audio sections for A/V relay compatibility, and decodes only
Opus audio. Once WHEP audio is ready, the parent peer's audio is excluded from
the mix to prevent duplication. HTTP runs on a worker; close/replacement joins
it and deletes the allocated HTTP resource on the peer cleanup thread.

HTTPS endpoints and loopback HTTP are supported, with system TLS verification,
bounded responses/timeouts, bearer tokens, duplicate suppression and same-origin
relative/absolute session locations. Redirects are not followed and tokens are
not printed in normal plugin diagnostics. ICE gathering completes before POST;
PATCH trickle/restarts, server counter-offers, legacy `type:meshcast`, screen
WHEP and seamless Meshcast handoff acknowledgements remain outside this initial
implementation. It does not advertise Meshcast switching support.

`npm run test:whep-receive` uses an owned WHEP HTTP/RTC fixture and synthetic
audio/video. It verifies encrypted control fallback, malformed/stale settings,
duplicate/replacement handling, bearer authentication, resource deletion, HTTP
errors, redirects, invalid SDP and oversized responses. These extra suites run
separately from the main integration gate. `npm run test:signaling-fuzz` exercises
4,008 malformed signaling messages and subsequent recovery on isolated peers.

### Optional Windows host firewall rule

The installer offers an **unchecked** task to select the actual DAW/bridge-host
`.exe` and allow its inbound UDP traffic on **Private** networks. It requests
administrator permission for that step only. Normal and silent installation
does not change firewall rules. Declining elevation leaves the plugin installed.
Windows rules apply to the hosting executable, including its other plugins;
they cannot target this VST3 DLL alone (see
[Microsoft's application-rule guidance](https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule?view=windowsserver2025-ps)). UDP ports are dynamic, so the rule is
limited by executable and network profile rather than a fixed port.

The installed helper is in `VDO.Ninja Tools` alongside the plugin bundle.
To inspect the rule without changing anything, from this repository:

```powershell
.\scripts\configure_windows_firewall.ps1 -HostPath 'C:\Program Files\REAPER (x64)\reaper.exe' -PlanOnly
```

Run without `-PlanOnly` in an elevated PowerShell to apply it. Use the same
path/profile with `-Remove` to remove just this helper's rule, even after the
host has been uninstalled. Repeated application updates the same rule. The
installer is not uninstallable, so removal is explicit. `-Profile Public` or
`-Profile Domain` is available only through the helper for an intentional
network-specific choice. Existing block rules and managed firewall policy are
not removed or bypassed. This cannot fix upstream UDP blocking or missing
TURN/TCP support. Dry-run checks: `tests/windows_firewall_test.ps1`.

## Tests

`npm run test:advanced-settings` uses an ephemeral loopback WebSocket server to
check the actual plugin's server path, stream suffixes, room hashes, automatic
salt, explicit salt, and Unicode salt. The native integration suite checks links,
password hashes, legacy migration, input validation, and both component/controller
state round trips. The macOS editor test exercises the Advanced toggle repeatedly.

For live gates without an existing broadcast, run:

```bash
node tools/tests/integration/with_test_publisher.js npm run test:integration
WEBRTC_PUBLISH_TEST_ROOM=1 node tools/tests/integration/with_test_publisher.js npm run test:integration:live
WEBRTC_TEST_SALT=custom-test WEBRTC_TEST_PASSWORD=secret npm run test:publish-audio
```

The publisher wrapper creates its own synthetic audio stream, passes the IDs to
the test command, and disconnects when finished. It does not access a microphone.

To test the real backup web client in both directions, with Chrome installed:

```bash
npm install --prefix build/test-tools/browser --no-package-lock --no-save playwright-core
node tools/tests/integration/browser_advanced_live.test.js
```

This opt-in live check uses an isolated headless Chrome profile and synthetic
audio (no physical microphone/camera). It checks `SALT`, an ASCII salt containing
spaces and reserved URL characters, automatic domain salt, password hashes,
and a mismatched-salt negative control. `view` checks decoded browser audio and
`push` checks the plugin's audio output. Logs/results go to
`build/browser-advanced-validation/`. Set `WEBRTC_TEST_CHROME` to an executable
path if Chrome is not in its usual location. This is not part of offline CI.
Set `WEBRTC_TEST_WEB_SOURCE_DIR` to the local `code/vdo.ninja` checkout to serve
that exact, unmodified web client in the isolated browser while retaining the
configured origin and signaling endpoint. This uses live signaling and synthetic
audio, not the SDK as a protocol substitute.

For a BrowserStack real Android **Chrome source** to the local Windows plugin,
provide `BROWSERSTACK_USERNAME` and `BROWSERSTACK_ACCESS_KEY` in the environment,
then set `WEBRTC_TEST_BROWSERSTACK=1` and `WEBRTC_TEST_CASE=automatic` before
running the same script. `BROWSERSTACK_DEVICE` and `BROWSERSTACK_OS_VERSION`
select the device (defaults: Samsung Galaxy S23 / Android 13.0). It uses the
official Playwright Android connection, real VDO.Ninja signaling, and a synthetic
audio source. `WEBRTC_TEST_WEB_SOURCE_DIR` also works for this route; no local
HTTP server/tunnel is needed. This path requires BrowserStack Automate access.
It passed on Samsung Galaxy S23 / Android 13 Chrome with the unmodified local
web checkout and a custom password (signed 0.2.11 package, received RMS 0.158686). It does not test
the native Android app or establish compatibility with app v109.

`WEBRTC_TEST_CASE=unicode` selects the Unicode case, now included in the default
suite. `WEBRTC_TEST_UI_LINK` can consume the actual native editor's copied link
and exercise the real browser password prompt.

Run workloads sequentially through `node tools/tests/guarded_run.js <command>`.
The guard enforces one workload, a timeout and disk headroom. On macOS it also
checks memory/swap headroom and limits total owned-process RSS; those memory
measurements are not implemented on Windows.
For long tests, set `WEBRTC_GUARD_SECONDS` above the requested duration plus
startup/cleanup time; its default is 240 seconds.
It must not be used to justify running multiple heavy suites in parallel on an
8 GB workstation. See `HARDENING_VALIDATION.md` for the interrupted soak history
and the latest measured results; older validation notes are historical.

For a real DAW check, run `tools/tests/reaper_smoke.lua` in REAPER. It creates a
new project tab with a tone generator, Seed and Play plugin instances, a custom
salt/domain, and a silent master. It checks the WebRTC return level, cycles the
editors/bypass, saves a project and reopens it to verify the settings. Logs and the
test project are written under `build/reaper-validation/`. Other project tabs
are left alone. Repeat on other hosts and physical Intel Macs before extending
the corresponding release validation claims.

### Windows 0.2.11 release validation (2026-11-08)

[Release 0.2.11](https://github.com/steveseguin/Ninja-VST3-Plugin/releases/tag/v0.2.11)
was published from `df221cbeda919a6a353b8cdf25dc02fecb589230` after the
[clean Windows build and all 13 native tests](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/42520197089)
passed. Linux, both macOS architectures and CodeQL also passed; the public assets
for this release are Windows x64 only. JS integration and live-room audio passed.

Downloaded signed artifacts were checked separately from the developer build:

- The installer and ZIP contain the same plugin bytes. Version metadata is
  0.2.11, with no external OpenSSL or Visual C++ runtime DLL dependency. The
  expected signer and timestamp were verified; this workstation does not trust
  the signing root. Both VirusTotal analyses completed with zero malicious or
  suspicious detections; some ZIP scan engines timed out or did not support it.
- REAPER 7.82 passed four simultaneous send/receive pairs, editor/bypass cycles,
  project save/reopen and a fresh host process. Audacity 3.7.9 passed five editor
  open/close cycles and clean shutdown after its first-run dialog was handled.
- The supplied native Android microphone stream produced 39 active one-second
  windows during a 40-second REAPER check. No microphone audio was recorded.
  The installed diagnostic collector exported DTLS completion and decoded-audio
  events, and the host exited cleanly.
- BrowserStack Samsung Galaxy S23 / Android 13 Chrome published synthetic audio
  through the unmodified local web checkout to the downloaded plugin.

Release testing also reproduced heap corruption in SDK 3.8's shared Windows
timer list during concurrent creation/destruction. Windows controller polling
now uses synchronized timer registration and invokes callbacks outside the lock.
The regression covers 200,000 timer lifetimes, owner-thread delivery and
cancellation from a callback. See `THREADING.md` for the lifecycle contract.

Additional tests of the unchanged published binary passed after publication:

- BrowserStack Google Pixel 8 / Android 14 Chrome, received RMS 0.157463.
- Five-minute stereo soak with two forced signaling disconnects: 290 steady
  one-second windows, 54,520 blocks, no silent blocks or nonfinite samples;
  maximum measured tone-frequency error 0.665 Hz.
- Receive negotiation with both forced-TURN routes and discovery failure, plus
  seven WHEP success/error/replacement cases including video and encrypted WSS.
- Actual relayed SRTP with 5% packet loss, 15 ms jitter and a three-second media
  outage, followed by successful recovery.
- 384 peer replacement requests and three subsequent recovery offers while
  live audio remained uninterrupted.
- 8,016 malformed signaling mutations/boundaries across two reproducible fuzz
  seeds in Seed and Play modes, checking status reporting, reconnect, finite
  audio and normal process shutdown.
