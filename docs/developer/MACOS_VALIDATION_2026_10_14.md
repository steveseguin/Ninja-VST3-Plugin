# macOS audio and stability validation — 2026-10-14

Tested on an Apple M1 MacBook Air, macOS 26.4.1, 8 GiB RAM. Changes build on
`a077a60` / version 0.2.6, including its existing resampler and packet-loss fixes.
This is development-build validation, not a new public release.
[Machine-readable measurements](validation/2026-10-14-macos-audio.json) include
the retained failure metrics and the final artifact identities.

## Fixes and performance

- Extended ASan/UBSan validation reproduced a real shutdown race after 76 seconds
  (fuzz seed 5653587): `onConnected` passed its shutdown check, then shutdown
  moved the signaling client away before the callback dereferenced it. Every
  signaling send now holds the session ownership lock through use, including
  announcements, room joins and Play refreshes. The early shutdown check alone
  was insufficient. The fuzz reconnect test now sweeps 0/1/3/10 ms connection
  delays across 80 activation cycles, against the extended runner's owned
  loopback server. This adds no locks to the host audio callback.
- The worker-to-host queue's fixed 1024-frame minimum provided only 10.7 ms of
  scheduling headroom at 96 kHz and 5.3 ms at 192 kHz. A continuously running
  96 kHz / 64-frame probe exposed four silent blocks. The minimum now scales
  with sample rate to preserve the original 48 kHz headroom of about 21 ms,
  within the existing fixed queue capacity. A deterministic regression pauses
  the producer for 16 ms at 48/96/192/384 kHz and checks every returned sample.
  A subsequent 90-second 96 kHz probe passed 121500 steady-state blocks without
  detected clicks, silence, clipping or non-finite output, at 68.9 ms p95 latency.
- Large callbacks could overflow the receive ring during packet bursts. At
  48 kHz / 1024 frames, a steady tone had a 0.452 adjacent-sample jump despite
  zero silent blocks and acceptable percentile distortion. Receive capacity now
  accommodates four maximum host blocks, bounded at 32768 frames, while keeping
  the existing sample-rate scaling and pre-fill threshold. The repeat measured
  maximum jumps of 0.033 / 0.050 on the two channels. At 48 kHz / 1024 frames,
  this costs an additional 16 KiB per stereo peer; capacities at 48 kHz with
  maximum blocks of 512 frames or less are unchanged.
- Hosts can supply intermediate normalized values to the two-choice mode
  parameter. Presets previously retained only Play/Seed, so JUCE/pluginval's
  cached automation value changed after restoration. State now also retains the
  exact normalized value, with bounded, consistent validation. The existing
  0.5 mode threshold and network behavior are unchanged. Legacy presets still
  work. The audio-thread mailbox remains lock-free.
- An initial four-second macOS `sample` profile showed ring-buffer reads on
  the audio worker (23 sampled stacks in `AudioRingBuffer::pop`); the host main
  thread spent most sampled time pacing. Ring-buffer planar copies and reads
  now use at most two contiguous spans, eliminating per-sample integer remainder operations while holding the lock.
  A matched benchmark against `a077a60`, using the same C++20 `-O3` harness,
  measured **7.8–9.4× faster planar push/pop** and **7.0–8.7× faster interleaved
  push plus planar pop**, depending on block size. These are buffer-operation
  speedups, not whole-plugin CPU improvements. Median of three runs, 100000
  pairs per run, stereo, 2048-frame capacity, 64/256/512/960-frame blocks.
- The CLI now supports explicit rate/block-size probes and fails when plugin
  processing fails. The audio analyzer detects isolated waveform jumps as well
  as percentile distortion. Offline regression fixtures include a single-sample
  click that percentile checks alone miss.

## Completed host and native checks

- Release build: all nine CTest suites passed, including integration, concurrent
  lifecycle stress, seeded host fuzz, Cocoa editor, packet impairment, settings,
  resampler and buffering tests. The new FIFO test compares 50000 randomized
  operations against an independent deque model.
- Pluginval 1.0.4: strictness 10, two randomized repeats, seed 20261014;
  32/44.1/48/96/192 kHz and 16/64/256/1024/2048-frame blocks. Passed after the
  mode-state fix. No tests were disabled to obtain the pass.
