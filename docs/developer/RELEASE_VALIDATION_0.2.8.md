# v0.2.8 Windows and Linux release validation

Validated on 2026-10-14–15 against downloaded CI packages. Release source/tag:
`6e7aab1a941ad4d732fc5b54b19ad15b6a91837c` / `v0.2.8`.
The release stayed in draft during package and host validation. The earlier
v0.2.7 draft was superseded before publication. macOS distribution remains
separate; this release does not replace the notarized Mac packages.

## Verified fixes

- **Linux editor lifetime:** reopening an editor could abort inside Cairo
  after VSTGUI disconnected a cached XCB connection. Retain the Cairo device
  for the connection lifetime and finish it before disconnecting. Overlapping
  editors keep their shared connection valid.
- **Linux redraw:** VSTGUI's synchronous XCB flush could consume expose events
  without waking the host's event loop. Flush requests without reading replies.
  The regression checks actual painted pixels without artificial wakeups.
- **Linux frame-only event loops:** Audacity 3.7.9 provides `IRunLoop` through
  the plugin frame. VSTGUI initialized X11 before installing that loop and
  crashed on first open. Build a corrected copy with those operations ordered
  properly, without modifying the SDK checkout. Both host arrangements are tested.
- **Receive recovery bursts:** a partially consumed PLC frame followed by
  three real packets could overflow the old 2,048-frame minimum receive ring
  and produce a phase jump. The minimum is now 4,096 frames, scaled with sample
  rate. The existing prefill is unchanged. A deterministic real-Opus regression
  fails before the change and becomes bit-exact afterward, for mono/stereo at
  44.1, 48, 96, 192 and 384 kHz.

An earlier Linux peer-churn CI run caught the audio discontinuity, and a local
repeat reproduced it. After the buffer fix, three consecutive local runs and
both final Linux CI runs passed the 384-request recovery test.

## Packages and security checks

| Artifact | SHA-256 |
| --- | --- |
| Windows VST3 ZIP | `1026bd709625fd4948f92f1a1b154f9f84e190496c05615cdb9565e375fcaba3` |
| Windows installer | `af5aee49158c69c3eca1303111914d9eb68b164128e51a72397f1a4da7986314` |
| Linux x86-64 archive | `0e56075f487fc82c054eec05479461a43afded488df8174e0b1b203343b93b96` |
| Windows inner plugin | `2de0ae2ebd361c4a7b2ec29e331d15c601f2845fc79b9607d81e2267f73e8000` |
| Linux inner plugin | `7bba81019b341ea3eb863b616e3279d9b0b189780fa0422accab814f9d5e5aef` |

Both bundles report version 0.2.8. The Windows installer exited 0 in an
isolated installation; its plugin matches the signed ZIP exactly. Import
inspection found no external OpenSSL or Visual C++ runtime DLL dependency.

Windows signatures identify **Social Stream Ninja / Steve Seguin** and
DigiCert timestamps. Local Authenticode/SignTool verification reports an
**untrusted signing root**, following the repository's documented fallback
policy. This is not a claim of public certificate trust; Windows may display
a trust/reputation warning.

Linux was built on Ubuntu 22.04 and exercised in Ubuntu 24.04 under WSL2 with
an isolated X11 display. The binary requires GLIBC 2.34; all runtime dependencies
resolved. Opus, OpenSSL and libdatachannel are not external runtime dependencies.
The release includes platform and combined SHA-256 checksum files.

Completed VirusTotal analyses were verified for the exact three package hashes:
**0 malicious / 0 suspicious**. Linux: 62 undetected, 13 unsupported.
Windows ZIP: 65 undetected, 3 timed out, 7 unsupported. Installer: 71 undetected,
4 unsupported. Original completed Windows analysis records were checked
directly; supplemental rescan requests were still queued. Scan identifiers and
URLs stay in private build logs. These are point-in-time results, not a security
guarantee.

## Build and packaged-host checks

- [Windows release CI](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38658499909):
  clean build, **11 native tests**, signing and installer packaging passed.
- [Linux CI used for the archive](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38658364147)
  and [independent tag CI](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38658499913):
  **11 native tests**, **537 extensive validator checks**, **21 offline checks**,
  integration, hardening, owned live-room audio, actual media-fault recovery and
  peer churn passed. Both builds use the tagged source.
- [Mac CI](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38658364134)
  passed on both architectures, and
  [CodeQL](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38658364161)
  passed. Mac release packaging and host approval are handled separately.
- Downloaded Windows **and** Linux plugins: **537 validator checks each**, full
  integration and owned live-room receive passed. Windows hardening also passed.
  Unsupported 64-bit sample-processing cases are explicitly skipped/rejected.
- Actual Linux archive: **128 editor opens**, including overlapping instances,
  redraw checks, and both factory-context and frame-only event-loop arrangements.
  Actual Windows ZIP: five native editor reopen cycles, controls and DPI checks.
