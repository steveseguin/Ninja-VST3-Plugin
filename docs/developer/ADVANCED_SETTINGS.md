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
projects. This does not add TURN or alter signaling message formats.

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
`WEBRTC_TEST_CASE=unicode` selects the Unicode case, now included in the default
suite. `WEBRTC_TEST_UI_LINK` can consume the actual native editor's copied link
and exercise the real browser password prompt.

Run workloads sequentially through `node tools/tests/guarded_run.js <command>`.
The guard checks memory/swap/disk headroom and limits total owned-process RSS.
It must not be used to justify running multiple heavy suites in parallel on an
8 GB workstation. See `HARDENING_VALIDATION.md` for the interrupted soak history
and the latest measured results; older validation notes are historical.

For a real DAW check, run `tools/tests/reaper_smoke.lua` in REAPER. It creates a
new project tab with a tone generator, Seed and Play plugin instances, a custom
salt/domain, and a silent master. It checks the WebRTC return level, cycles the
editors/bypass, saves a project and reopens it to verify the settings. Logs and the
test project are written under `build/reaper-validation/`. Other project tabs
are left alone. Repeat on other hosts and physical Intel Macs before release.
