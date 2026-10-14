# v0.2.6 packaged-release validation

Validated on Windows x64 on 2026-10-14, after pulling the current branch and
fetching `main`; no upstream changes were missing. Tag `v0.2.6` points to
`4f0c8bafd1db3d84f39810b7428ac546f0d2ac7d`. The public release was held as a draft
while these checks ran against the downloaded, signed CI packages.

## Packages and signatures

| Artifact | SHA-256 |
| --- | --- |
| Windows VST3 ZIP | `1bfacb76d3323eba3134360cbe340c6b5feb183ed9747436e6b26adc42412092` |
| Windows installer | `7913ffdbc0789f02dbc5d0ac20c504b3e4e02bfc98767c6fa9c0a18fa76eb433` |
| Plugin binary in either package | `790534512306c01b022dcc734d673cd9ee4ce111c1078100a20125f4841d8fd5` |

Module metadata reports 0.2.6. An isolated silent installation exited 0;
the installed plugin matches the ZIP's binary exactly. Import inspection
found no external OpenSSL or Visual C++ runtime DLL dependency.

CI signed both artifacts as **Social Stream Ninja / Steve Seguin** with
DigiCert timestamps. Local Authenticode and SignTool checks identify the
signer and timestamps but report an **untrusted signing root**. This follows
the repository's explicit fallback policy; it is not a claim of public
certificate trust. Windows may show a trust/reputation warning.

Completed VirusTotal scans reported **0 malicious / 0 suspicious**: 68 engines
undetected for the ZIP (7 unsupported), 71 for the installer (4 unsupported).
These are point-in-time results, not a security guarantee. Analysis URLs are
retained only in internal release logs. `SHA256SUMS.txt` accompanies the packages.

## Passed checks

- [Windows CI](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38517837251):
  clean build; all **9 native tests**, including impairment, resampling,
  lifecycle, stress, fuzzing and editor operation with/without resources.
- [Mac CI](https://github.com/steveseguin/Ninja-VST3-Plugin/actions/runs/38517837265):
  both Apple Silicon and Intel builds; **8 native + 15 offline tests per
  architecture**, plus SDK, security, configuration, crypto, reconnect,
  resource-limit and audio transport checks.
- Downloaded Windows binary: Steinberg SDK 3.8.0 validator **47 passed,
  0 failed**. Unsupported 64-bit sample-processing cases are skipped.
- Downloaded Windows binary: five native editor reopen cycles, text fields,
  Advanced controls, Play/Publish layout and DPI scaling.
- Full JavaScript integration gate, using the release binary, passed security,
  SDK behavior, Unicode/custom-salt settings, audio-only negotiation, ignored
  video tracks, CLI receive and SDK receive from plugin Publish. Owned
  synthetic senders were used; source pacing remained approximately 1:1.
- Live room receive passed with RMS **0.171692** and audio-only preferences.
- **REAPER 7.82/x64**, Dummy Audio, 48 kHz project: verified the loaded module
  path; two 35-second audio phases, custom password/salt/domain persistence,
  bypass recovery, project reload and **21 sustained peak windows** passed.
  A fresh process restored the project and passed a further 35 seconds /
  **13 peak windows**. These are sustained-activity checks, not sample-level
  dropout measurements.
- REAPER offline rendering: 96,000 frames/channel of stereo PCM24 at 48 kHz.
  Publish matches the bypass PCM **byte for byte**; Play outputs all zeros,
  as intended for a live receiver during offline export.
- **Audacity 3.7.9**: five editor open/close cycles and clean shutdown, with an
  owned live stream configured. No Audacity playback measurement is claimed.
  **Audacity 4.0.1**: five fresh CLI registration processes all reported
  Validated, realtime capable, version 0.2.6; GUI/audio behavior is not certified.
- Final **300-second strict stereo soak**, 48 kHz / 256 frames, including
  signaling disconnections at 20 and 40 seconds while media remained connected:
  **0 silent blocks / 54,520 steady blocks**, minimum channel
  RMS **0.176629**, maximum observed callback **194.6 microseconds**.
- Separate **45-second strict playback-quality probe**: **0 silent,
  clipped or nonfinite blocks**; local one-way marker latency p95
  **66.15 ms**, spread **0.96 ms**. Tone residual medians
  L/R **0.368% / 0.994%**, p95
  **0.875% / 1.120%**. Existing limits were unchanged.

The timed probes ran after the other host/audio tests finished. Results and
hashes are in [the machine-readable record](validation/2026-10-14-release-0.2.6.json).
The [earlier review](ISSUE_PR_REVIEW_2026-10-13.md) includes issue/PR assessment,
Ableton validation, additional sample rates, packet-loss regressions, and
retained failed experiments and their diagnoses.

## Scope and remaining limits

This is a **Windows package release**. Mac source passed both CI architectures;
new Mac distribution still requires Developer ID signing, notarization and
manual DAW checks. Existing notarized [Mac v0.2.5 downloads](https://github.com/steveseguin/Ninja-VST3-Plugin/releases/tag/mac-v0.2.5)
remain available.

Audio measurements use synthetic signals and same-machine media paths; they
do not certify physical audio interfaces or impaired WAN performance. Opus
FEC remains disabled; packet reordering/adaptive jitter support is unchanged.
The plugin intentionally accepts 32-bit float audio only. Record remote audio
in real time before offline export. No test suite guarantees every DAW/device
combination; no remaining blocker was observed within the checks above.

Actual application screenshots and the short native-editor video are in the
[media gallery](../../media/gallery/README.md), including an Audacity capture
of this signed release binary.
