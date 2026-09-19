# Hardening and user-journey validation — 2026-09-19

Unreleased working tree, Apple Silicon macOS 26.4.1, 8 GB RAM. Public downloads
are unchanged. This supersedes the feature-stage validation notes. Logs and
captures are retained locally under `build/test-tools/`; synthetic streams and
credentials only, no microphone/camera capture.

## Safety incident and revised execution

An earlier attempt ran native fuzz/stress, live audio, builds and host validation
concurrently while disk headroom was about 2 GB. The machine subsequently had a
watchdog kernel panic. Its report records exhausted compressor segments and low
swap space. This strongly suggests resource pressure; it does not establish an
individual plugin leak. Subsequent testing identified a concrete external
resource-amplification bug: host writes to the QR action parameter launched
browser tabs. Pluginval had opened 199 local QR tabs through the default browser,
outside the validator's child-process tree. This likely contributed to the
earlier pressure, although the panic alone cannot establish that causal chain.
The interrupted native run recorded 84 completed suites
in about six minutes, and the audio run about five minutes. **Neither 30-minute
run passed or completed.**

Resumed validation uses `tools/tests/guarded_run.js`: one workload lock, minimum
20% memory headroom and 8 GiB free disk, bounded swap growth, default 1 GiB
owned-process RSS, and a wall-clock timeout. Normally swap stays below 512 MiB.
After applications close, a stable pre-existing swap baseline up to 2 GiB is
accepted only with ≥50% starting/≥40% running memory headroom and ≤128 MiB growth;
the total 2 GiB ceiling remains. Browser-only runs allow 1.5 GiB with
the same global headroom limits and limited renderer count. The initial 1 GiB
browser run was safely aborted at 1107 MiB; it is not a pass. No disk cleanup or
privileged commands were needed in the resumed run (33 GiB initially free).
Owned-process RSS does not include applications launched through macOS
LaunchServices. The global memory check remains essential, and the Copy/QR
regression intercepts external opens in-process before testing host writes.

## Concerns reviewed

