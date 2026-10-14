# macOS seeded fuzz, audio discontinuity and error-recovery validation

This extends [the previous validation](MACOS_FOLLOWUP_2026_10_14.md) on the same
M1 Mac. Checks are short and sequential under the resource guard; the earlier
30-minute soak was not repeated. Logs and diagnostic stacks remain in
`build/test-tools/fuzz-audio-validation/`.

## Reproduced defects and fixes

1. **Peer replacement deadlock.** Signaling fuzz stalled subsequent valid
   negotiation. A sampled stack showed signaling holding the session lock while
   clearing `onLocalCandidate`, and an RTC candidate callback holding its
   callback lock while waiting for the session lock. Peer retirement now removes
   routing and marks audio inactive immediately, then a joined cleanup worker
   resets callbacks and closes resources outside both locks/callback contexts.
   Outstanding retirement capacity is bounded; active peers retain their routing.
   The diagnostic is `stall-sample.txt`, with the failing capture under
   `signaling-fuzz-1791997737793`.
2. **libdatachannel callback-assignment race.** Once teardown could progress,
   TSan caught concurrent copies of the stored callback's pending-event state
   during `Channel::resetCallbacks()`. In pinned libdatachannel 0.22.1, hidden
   base assignment operators cause temporary derived-wrapper assignment outside
   the base lock. An idempotent CMake patch routes callable/null assignments
   through the synchronized base. A focused concurrent assignment/replay test
   and the real signaling fuzz cover this. Original diagnostic:
   `signaling-fuzz-1791998345662/seed.log`.
3. **Optional console logging race.** TSan found plugin workers and the CLI host
   accessing shared `std::cout` formatting state concurrently. Plugin diagnostic
   lines now use one stdio write instead. The text and opt-in behavior remain
   unchanged. Original diagnostic: `signaling-fuzz-1791998251135/seed.log`.
4. **Misleading error status.** A stream-ID conflict was overwritten almost
   immediately by `Publishing audio` while the worker prepared outgoing frames.
   A rejected publish now suppresses that success message until a fresh publish
   announcement. Disconnection status also distinguishes a connected peer from
   a peer that exists but has not connected. Audio routing/codec behavior is
   unchanged by these status fixes.

The first fuzz fixture allowed mutated identities to occupy additional peer
slots. Constraining well-typed identities isolated parser/replacement behavior
from the separately tested 16-peer cap; the deadlock still reproduced. The
dependency patch's first draft exposed an overload ambiguity with library lambdas
at compile time; the final patch handles callable assignment directly.
An integration invocation also failed because its default public stream was
offline; the complete gate was rerun with `with_test_publisher.js`, the existing
owned synthetic-publisher fixture.

## Coverage

- Five reproducible seeds (`1`, `42`, `5653583`, `3735928559`, `4294967295`) for
  15,070 saved-state cases through both actual VST interfaces and 50,000 parser
  mutations. Rejections must preserve prior state; accepted state must save and
  restore canonically. Invalid UTF, types, mode consistency, endpoints, size and
  nesting are included.
- 2,004 signaling mutations/boundary inputs per mode per run, followed by valid
  negotiation, forced disconnect/reconnect, controller error/recovery status and
  normal teardown. All malformed signaling stays on an owned local server.
- 5,000 random/damaged Opus payloads at the production receive boundary, with
  finite-output, queue-bound and valid-audio recovery checks. Existing PLC
  partition, packet impairment, peer isolation and concurrent receive/pull checks
  remain enabled.
- A short constant stereo-tone capture examines every steady-state sample and
  host block boundary. A second-order sinusoidal predictor detects isolated
  discontinuities that a one-second RMS measurement could miss. The residual
  threshold is 0.001 full scale; controls inject a 0.002 spike and a phase splice.
  A five-second WAV of the measured return is saved with the capture.

## Final results

All final gates passed. [Machine-readable results](validation/2026-10-14-macos-fuzz-audio.json)
record the tested binaries and measurements. The installed Release binary matches
SHA-256 `d44ae6b1f01a37c8bdc0a5e1683d2622bf923bd0c3762fb7f8fc915fd9db1e4d`.