- REAPER 7.80 arm64: installed bundle identity checked; live Seed → Play audio,
  custom salt/domain restoration, bypass/re-enable, repeated editor open/close,
  save/reopen, and another reopen in a fresh host process. All passed, with 21
  sustained observation windows in the initial run and 13 in the restart run.
  The test project master was muted. No Studio One or Audacity was installed.
- Chrome 155.0.8059.41 with the real backup VDO.Ninja website: plugin → browser
  and browser → plugin custom-salt audio passed. The mismatched-salt control
  received no audio. A separate instrumented live plugin-to-plugin probe passed.
- Main JS integration, live-room and hardening gates passed: audio-only
  negotiation, real decoded media, crypto compatibility, repeated reconnects,
  malformed/deep/oversized signaling, pre-SDP ICE floods and the 16/17-peer
  boundary. All 18 offline analyzer/publisher tests passed, including isolated
  clicks and resampled-format quality limits.

The full plugin, CLI, native tests and built dependencies were compiled with
AddressSanitizer and UndefinedBehaviorSanitizer (`-O1 -g`, frame pointers).
Prebuilt OpenSSL was not instrumented. All nine native suites passed with
`ASAN_OPTIONS=quarantine_size_mb=4:thread_local_quarantine_size_kb=64`, recording
1933312-byte RSS growth across 30000 string edits. The release build's growth
was 458752 bytes. An earlier run with default sanitizer settings passed eight
suites, but the integration suite's *RSS assertion* failed because ASan retained
freed allocations (324026368-byte growth), without a sanitizer error. Bounding
the quarantine permits the same RSS assertion to run under instrumentation.
No sanitizer findings were suppressed. macOS leak detection was not certified.

After fixing the shutdown race, the full instrumented plugin passed 608 seconds
of repeated integration, settings, fuzz and concurrent lifecycle stress:
124 suites, 31 seeds (5653587–5653617), including 2480 rapid reconnect cycles.
The runner checked the plugin hash throughout. Peak aggregate owned RSS was
234 MiB; minimum system memory headroom was 57%. The original failing run is
retained as `extended-1791988525491`; the passing repeat is
`extended-1791989125974`. This is ASan/UBSan coverage, not full-stack TSan.

On this interactive desktop, the Cocoa smoke process needed to be brought to
the foreground with System Events to receive real keyboard focus. The unchanged
editor test then passed actual text editing, Copy, independently decoded QR and
repeated editor toggles. CLI invocation without foreground access is not an
equivalent UI test.

## Interpreting tone residual measurements

An additional 44.1 kHz rerun initially failed the inherited 1% median residual
limit: its right channel measured 1.40%, with no detected clicks or dropouts.
The standalone [codec reference](../../tools/tests/diagnostics/codec_quality_reference.cpp)
then reproduced up to 1.67% using the unchanged production resampler and default
Opus AUDIO settings, without the network, queues or recent production changes.
It measures 12 starting phases at each rate, after eight seconds of settling.
Maximum medians across those phases/channels were:

| Host rate | Reference median residual, maximum |
| --- | ---: |
| 32 kHz | 1.21% |
| 44.1 kHz | 1.67% |
| 48 kHz | 0.86% |
| 88.2 kHz | 1.38% |
| 96 kHz | 1.19% |
| 192 kHz | 1.45% |

The probe consequently retains its 1% median gate at native 48 kHz and uses 2%
when resampling. The 3% p95, waveform-jump, silence and clipping gates remain
unchanged. New offline fixtures verify both residual limits and reject a click
at 44.1 kHz. This changes the measurement's format assumptions, not the codec or
audio processing. The failed original-limit capture is retained locally as
`playback-probe-1791985877224`, with its original metrics in the evidence JSON.