| # | Concern | Fix/evidence and remaining boundary |
|---|---|---|
| 1 | Audio-thread blocking/allocation | Codec, network and status work moved off `process`; bounded preallocated SPSC audio bridge. Native format/burst tests and actual callback timing. ASan/UBSan and TSan settings/bridge tests pass, including 100,000 concurrent receive/reset blocks. Full-plugin allocator instrumentation/TSan still outstanding. |
| 2 | Partial malformed state | Reproduced partial application; bounded candidate parse/atomic commit. Invalid-type/depth/size regression and short-write failure reporting. |
| 3 | QR collisions | Per-controller private immutable files, owner-only directories, eight-page bound and cleanup tests. Actual Mac QR independently decoded with Apple Vision. |
| 4 | Unicode/browser crypto | Reproduced password escaping and AES key-byte mismatch; browser-compatible UTF-16 low bytes for AES, UTF-8 hashes. Actual browser UI-password test; independent crypto vectors. SDK room double-encoding reproduced and fixed. |
| 5 | String registry growth | Replaced global accumulating edit tokens with bounded per-instance strings and serialized messages. 30,000-edit RSS regression; float-token round-trip test. |
| 6 | Remote networks/TURN | Confirmed STUN-only configuration, no TURN. Controlled tests do not validate restrictive NAT/firewalls, mobile networks or geographically remote latency. This remains a deployment limitation. |
| 7 | Recovery | Cancellable IX retry ownership/backoff; idle Seed/Play reconnect regression. Live interruptions exposed duplicate peers/amplification; fixed healthy-Play preservation and same-stream replacement. Final five-minute strict audio soak passed with signaling interruptions at 20/40 seconds and zero silent blocks after warmup. |
| 8 | Full user workflow | Actual Cocoa text entry/Apply/Copy, independent QR decode, real backup website password dialog and browser-to-plugin audio passed. REAPER final journey recorded separately below. |
| 9 | Atomic advanced changes | Drafts do not alter current links/session; explicit Apply commits web/salt/WSS together. Two-instance state/message regression. Exhaustive live endpoint-change race coverage remains open. |
| 10 | Sustained playback quality | New timestamped real-media probe measures one-way marker delay, variation, silence, clipping and steady-tone residual. Found/fixed try-lock dropouts and insufficient scheduling reserve; results below. |
| 11 | Formats/transport edges | 128 combinations of rates/blocks/mono/stereo; finite samples, in-place processing, silence flags, invalid setup, sample64 rejection, surround rejection. Actual REAPER exposed prefetch incorrectly treated as offline: corrected and `IPrefetchableSupport` now requests real-time processing. Offline bounce still does not send DAW audio. Full live rate-change/clock-drift matrix remains open. |
| 12 | Rooms/peer limits | Salted Unicode-password live room passed. 16/17-peer and 1024-candidate flood exposed a certificate/worker-pool deadlock; explicit negotiation outside the lock fixes it. Pending ICE capped at 256 with bounded diagnostics. Sixteen simultaneous audible remote streams not measured. |
| 13 | Input/TLS limits | 16 KiB state, 32 JSON nesting levels, 256 KiB signaling application limit, strict UTF-8/URLs/ports/IPv6 checks. Self-signed TLS rejection passed. IX still assembles frames before the application size check; hostile transport-level memory bounds remain open. |
| 14 | State/host portability | Serializable configuration messages, independent instances, legacy migrations, preset round trips and short-write errors covered. Full separate-process controller sandbox/host undo matrix not available. |
| 15 | Platforms/hosts | Final arm64 and Rosetta x86_64 native gates/pluginval pass. Physical Intel, macOS 11, Windows, Studio One/Audacity and AU remain unverified/out of scope here. |
| 16 | Packaging/install | Local installer preserves prior bundle; final development bundle installed for host testing. Current development install is ad-hoc signed, not a notarized public release. Final Developer ID packaging, quarantine/clean-machine and VirusTotal checks remain release gates. |
| 17 | Privacy/trust | Parse errors redact payloads; Copy omits passwords; private QR files. Projects still save plaintext passwords, custom WSS query tokens are shareable, trusted saved Seed projects can publish on activation. Documented, not silently changed. |
| 18 | Host-driven external UI actions | Reproduced 200 intercepted QR launches from 200 host parameter writes. Copy/QR now run only from an actual native editor gesture, never host parameter writes. Fixed test: zero launches/clipboard mutations; real Copy/QR still pass, including independent QR decode. |

## Playback probe: observed regression and fix

Two actual VST3 instances in separate CLI-host processes: Seed PCM → Opus →
encrypted WebRTC → Play PCM. Real backup signaling; direct same-machine media,
48 kHz stereo, 256-frame host blocks. Both ends have monotonic block timestamps.
The probe uses 1000/1500 Hz tones with known amplitude markers. It does not
measure audio-interface ADC/DAC delay or a remote WAN path.

Initial 45-second run: median 56.4 ms, five fully silent host blocks after warmup.
Two-block SPSC reserve: median 55.4 ms, two silent blocks coinciding with ~18 ms
host scheduling pauses followed by catch-up calls. Four-block/minimum-1024-frame
reserve removes these dropouts in the subsequent 90-second run:

| Metric | 90-second result |
|---|---:|
| One-way latency, median / p95 | 67.80 / 68.72 ms |
| Latency min–max | 66.63–68.82 ms |
| Observed marker variation | 2.19 ms across 40 markers |
| Silent / clipped / non-finite output blocks | 0 / 0 / 0 |
| Steady blocks checked | 15,187 |
| Left tone residual, median / p95 | 0.282% / 0.544% |
| Right tone residual, median / p95 | 0.790% / 0.990% |

Source: `p0-playback-final-measurement.log` and
`playback-probe-1789854965105/results.json`. Level-edge latency is resolved at
host-block granularity (~5.33 ms); variation is **not** an RTP network-jitter
measurement. Tone residual is a least-squares sinusoidal residual over stable
10 ms plateaus, excluding intentional amplitude transitions; it is not a full
perceptual music/speech quality or frequency-response test. Opus is lossy: these
measurements must not be described as zero distortion or universally zero jitter.

## Final P0/P1 gates