| Check | Result |
| --- | --- |
| Native suites / offline checks | 10/10 suites; 21/21 checks |
| Steinberg extensive validator | 537 passed, zero failed |
| pluginval | Strictness 10, two randomized repeats passed |
| State/parser fuzz under ASan/UBSan | 65,070 cases passed across five seeds |
| Signaling fuzz | 4,008 inputs each in Release, ASan/UBSan and TSan passed |
| Decoder impairment under ASan/UBSan | 126 partition cases, 21 scenarios and five specific checks, including 5,000 malformed Opus payloads |
| TSan lifecycle and callback assignment | Four lifecycle suites plus concurrent assignment/replay passed |
| Actual SRTP loss/jitter/outage | Release and TSan recovered after 5% loss, 15 ms jitter and a three-second outage |
| Full JS integration and hardening | Passed with owned publisher fixture |
| REAPER 7.80 arm64 | Eight instances/four live pairs; bypass, editor cycles, project reload and fresh-process reopen passed |

The 32-second stereo capture analyzed 1,007,872 frames per channel after warmup:
2,015,744 samples, with zero suspicious discontinuities at the 0.001 full-scale
threshold. Maximum predictor residual was 0.00002214; maximum block-boundary
residual was 0.00001476. The steady capture had no silent blocks, clipping or
nonfinite values. Worst measured process callback was 25.5 microseconds. The
five-second audition file remains locally at
`build/test-tools/audio-soak-1791998857632/received-stereo.wav`.

A separate 48 kHz / 256-frame playback probe measured median local one-way
latency 63.71 ms, p95 64.22 ms, across eight markers. Median residual THD+N was
0.31% left and 0.76% right; 3,000 steady blocks had no silence, clipping or
nonfinite samples. These measurements include codec and buffering delay.

Whole CLI-host peak RSS was 22.39 MiB for Play and 22.66 MiB for Seed. CPU time
was 0.73 seconds over 27.10 seconds wall time for Play, and 1.13 over 34.09 for
Seed. Before these changes, the corresponding measurements were 22.39/22.53 MiB
and 0.77/1.22 CPU seconds. Baseline latency p95 was 61.95 ms. These single runs
show similar resource use; they do not establish a speedup or explain the small
latency difference. Measurements include host pacing and capture, rather than
isolating plugin CPU. Retirement is now bounded under hostile replacement
traffic; no DSP or codec optimization was introduced in this round.

Studio One and Audacity were unavailable on this Mac. The previously completed
30-minute soak applies to the prior artifact; this final artifact received the
short checks above.

## Reproduction

After building with `CMAKE_BUILD_PARALLEL_LEVEL=1 bash scripts/build_macos.sh`,
run workloads sequentially:

```bash
node tools/tests/guarded_run.js npm run test:offline
node tools/tests/guarded_run.js npm run test:hardening
node tools/tests/guarded_run.js npm run test:audio-clicks
node tools/tests/guarded_run.js npm run test:media-faults
node tools/tests/guarded_run.js npm run test:playback-quality
node tools/tests/guarded_run.js node tools/tests/integration/with_test_publisher.js npm run test:integration
```

See [test tooling](../../tools/tests/README.md) for sanitizer build selection,
seed controls, native state-only fuzz and REAPER validation. Raw logs are local
artifacts; the checked-in JSON retains the final measurements.

## Additional short validation after 4db12c4

The same Release, ASan/UBSan and TSan binaries were exercised again, without
production-source changes. CI for 4db12c4 also passed macOS Build And Test and
CodeQL. [Additional measurements](validation/2026-10-14-macos-extra.json) retain
the format probes, peer-replacement captures and fuzz results.

A new owned-localhost fixture holds a live Seed/Play pair while sending three
bursts of 128 requests replacing a separate peer. Release produced 382 distinct
replacement offers, TSan 381 and ASan/UBSan 379. Every run negotiated a fresh
peer after each burst and exited normally. Each analyzed 960,000 frames per
channel: 5,760,000 samples across the three runs, with no detected tone
discontinuities, nonfinite values or clipping. The measured live-audio windows
had no silent blocks. The runs peaked at 153, 347 and 393 MiB of combined owned
workload RSS respectively; this includes the test harness and both hosts.

All six real-media format probes passed their existing latency, distortion,
silence and clipping gates:

| Sample rate / host block | Local one-way latency p95 |
| --- | --- |
| 44.1 kHz / 128 | 71.17 ms |
| 48 kHz / 32 | 68.31 ms |
| 48 kHz / 256 | 66.88 ms |
| 48 kHz / 1,024 | 148.29 ms |
| 96 kHz / 64 | 69.49 ms |
| 192 kHz / 256 | 64.70 ms |