- **REAPER 7.82 on both platforms:** four simultaneous Seed/Play pairs (eight
  plugin instances), verified package module paths, sustained synthetic audio,
  custom settings, bypass recovery, editor reopening, save/reload, and a fresh
  process restoring the project all passed. These use Dummy Audio and sustained
  peak checks; they do not certify physical audio devices or every sample.
- **Audacity 3.7.9 on both platforms:** five editor open/close cycles and clean
  shutdown passed, with an owned synthetic live publisher configured. The loaded
  module was checked against the released package. This is an editor smoke test,
  not an Audacity audio measurement. Windows **Audacity 4.0.1** additionally
  validated version 0.2.8 in five fresh CLI registration processes.

## Audio results and measurement correction

Both packages passed six real encrypted-media formats: 44.1 kHz/128 frames,
48 kHz/32, 48 kHz/256, 48 kHz/1024, 96 kHz/64 and 192 kHz/256. Final captures
contained no silent, clipped or nonfinite steady-state blocks and passed
latency, level, discontinuity and codec-residual limits. Large blocks use the
existing allowance for their extra scheduling reserve. Exact metrics are in
the [machine-readable evidence](validation/2026-10-15-release-0.2.8.json).

The initial 1% native-rate median-residual gate failed intermittently: Linux
right-channel medians were **1.02779%** and **1.19337%**; a Windows 32-frame run
measured **1.18734%**. None of those captures contained silence, clipping,
nonfinite samples or abrupt tone jumps. These failures were investigated,
not discarded in favor of a passing repeat.

The expanded [codec-only reference](../../tools/tests/diagnostics/codec_quality_reference.cpp)
varies encoding start and the independent 10 ms analysis-window origin.
Linked directly to the old and new packaged Linux Opus implementations, it
produces **identical** measurements and reaches **1.20855%** median residual
without queues, networking, loss or host scheduling. One reference alignment
reproduces the failing Linux capture's **0.450763% / 1.19337% medians** and
**0.466836% / 1.55538% p95** to the printed precision.
The [240 reference measurements](validation/2026-10-15-codec-reference.txt)
are retained alongside the release evidence.

Accordingly, the test now allows **1.5% median at 48 kHz**; the resampled limit
remains 2%. The 3% p95, zero-silence, finite/unclipped output, waveform-jump,
latency and level gates remain unchanged. All **15 analyzer regressions** pass,
including rejection of clipping, isolated clicks and a 1.6% native-rate harmonic.
This is a measurement calibration, not a codec or transport change. The test
and evidence follow-up does not alter the tagged binaries.

Both platforms passed separate sample-level click/boundary analysis. Windows
also passed actual SRTP 5% loss, 15 ms jitter, three-second outage/recovery and
384 replacement requests with 384 distinct offers and three recovery offers.
Linux passed equivalent fault/churn checks in both final CI runs.

The final **300-second strict Windows soak** included signaling interruptions
at 20 and 40 seconds: **0 silent blocks / 54,520 steady blocks**, minimum RMS
**0.177708**, maximum observed callback **249.9 microseconds**.

### Preserved scheduling-limit finding

An earlier Windows 32-second run, concurrent with the eight-instance REAPER
test, recorded **one fully silent 256-frame block** at approximately 29.107
seconds. Nearby capture timestamps show **31.17 ms Seed / 29.28 ms Play host
callback gaps**, beyond the roughly 21 ms four-block reserve at this format.
This is consistent with a scheduling stall; the exact OS cause was not proven.
An isolated repeat passed the strict capture and all sample-level checks, and
the subsequent five-minute soak passed without silence. The failed capture's
summary and timing evidence are retained. These results do not promise
dropout-free operation during host/OS stalls or arbitrary system load.

## Scope and remaining limits

This release uses Opus packet-loss concealment. **Opus in-band FEC is not
enabled**, and incoming RED support extracts the primary payload; it does not
provide redundant-packet recovery. Prefill remains fixed, with bounded buffering;
there is no adaptive jitter buffer or general packet reordering implementation.

Audio processing supports 32-bit float only. Linux requires an X11/XEmbed VST3
host; native Wayland, ARM Linux, physical audio interfaces, hardware end-to-end
latency and WAN conditions were not certified here. Local tone probes exercise
real signaling and encrypted media but exclude ADC/DAC latency. Ableton's earlier
missing-resource GUI fix and Windows trial evidence remain in the
[issue/PR review](ISSUE_PR_REVIEW_2026-10-13.md); this pass did not repeat every
Ableton configuration.

The final remote fetch found no missing main-branch changes. Open issue #6
and PRs #3–#5/comments were rechecked; they contained no new actionable findings
beyond the previously validated fixes. Reports and comments were treated as
leads, with code inspection and reproduction used for decisions.

Actual packaged-application screenshots and the silent eight-second REAPER
recording are in the [media gallery](../../media/gallery/README.md).
