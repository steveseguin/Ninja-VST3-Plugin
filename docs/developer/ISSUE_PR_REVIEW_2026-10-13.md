# Issue and PR review — 2026-10-13

Reviewed against `main` at `cb63ae0978e3d0bb4b25dee42e53bdc323ea4080`.
Reports and PR descriptions were treated as hypotheses. Changes below were
checked against production code and exercised locally on Windows x64 with
MSVC 2022, Release configuration, and VST3 SDK 3.8.0 build 66.

## Disposition

| Item | Independent finding | Change |
| --- | --- | --- |
| [PR #3](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/3) | The interpolation loop omits valid fractional positions at block ends. The regression compiled against the original production implementation fails with “Resampled length depends on host block size.” | Incorporated the loop correction, extracted production resampler, and continuity/frame-count regressions. |
| [PR #4](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/4) | The receive ring holds host-rate frames but was fixed at 2,048 frames. A 20 ms packet at 192 kHz already exceeds that capacity. The original fixed sizing fails the two-packet regression. | Incorporated host-rate capacity and pre-fill scaling, preserving existing minimums below 48 kHz. Added production ring tests from 8 to 384 kHz. |
| [PR #5](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/5) | The original analyzer accepts alternating clipped plateaus, including right-channel-only clipping, because their RMS falls outside its selection band. | Incorporated analysis of every timed plateau while retaining a separate expected-level coverage requirement. Added the offline regression command to macOS CI. |
| [Issue #6](https://github.com/steveseguin/Ninja-VST3-Plugin/issues/6) | Complete source and released 0.2.5 Windows bundles open at 720×340. The released inner binary alone has a 0×0 editor in the native host and a tiny 26×0 client area in Ableton Live 12.4.6. The fixed binary alone opens at 720×340 in both hosts. | Embedded the Windows layout as a fallback when its external file is missing; clarified copying the whole bundle; added native Win32 editor tests with and without external resources. This reproduces the symptom through a verified installation failure path; the reporter's precise installation and unspecified Live 12 Beta build remain unknown. |
| [Closed PR #2](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/2) | Its proposed ws 8.20.1 update is superseded by the current 8.21.3 dependency. | Kept the current dependency; installed-dependency security tests pass. |
| [Merged PR #1](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/1) | Historical automation change; its Claude workflow is no longer present on current main. | No application bug fix needed. |

Reviewed PR heads: #3 `442001f45cf3cf6534a8b324d0a34cb69c04caec`,
#4 `1f3380105c85e59cce2bbba287662dd8eab9ad65`,
#5 `f4001162c4433778d7d20ab5bd5b3dfd73f41635`.
The combined changes are on the
[validation branch](https://github.com/steveseguin/Ninja-VST3-Plugin/tree/codex/validated-issue-fixes-20261013).
They do not merge or close those GitHub items.

## Conversation and review follow-up

Rechecked GitHub issue/PR conversations, submitted reviews, inline review
comments, resolved/unresolved threads, and current PR checks after the Ableton
testing. The repository still has five PRs (three open) and one ordinary issue.
All pages were retrieved. Issues and PRs contain **three conversation comments**;
all five PRs have **zero submitted reviews and zero inline review threads/comments**. Open
PRs #3, #4, and #5 have no conversation comments. Remote main and their head
commits still match the revisions recorded above.

| Comment | Independent assessment | Action |
| --- | --- | --- |
| [PR #1 workflow review](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/1#issuecomment-3380628003) | The bot flags missing comment-write permission and suggests timeouts, concurrency, tool restrictions, and filtering for the Claude workflows. Both referenced workflows are absent from current main. Its claims that token permissions imply a read-only filesystem and version tags constitute strong action pinning should not be treated as security guarantees. | Obsolete for this codebase. No workflow or permission change needed. |
| [PR #2 Dependabot notice](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/2#issuecomment-5715237896) | Informational notice about closing a grouped update, not a defect report. Installed `ws` is 8.21.3, newer than the proposed 8.20.1. | No further dependency change needed for this comment. |
| [Issue #6 acknowledgement](https://github.com/steveseguin/Ninja-VST3-Plugin/issues/6#issuecomment-6147831733) | Maintainer acknowledgement repeats the initial report; it adds no reproduction details or new evidence. | The local resource fallback and Ableton evidence below address the reproduced installation failure. A follow-up with a distributable fixed build and a request to retest remains appropriate; the reporter's exact cause is unconfirmed. |

The open PR descriptions also call out macOS CI failures. These were checked
against downloaded job logs, rather than accepted from the descriptions:

- [PR #3 ARM64](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/37207798538/job/111452546316)
  fails `MacEditorSmoke` with `Advanced domain field did not become editable`.
  Its other five native tests pass; Intel passes the full job.
- [PR #4 Intel](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/37208239079/job/111453871427)
  fails with the same message. Its other five native tests pass; ARM64 passes
  the full job.
- [The base commit's ARM64 job](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/35481036463/job/105998765947)
  has the same editor failure. This establishes that the failure predates
  these DSP changes; it does not establish whether the cause is the Cocoa
  editor, test event/focus handling, or runner conditions.
- [PR #5](https://github.com/steveseguin/Ninja-VST3-Plugin/pull/5) currently has
  passing ARM64, Intel, and CodeQL checks. CodeQL also passes on #3 and #4.

The follow-up below investigates the Cocoa test on actual macOS runners.
No additional application behavior change was justified by the comments.
No replies were posted and no threads were resolved.

Audit metadata: [comment/review snapshot](validation/2026-10-13-github-comments.json).

### Cocoa editor test follow-up

The first Apple Silicon run reproduced the Advanced-field failure. Changing
only event-loop pumping did not fix it. A second diagnostic read the actual
`NSTextView` contents and recorded a cold-run failure with `VSTGUI_NSView`
still holding focus; the later 20 repetitions passed on each architecture.
This isolated a timing assumption in the host test, rather than establishing
a plugin settings defect.

The test clicked a field, waited a fixed 50 ms, and assumed Cocoa had finished
focusing it. The pinned SDK [defers text-field focus on the main queue](https://github.com/steinbergmedia/vstgui/blob/76823bdbe286e4bdb9f79ab8986af5ce7202336c/vstgui/lib/platform/mac/cocoa/cocoatextedit.mm#L191).
The candidate now waits up to one second for the expected editable field,
dispatches queued Cocoa events, and queues both halves of each mouse gesture
before processing them. It reads the complete field-editor string when
checking/replacing text. The original value, Unicode editing, Apply, Copy,
independent QR decoding, host-side-effect and reopen assertions remain.

The [validation workflow](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38507298437)
tests source commit `2f59aa743a060fcb3d090b9cd913ffd03b05bd52` and compares the
original test with 20 repetitions of the candidate on each architecture.
The first Intel attempt encountered a runner DNS failure cloning libdatachannel;
it did not reach compilation. The retry passed the complete workflow.

Apple Silicon passed all seven native tests, all 15 offline tests, and every
security/signaling/WebRTC workflow step. In the same job, the original editor
test failed once in five repetitions and the revised test passed all 20.
The final repetition's retained log records that it needed the asynchronous
focus wait before completing all actual text-edit and QR assertions.
Intel also passed all seven native tests, all 15 offline tests, the complete
security/signaling/WebRTC workflow, and 20 editor repetitions. Its original
test passed all five baseline repetitions, consistent with an intermittent
failure. Across both architectures, the revised test passed **40/40** repeated
runs plus both initial native-suite runs. The temporary comparison steps were
removed from the final workflow; the regular native and JS gates remain.
Evidence: [macOS UI validation](validation/2026-10-13-macos-ui.json).

### Synthetic sender timing follow-up

The existing integration publisher had the same callback-counted sender that
underfed the first Ableton probes. Running the original sender callback with
15.625 ms timer intervals produces only 0.64 seconds of audio per second.
The shared monotonic-time sender produces the full second, maintains phase
through delayed callbacks, and does not emit early when callbacks repeat.
Both the integration publisher and manual DAW probe now use this helper.

All 15 offline regressions pass, including three timing cases. A real Windows
`RTCAudioSource` accepted 10 seconds of tone in 10.0069 seconds (99.93% pacing).
The owned-room integration smoke also passes with decoded RMS 0.171816 and
99.93% sender pacing. Its required `WEBRTC_PUBLISH_TEST_ROOM=1` setting must be
present; an initial invocation without it had no remote peer and was corrected.
These are test-harness changes, with no plugin audio or feature changes.
Evidence: [sender pacing](validation/2026-10-13-sender-pacing.json).

## Validation

- Full Windows plugin, CLI host, and native test binaries built successfully.
- All eight native checks passed across the final native and editor runs:
  receive buffer, resampler, settings, integration, stress, fuzz, bundled editor
  and editor without resources. An initial CTest-only editor failure was traced
  to CMake's inherited hidden-window startup flag; the test host now explicitly
  makes its window visible. Rendering assertions and timeouts remain enforced.
- Native resampler and receive-buffer regressions fail with original behavior
  and pass with the changes. The resampler test covers planar/interleaved ramp
  and sine continuity, different block sizes, reset and single-frame calls.
- Offline playback-analysis regressions: 12/12 pass. On the original analyzer,
  both alternating-clipping rejection cases fail because no exception is thrown.
  Three additional baseline assertions depend on Node's assertion-message
  formatting; they are not counted as additional production bugs.
- `npm run test:integration` passed with an owned synthetic tone publisher:
  WebSocket security, SDK behavior, advanced settings, audio-only signaling,
  rejection of offered video tracks, plugin loopback, and plugin-to-SDK audio.
- `npm run test:hardening` passed: browser-compatible crypto vectors, reconnects,
  malformed/oversized signaling, peer limits and teardown.
- `npm run test:integration:live` passed with an owned room and synthetic tone.
  The integration tests use real public signaling and synthetic audio; they
  do not capture a microphone or establish physical audio-device performance.
- The Windows editor host checks initial size, actual rendered pixels, editable
  fields, repeated Advanced toggles, mode changes, 125%/150%/200% scaling,
  rejection of a 64×64 size request, and five editor reopen cycles. The same
  checks run against a copied module with no external resource directory.
- The published Windows 0.2.5 ZIP was also downloaded and its complete bundle
  passed the native editor checks before the fallback change.

### Playback quality: retain both results

The first 45-second real plugin-to-plugin probe failed the unchanged 1% median
tone-residual limit on the right channel: **1.137% median**, 1.431% p95.
The left channel measured 0.381% median / 0.457% p95. Latency p95 was 60.57 ms.
This run overlapped other validation work; that does not establish the cause.

A repeat passed: **63.79 ms latency p95**, 0.69 ms spread, left/right median
residuals 0.404% / 0.840%, and p95 residuals 0.741% / 1.199%. Both runs measured
522 plateau windows per channel and had zero silent, non-finite or clipped
blocks after warmup. This is a 48 kHz, 256-frame, same-machine measurement,
excluding audio devices and WAN media paths. It is not an unconditional
quality or latency guarantee.

Machine-readable results: [first run](validation/2026-10-13-playback-first.json),
[repeat](validation/2026-10-13-playback-repeat.json). No test thresholds were relaxed.
Investigating why the right-channel median straddles 1% remains follow-up work;
there is insufficient evidence here to change codec settings or audio behavior.

A subsequent diagnostic isolated the same production-built Opus library from
the plugin and networking. With its default stereo encoder and 20 ms packets,
the right-channel median reached **1.204%** solely by changing the alignment of
the 10 ms analysis windows; 111 of 480 tested alignments exceeded 1%. The
aligned-window checks across 60 input offsets had all stayed below 1%.
This shows that the gate can fail for the codec alone. It is consistent with,
but does not establish the exact cause of, the original live result. The
measurement method and acceptable fidelity target need review before treating
that result as a new plugin defect. Neither codec defaults nor thresholds were
changed. [Diagnostic method and results](validation/2026-10-13-opus-window-diagnostic.json).

## Reproduce locally

```powershell
cmake -S webrtc_vst -B build/webrtc_vst_win -G 'Visual Studio 17 2022' -A x64 `
  '-DVST3_SDK_ROOT=C:/path/to/vst3sdk' `
  '-DOPENSSL_ROOT_DIR=C:/vcpkg/installed/x64-windows-static' `
  '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' '-DSMTG_CREATE_PLUGIN_LINK=OFF'
cmake --build build/webrtc_vst_win --config Release --parallel 4 --target `
  webrtc_vst_cli_host webrtc_vst_resampler_test webrtc_vst_peer_audio_buffer_test `
  webrtc_vst_settings_test webrtc_vst_integration_test webrtc_vst_stress_test `
  webrtc_vst_fuzz_test webrtc_vst_windows_editor_test
ctest --test-dir build/webrtc_vst_win -C Release --output-on-failure --parallel 1
npm.cmd ci
npm.cmd run test:offline
node tools/tests/integration/with_test_publisher.js npm.cmd run test:integration
npm.cmd run test:hardening
$env:WEBRTC_PUBLISH_TEST_ROOM = '1'
node tools/tests/integration/with_test_publisher.js npm.cmd run test:integration:live
npm.cmd run test:playback-quality
```

The quoted policy option supports the local CMake 4 installation while avoiding
PowerShell's parsing of an unquoted dotted option value. CMake 3.31 is used by
the existing Windows release workflow.

## Limits and artifacts

### Ableton trial follow-up

At the user's request, Ableton Live **12.4.6 Trial, Windows x64** was downloaded
from [Ableton's official trial page](https://www.ableton.com/en/trial/) and
installed in `C:\ProgramData\Ableton\Live 12 Trial`. The installer and installed
`InstallHelper.exe` both have valid Ableton AG Authenticode signatures. Setup
reported success and did not require a restart.

First launch waited for Windows permission to run Ableton's registration
helper, then completed. Testing used Live's offered **Save/Export Disabled**
demo mode, without an Ableton account sign-in or trial activation.

Each retained editor result used a separate custom VST3 folder under the ignored
`build/ableton-trial/plugins/`, a fresh Live process, and verification of the
loaded module path. Early folder-switch attempts that retained the previous
path were excluded. All four module hashes match the values below.

| Installed variant | Editor client size in Live | Result |
| --- | --- | --- |
| Released 0.2.5 complete bundle | 720×340 | Controls render normally. |
| Released 0.2.5 inner file alone | 26×0 | Unusable 42×39 outer window, essentially just its close button. |
| Fixed inner file alone | 720×340 | Fields and Advanced view work; five close/reopen cycles pass. |
| Fixed complete bundle | 720×340 | Five close/reopen cycles, Advanced view, and two simultaneous instances pass. |

This validates the resource fallback in a real Ableton host. It does not prove
that the reporter copied only the inner file, or establish compatibility with
the unspecified Live 12 Beta build in issue #6. Measurements here use 100%
Windows scaling; other scales were exercised in the native test host.

The fixed complete bundle also passed a real DAW audio route:
**SDK synthetic 440 Hz → Live Play → Live Publish → SDK receiver**. Live ran at
44.1 kHz with MME/DirectX and hardware input disabled. Each retained paced run
measured 1,000 ten-millisecond return frames after three seconds of warmup:

- 512-sample output buffer: RMS 0.17168, zero-crossing estimate 440.2 Hz.
- 4,096-sample output buffer: RMS 0.17174, estimate 440.9 Hz.
- Reusable probe at 4,096 samples: RMS 0.17151, estimate 443.0 Hz.

All three passed the same smoke thresholds with no silent frames or clipping.
This establishes functional receive/publish routing, not distortion, latency,
stereo fidelity, or device-driver certification. The earlier stricter CLI
quality investigation above remains open.

Initial audio probes failed because their `setInterval(10)` sender supplied
only about **70.1%** of real-time audio on this machine. Reducing Live's output
buffer alone did not fix that. Pacing the sender by elapsed monotonic time
restored approximately 99.9% delivery and the same audio checks passed, with
no additional production audio changes. The failed pacing diagnostic is
retained alongside the passing results; it is not counted as a plugin defect.

Evidence: [editor matrix](validation/2026-10-13-ableton-ui.json),
[audio results and pacing diagnosis](validation/2026-10-13-ableton-audio.json),
and [Ableton screenshots](../../media/gallery/README.md#ableton-live-1246).
The reusable [DAW probe](../../tools/tests/manual/daw_roundtrip_probe.js) creates
an owned synthetic sender and measures its return through the two plugin
instances. Run it, configure the displayed input/output IDs in the respective
Play/Publish instances on one audio track, and start Live processing:

```powershell
node tools/tests/manual/daw_roundtrip_probe.js --host-rate 44100 --buffer-size 4096
```

Keep hardware input off, both passwords empty, and default VDO.Ninja endpoint
settings. The host-rate and buffer arguments document settings observed in
Live; the script does not configure or independently measure those settings.
Use `--input`, `--output`, and `--out` to repeat an already configured owned
test route. Results are written under `build/daw-roundtrip/` by default.

### Remaining limits

No REAPER, Studio One, or Audacity validation was performed. Ableton testing
covered the editor and synthetic audio route above; saving, reloading sets,
and export were unavailable in the selected demo mode. Native host tests
substitute for specific VST3 operations, not complete DAW compatibility.
High host rates were tested in the production DSP/ring code and host setup
tests; the live quality probe runs at 48 kHz. Windows was built locally and
macOS validation ran on GitHub's Apple Silicon and Intel runners, as detailed
above. Linux builds and macOS DAW tests were not performed.

The [media gallery](../../media/gallery/README.md) contains actual editor
screenshots and a short silent recording. The local development binary is not
a signed release. No release, system plugin deployment, GitHub merge, or issue
closure is part of this review. Ableton was installed only for the requested
host testing.

Windows module SHA-256:

- Downloaded 0.2.5: `55e00cb554776e3616cad4e84713b33a266948c48f5adb622eabde3cfa7c67f7`
- Reviewed build: `2f34a905704ba81cea8f977543d62f785791d5bc8c1e1d1597edbbb540fcf83c`

## Packet-loss validation follow-up (2026-10-14)

A deterministic harness now feeds real Opus packets into the production session
receive function and pulls audio through the production resampler, per-peer
ring, and concealment path. It does not duplicate these algorithms in the test.
The existing track callback was extracted into a private member method; a
friend test fixture supplies owned peers without contacting signaling servers.

**Confirmed defect:** a partial DAW callback discarded the rest of a generated
20 ms PLC frame. For the same 100 ms outage at 48 kHz, one large pull generated
five PLC frames, but 64-sample callbacks generated 75 (1,500 ms internally), and
256-sample callbacks generated 19 (380 ms). Output samples and subsequent
recovery depended on callback partitioning. The same final test program was run
against the pre-fix receive/pull code and the fixed code: the baseline fails
118 of 126 partition cases, 11 scenario concealment budgets, and the mid-frame
arrival check. Those failures are retained, not counted as a passing gate.

**Fix:** enqueue every resampled PLC sample in the peer's existing bounded ring.
A partial callback leaves the rest ahead of any newly arriving packet. Decode,
resample, and enqueue now share the context lock so incoming packets cannot be
interleaved with concealment between those operations. Interleaved ring writes
avoid converting every decoded packet into newly allocated planar arrays.
Encoder settings, FEC, packet duration, pre-fill thresholds and buffer capacities
are unchanged.

Validation on Windows x64 / Release:

- All 126 block-partition cases pass: nine host rates from 8 to 384 kHz,
  mono/stereo, and seven callback sizes from 32 to 4,096 frames. Concealment and
  recovery samples exactly match the large-pull reference, and each 100 ms
  outage generates five 20 ms PLC packets.
- Clean 48 kHz audio matches an independent Opus decoder; arrival during a
  partial concealed frame, independent peers, and concurrent receive/pull pass.
- Twenty-one virtual ten-second scenarios exercise regular 5% packet loss,
  100 ms loss bursts, 12 ms arrival jitter, 30 ms reorder events, duplicate
  payload delivery, a one-second outage, and clean traffic at three block sizes.
  Outputs remain finite/unclipped and recover audible tail levels. At 64-frame
  callbacks, 5% loss now needs 25 PLC packets instead of 375; the one-second
  outage needs 50 instead of 750.
- The nine native Windows checks and all 15 offline JavaScript tests pass.
- A subsequent 90-second real WebRTC soak passes its normal level, frequency,
  finite-output and dropout-rate checks, but fails the optional strict zero-silence
  check: one silent block among 15,040 steady blocks. This is retained as a failed
  strict result pending diagnosis; the deterministic PLC fix does not establish
  dropout-free operation of the entire host/worker/transport path.

Evidence: [baseline](validation/2026-10-14-audio-impairment-baseline.json),
[fixed](validation/2026-10-14-audio-impairment-fixed.json), and
[test source](../../tests/audio_impairment_test.cpp).

### Limits of the impairment checks

The packet schedule is applied at the Opus payload boundary, after transport
processing. It does not test SRTP replay protection, ICE, actual UDP scheduling,
WAN performance, or a real DAW under impaired networking. Reordering and
duplicate scenarios check bounded, finite playback and recovery, not correct
RTP chronology or music fidelity. The current onFrame path ignores RTP timestamps
and libdatachannel's pinned Opus depacketizer does not reorder packets. A bounded
packet jitter/reorder layer remains follow-up work before adding FEC recovery.
In the 1,024-frame callback simulation, 12 ms jitter still generates 29 PLC
packets without intentional packet loss; extra concealment and buffer overflow
under that schedule need attention in that broader playout design.

FEC remains disabled. The earlier strict distortion measurement caveat and the
unperformed DAW save/export and macOS DAW tests still apply. No new UI assets are
needed for this audio-only change.