The larger callback uses the existing four-block scheduling reserve and its
correspondingly higher latency allowance; passing does not imply imperceptible
delay. These are same-machine measurements, excluding physical devices/WAN.

The TLS fixture now also checks controller-visible error status. Its initial
assertion expected the words certificate/SSL/TLS; macOS actually displayed
`OSStatus error -9807`. The fixture was corrected to accept the native error
format while independently requiring an attempted TLS connection and zero
accepted WebSocket connections. No trust settings were changed.

Two additional signaling-fuzz seeds passed: `20261014` under TSan and
`3735928559` under ASan/UBSan, for 8,016 additional malformed/boundary inputs
across Seed and Play. Both runs verified error visibility, reconnect and clean
teardown. An offline Seed probe preserved dry host audio and produced no audio
in the real-time Play receiver. No new plugin defects were found in this round;
only test coverage and the validation record changed.

Reproduce the new sustained-audio peer replacement check with
`node tools/tests/guarded_run.js npm run test:peer-churn-audio`. See the test
tooling README for selecting the TSan build. Each run lasts about 28 seconds;
no additional long soak was performed.

## Mac v0.2.6 release package validation

Both final DMGs are Developer ID signed, Apple-notarized and stapled. Apple's
logs report `Accepted` with no issues. Local verification and the independent
GitHub release job verified the exact hashes, signatures, architecture, version,
stapled tickets and Gatekeeper acceptance. See the
[artifact record](validation/2026-10-14-macos-release-0.2.6.json).

The source tag `mac-v0.2.6` identifies `b0836a6`; subsequent release-workflow and
website changes do not modify the plugin. The Apple Silicon build is the tested
production build above, with its distribution signature added. Intel was rebuilt
from the same runtime source and passed all ten native suites under Rosetta.

Both disk images were mounted read-only, their plugins copied into isolated test
folders, and quarantine attributes applied to those copies. Each final signed
plugin passed 15 native integration cases, strictness-10 pluginval, 384 peer
replacement requests with uninterrupted measured stereo audio, and REAPER
editor/bypass/saved-project reload checks with 21 sustained audio windows. The
exact plugin path was checked in REAPER to exclude another installed copy.
The signed Apple Silicon artifact also passed the owned-publisher live-room
gate, including audio-only preferences, datachannel mapping and decoded output
(RMS 0.168857).

The Intel standalone Cocoa test initially could not activate its window. A
foregrounded rerun passed; the local launcher was also corrected to tolerate
the test process exiting before its final focus request. Intel REAPER initially
waited for permission to install its optional ReWire component, before loading
the VST3. That request was cancelled; no administrator credentials or ReWire
installation were needed. REAPER's evaluation dialog was dismissed before the
successful test journey. Its optional FFmpeg lookup found an incompatible
Homebrew architecture, but the VST3 audio checks passed. The first Intel packaging
attempt lacked the shared dependency-license path; the final package includes
the licenses and passed all verification. These were host/packaging setup issues,
not additional plugin fixes.

The release workflow now waits for completed VirusTotal results rather than
publishing on submission alone. Both final DMGs completed with zero malicious
or suspicious detections (60 and 61 undetected results respectively). Analysis
URLs remain in the workflow/local verification logs, outside public release notes.
The verification run is `38652439283`; local evidence is under
`build/test-tools/release-026/`. Tests ran sequentially behind the resource guard,
which waited when concurrent work reduced startup memory headroom.

| Final DMG | SHA-256 |
| --- | --- |
| Apple Silicon | `02c54d5e74dddc136b5e241a8b6f727972e68116e0b1aa390bb436d7f0a531b9` |
| Intel | `db0bea2dd04da47e131f80317f2d389053cc8461ed0aedad29c83333b3caedd3` |

## Scope

These are objective synthetic audio checks, not a subjective speaker/headphone
listening evaluation. The native-rate tone predictor is not a perceptual model
for speech/music. Same-machine tests exclude physical clock drift, device/DAC
latency and WAN/NAT behavior. Loss/concealment can still be audible. Malformed
Opus tests are after the SRTP authentication boundary; they do not show corrupt
network packets bypass authentication. Sanitizers cover built dependencies,
excluding prebuilt OpenSSL; no suppressions are used, and ASan leak detection is
disabled. No test result proves absence of every crash, race or audible artifact.