| Priority / journey | Result | Evidence |
|---|---|---|
| P0 actual Mac fields → Apply → Copy → QR | Passed after external-action fix | `p0-qr-regression-after.log`; 200 host writes inert, genuine clicks work, exact link recovered with Apple Vision; original clipboard restored |
| P0 copied link → real backup website → password dialog → Play audio | Passed on corrected build | `p0-qr-fixed-browser-ui.log`, RMS 0.168957; browser-only guard used 1.5 GiB cap |
| P0 real backup browser ↔ plugin, custom salt and wrong-salt rejection | Passed | `p0-qr-fixed-browser-custom.log`: browser decoded RMS 0.2364; plugin RMS 0.1684; mismatched salt received zero packets |
| P0 encrypted stereo playback quality | Passed, measured scope above | `p0-playback-final-measurement.log`, 90 seconds, zero silent/clipped/non-finite blocks after warmup |
| P0 REAPER Seed/Play, live bypass/editor cycling, saved-project reopen | Passed on installed corrected build | `build/reaper-validation/result.txt`, 21 sustained windows; `p0-final-reaper.log`, peak owned RSS 234 MiB, minimum memory headroom 69% |
| P1 full native settings/integration/stress/fuzz/Cocoa suites after fixes | Passed, all five CTest suites | `p1-all-final-arm64-ctest.log`, 16.37 seconds, including concurrent RX/reset and inert host-driven Copy/QR regressions |
| P1 final pluginval strictness 10 | Passed after external-action fix | `p1-pluginval-qr-fixed.log`: all tests passed, peak owned RSS 95 MiB, minimum headroom 67%, zero QR tabs afterwards |
| P1 final full JS, recovery, signaling limits, TLS and salted room | Passed | `p1-qr-fixed-js.log`, `p1-qr-fixed-hardening.log`, `p1-qr-fixed-tls.log`, `p1-qr-fixed-room.log`; peak owned RSS 362 MiB, minimum headroom 68% |
| P1 prefetch live audio and offline isolation | Passed | `p0-prefetch-quality.log`: p95 68.36 ms, zero silent/clipped/non-finite blocks; `p1-offline-isolation.log`: offline Seed dry audio preserved, no audio transmitted to real-time Play |
| P1 refreshed Intel build/native tests/pluginval 10 under Rosetta | Passed | `p1-final-intel-build.log`, `p1-final-intel-ctest.log` (all five), `p1-final-intel-pluginval.log` |
| P1 ASan/UBSan and TSan settings/audio bridge | Passed | `p1-asan-rx.log`, `p1-tsan-rx.log`; separate instrumented binaries, 100,000 TX and RX/reset blocks, 10,000 state mutations |
| P1 five-minute strict stereo audio soak, two signaling interruptions | Passed | `p1-final-audio-soak.log`, `audio-soak-1789856965624/results.json`; 54,520 steady-state blocks, zero silent/non-finite blocks, maximum callback 63.08 μs, maximum measured pitch error 0.665 Hz |
| P1 30-minute sanitizer-assisted fuzz/lifecycle soak | Passed, 1806.143 seconds uninterrupted | `p1-final-extended.log`, `extended-1789857275930/results.json`; 173 cycles / 692 suites, zero failures, 1.73 million randomized state mutations; peak guarded workload 214 MiB, minimum memory headroom 65% |

The fresh isolated REAPER test initially hit startup/evaluation-dialog timing;
the harness now delays until host startup settles. The subsequent **real plugin
failure** (dry Seed audio but no network frames/Play audio) was reproducible under
REAPER's prefetch processing. Opting out of prefetch and distinguishing it from
offline bounce restored live and reopened-project audio. No anticipative track
override was applied to make the test pass.

The final pluginval attempts stopped at swap/headroom guard limits. Investigation
found **199 test-generated QR tabs**, not unrelated user browsing: non-automatable
action parameters were still executed when a host wrote them. Only those exact
local QR tabs were closed, restoring 67% headroom. The UI-only action fix has a
safe red/green regression with `NSWorkspace.openURL` intercepted, so even a failing
test cannot launch tabs. Evidence: `p0-qr-regression-before.log` (200 requests),
`p0-qr-regression-after.log` (zero host effects and genuine UI actions pass).
Cold swap remains at ~1 GiB; the stricter stable-baseline guard is retained.
Do not label interrupted runs passes.