The live probe also now keeps the test host processing during its connection
warmup, using `WEBRTC_CLI_HOST_CONTINUOUS_WARMUP`. Previously it activated the
plugin, primed one block, then stopped callbacks for two seconds, saturating
receive queues before measurement. That stalled-host configuration produced a
64-frame discontinuity at 96 kHz (`playback-probe-1791986488182`). Continuous
warmup then exposed the separate high-rate worker-headroom problem described
above. CLI defaults retain the original warmup mode for deliberate stall tests;
the continuous mode models a normally running DAW audio engine. A separate
90-second repeat of the deliberately stalled 96 kHz / 64-frame startup passed
after the fixes: 121500 measured blocks, zero silent/clipped/non-finite blocks
and no detected plateau discontinuities (`playback-probe-1791990801396`).
Arbitrary host stalls and saturated-queue recovery are not certified as click-free.

## Sustained audio and memory

All six final live-format probes passed with zero detected plateau clicks,
silent blocks, clipped blocks or non-finite samples. Each used 30 seconds of
captured playback, with eight seconds excluded for settling. Latency is
same-machine one-way media delay, excluding physical audio devices and WAN.

| Host format | Median / p95 latency (ms) | Seed / Play host CPU (% of one core) | Maximum host RSS (MiB) |
| --- | ---: | ---: | ---: |
| 44.1 kHz / 128 frames | 69.8 / 72.7 | 3.6 / 2.9 | 22.7 |
| 48 kHz / 32 frames | 68.4 / 68.4 | 3.6 / 3.0 | 22.6 |
| 48 kHz / 256 frames | 66.4 / 67.3 | 3.5 / 2.8 | 22.6 |
| 48 kHz / 1024 frames | 135.0 / 156.3 | 3.3 / 2.6 | 22.8 |
| 96 kHz / 64 frames | 68.3 / 68.3 | 3.8 / 3.3 | 22.6 |
| 192 kHz / 256 frames | 67.5 / 67.7 | 4.4 / 3.9 | 22.7 |

CPU/RSS cover complete CLI processes, including pacing and capture overhead;
they are not plugin-only costs. Larger blocks receive additional latency
allowance for the four-block scheduling reserve and block timestamp resolution.
The measurements do not imply zero latency.

A five-minute stereo soak included two deliberate signaling disconnects while
media remained connected. All 290 steady-state one-second windows passed:

- 54520 steady-state blocks; **zero silent blocks** and no non-finite samples.
- Minimum RMS 0.177712; maximum measured tone-frequency error 0.665 Hz.
- Worst measured plugin callback 130.75 µs versus a 5333 µs block deadline.
- From 30 seconds onward, sender RSS went from 19776 to 19824 KiB; receiver RSS
  went from 19536 to 19552 KiB. RSS stayed essentially flat over five minutes;
  this does not prove leak freedom.

## Resource handling and installation

Approximately 18 GiB was reclaimed by removing iOS 18.4/18.5, visionOS 2.5 and
tvOS 18.5 runtimes, Xcode DerivedData and the Playwright browser cache. iOS 26.5,
simulator user data, Xcode Archives and DeviceSupport were retained. Docker was
not installed. Approximately 17 GiB remained free after the new builds/tests.

All validation workloads ran sequentially under `guarded_run.js`. The initial
two-job sanitizer build was stopped at 1126 MiB owned RSS; the one-job resume
completed at 823 MiB. Browser testing used the documented 1536 MiB allowance
and the final run peaked at 1308 MiB with 58% system memory headroom. Other
workloads retained the default 1024 MiB limit. Stopped attempts are not counted as passes.

The installed, locally ad-hoc-signed plugin matches the tested Release binary:
`1a1de0629fc1980fbef0a217d993358bd8d7ab899c23cb2341e43f4c89b884ac`.
The original pre-test installation remains under
`~/Library/Application Support/VDO.Ninja/VST3 Backups/webrtc_vst.4dIgIm/`.
Detailed local logs/captures are under `build/test-tools/oct-validation/` and
the associated `playback-probe-*` / `audio-soak-*` directories.

## Limits

These checks use synthetic audio. They do not certify subjective listening
quality, physical ADC/DAC delay, remote WAN conditions, independent device-clock
drift, hours-long sessions, sixteen simultaneously audible peers, every
sender/receiver format combination, or every DAW. Larger blocks incur more
buffering delay. No finite test can establish that a plugin cannot crash. Intel hardware and Windows were not exercised on
this Mac. Public release signing/notarization, quarantine installation and
VirusTotal distribution gates remain separate; public downloads were unchanged.
