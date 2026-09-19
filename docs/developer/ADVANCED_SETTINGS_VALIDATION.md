# Advanced settings validation — 2026-09-19

Historical feature-stage report. The Unicode restriction and immediate-apply
behavior described below were subsequently fixed. Current behavior and testing
are in [ADVANCED_SETTINGS.md](ADVANCED_SETTINGS.md) and
[HARDENING_VALIDATION.md](HARDENING_VALIDATION.md).

Unreleased working-tree changes based on `a73eb46`. Public v0.2.4 downloads have
not been replaced by this feature. Tested on Apple Silicon, macOS 26.4.1.

| Check | Result |
| --- | --- |
| Apple Silicon Release build | Pass |
| Intel Release build | Pass; execution under Rosetta, not a physical Intel Mac |
| Native integration, stress, fuzz, Cocoa editor suites | All four pass on both architectures |
| Advanced native integration cases | Custom/automatic/legacy salts, password hashes, push/view URLs, component/controller state round trips, endpoint validation |
| Cocoa interaction | Five editor lifecycles, twenty Advanced open/close cycles, domain field becomes editable |
| pluginval 1.0.4, strictness 5, GUI tests enabled | Pass on arm64 and x86_64 |
| Custom WebSocket endpoint | Loopback server receives the specified path/query and expected stream/room hashes; Unicode salt preserved |
| Plugin → independent JS SDK, custom salt/password/domain | Pass; decoded audio RMS 0.1531 |
| Full `npm run test:integration` with owned SDK tone publisher | Pass |
| Live-room receive gate with owned SDK tone publisher | Pass; decoded audio RMS 0.16865 |
| REAPER 7.80, native Apple Silicon | Two instances, Seed tone → WebRTC → Play return for 25 seconds; return peak 0.25435 |
| REAPER UI/bypass/project persistence | Repeated editor open/close and bypass; saved project reopened with custom salt/domain intact |

REAPER exposed recursion in the initial unbound Advanced-button implementation.
The local control is now detached from VST parameter binding; the Cocoa interaction
test covers that regression. The button also has a dark, high-contrast background.

The pre-existing receive gate initially had no active reference stream. The new
`with_test_publisher.js` helper supplies an owned synthetic stream. Direct-stream
tests and room tests use separate publisher sessions because joining a room changes
direct-stream discovery on the signaling service.

Local logs are under `build/test-tools/`; the REAPER project and result log are under
`build/reaper-validation/`. REAPER and pluginval were installed in `/Applications`.
Test audio is synthetic; the REAPER test project's master is silent.

Not covered: Windows/Linux execution for these changes, physical Intel hardware,
older macOS versions, additional DAWs, long-duration reliability, or notarization.
No public release or website download was changed in this task.

## Backup-service browser follow-up

Tested the real `https://backup.vdo.ninja/` web client with
`wss://apibackup.vdo.ninja/` in Chrome 153.0.8010.50 on Apple Silicon. The supplied
`backup.vdon.ninja` hostname did not resolve. Browser tests use a fresh headless
profile, a synthetic audio source, and muted local output; no camera/microphone
is captured. These checks load the plugin in the CLI host, not REAPER.

The first browser run exposed a generated-link bug: `&wss=` selects the browser's
generic-relay protocol, which rejects messages addressed using the native
signaling server's conventions. Changing only the endpoint requires `&wss2=`.
Generated links now use `wss2`; native integration tests assert both backup
push/view links and their password hashes. No plugin signaling logic changed.

The independent JS SDK received encrypted plugin audio through the backup server
with a custom salt/password (RMS 0.1587). The browser also received plugin audio
and published audio back to the plugin with `&salt=SALT`. A wrong-salt viewer
received zero audio packets during the eight-second negative-control window.

Final live results (all six audio paths and the mismatch control passed):

| Salt setting | Plugin → browser decoded RMS | Browser → plugin output RMS |
| --- | --- | --- |
| `SALT` | 0.23624 | 0.17055 |
| `mix &+/=?#-ASCII` | 0.19630 | 0.16973 |
| Blank, automatically `vdo.ninja` | 0.20015 | 0.17082 |

After the link fix, all four native suites and pluginval strictness 5 passed on
both Mac architectures (Intel under Rosetta). The final bundles were Developer
ID signed; the arm64 user installation was refreshed with the prior install
preserved. This follow-up still does not publish or notarize a new release.

**Known browser incompatibility, not a passing case:** the salt `mix &+/=?#-é`
matched stream/password hashes but failed encrypted SDP/ICE decryption. Browser
`convertStringToArrayBufferView` truncates JavaScript character codes to bytes;
the plugin/SDK derive the AES key from UTF-8. For example, `é` becomes `e9` in
the browser versus `c3 a9` in UTF-8. The plugin continues to preserve Unicode for
state/SDK compatibility, but the editor and guide now advise ASCII salts for
browser peers. Changing crypto encoding is outside this endpoint/salt feature.

`tools/tests/integration/browser_advanced_live.test.js` provides repeatable live
coverage; its default cases cover ASCII salts, including spaces and reserved
punctuation, and automatic domain salt. The optional `WEBRTC_TEST_CASE=unicode`
case reproduces the known incompatibility and fails rather than masking it.
Detailed logs are in `build/browser-advanced-validation/` and
`build/test-tools/backup-*.log`.