The replacement prolonged run **completed**, independently of the earlier
interrupted attempts. Each of 173 cycles ran ASan/UBSan-instrumented settings/
audio-bridge tests, native host integration, seeded host fuzz and lifecycle
stress. The results file reports `complete: true`, required duration 1800 seconds,
actual duration 1806.143 seconds and zero failed suites. Individual native child
processes peaked at 53.36 MiB RSS; the whole guarded workload peaked at 214 MiB.
The plugin SHA-256 was checked after every suite and remained unchanged. This
is repeated lifecycle/mutation validation, not a claim that one live plugin
instance streamed audio for 30 minutes; continuous live audio was tested for
five minutes separately, with the stricter zero-silent-block assertion enabled.

Installed/built arm64 plugin SHA-256:
`dd8f3e98bf84b907fef53cd8b60ea6e444710838759a84c48049ffd22cf834a5`.
`WEBRTC_CLI_HOST_PROCESS_MODE` provides dedicated prefetch/offline media probes;
both passed on the current plugin. JavaScript syntax checks and `git diff --check`
passed after these test additions. No public upload, notarization or release
claim is made.
Final cleanup confirmed no validation processes or workload lock remain, zero
test QR tabs are open, 70% memory headroom and approximately 30 GiB free disk.
The user's original REAPER session was left running. The installed artifact's
signature verifies locally and its binary hash matches the tested build above;
restart/rescan an existing host to load it. The previous installation was saved
under `~/Library/Application Support/VDO.Ninja/VST3 Backups/webrtc_vst.7Lgala/`.

## Remaining boundaries before a broad release claim

- Remote WAN, restrictive NAT/TURN, injected media loss/reordering, long-term
  independent audio-device clock drift, live sample-rate changes and sixteen
  simultaneously audible peers are not certified by these same-machine tests.
- The latency/residual figures cover synthetic 48 kHz stereo and 256-frame host
  blocks, not physical ADC/DAC, a listening evaluation, all buffer sizes, or
  perceptual speech/music fidelity. Arbitrary CPU starvation can still underrun.
  Real website E2E used Chrome; Safari, Firefox and mobile browser journeys
  were not covered in this run.
- Full-plugin/third-party-stack sanitizers and real-time allocation tracing,
  transport-level frame assembly bounds, other DAWs, physical Intel hardware,
  oldest supported macOS and Windows regression execution remain open.
- The installed development artifact is ad-hoc signed. Developer ID signing,
  notarization, clean-machine/quarantine installation and VirusTotal submission
  remain distribution gates; public downloads were not changed by these tests.

## Reproduce safely

```bash
node tools/tests/guarded_run.js ctest --test-dir build/webrtc_vst_mac -C Release --output-on-failure --parallel 1
node tools/tests/guarded_run.js npm run test:hardening
WEBRTC_PROBE_SECONDS=90 node tools/tests/guarded_run.js npm run test:playback-quality
WEBRTC_GUARD_SECONDS=420 WEBRTC_SOAK_SECONDS=300 WEBRTC_SOAK_RECONNECT=1 WEBRTC_SOAK_STRICT=1 node tools/tests/guarded_run.js npm run test:audio-soak
WEBRTC_GUARD_SECONDS=2100 WEBRTC_EXTENDED_SECONDS=1800 node tools/tests/guarded_run.js npm run test:extended
```

For the sanitizer-assisted variant used above, build the standalone settings/
bridge binary first (it does not load/instrument the full plugin), then set
`WEBRTC_SANITIZED_SETTINGS=build/test-tools/settings-sanitized` on the extended
command:

```bash
node tools/tests/guarded_run.js c++ -std=c++20 -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -g tests/settings_unit_test.cpp -I vst3sdk -I build/webrtc_vst_mac/_deps/nlohmann_json-src/include -o build/test-tools/settings-sanitized
node tools/tests/guarded_run.js c++ -std=c++20 -O1 -fsanitize=thread -g tests/settings_unit_test.cpp -I vst3sdk -I build/webrtc_vst_mac/_deps/nlohmann_json-src/include -o build/test-tools/settings-tsan
node tools/tests/guarded_run.js build/test-tools/settings-tsan
```

Do not start another workload until the previous guard exits. A stale lock must
be inspected for an active owner before removal. The runner does not grant
permission to terminate unrelated applications or delete user files.
