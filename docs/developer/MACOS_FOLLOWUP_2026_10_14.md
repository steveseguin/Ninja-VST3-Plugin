# macOS follow-up: short fault and concurrency tests

This follows [the initial macOS validation](MACOS_VALIDATION_2026_10_14.md).
Short checks ran first; the 30-minute audio soak completed last.
Local logs are in `build/test-tools/quick-validation/`.
The [machine-readable results](validation/2026-10-14-macos-followup.json) include
binary hashes, fault counters and memory measurements.

## Findings and fixes

ThreadSanitizer found two distinct races during concurrent instance stress:

- The test host called SDK `PluginFactory::classInfos()` concurrently. SDK 3.8
  uses a global mutable UTF converter here. Enumeration is now serialized in
  the test host; plugin creation, initialization and destruction still run
  concurrently. No plugin operations were removed from the stress test.
- The plugin's SDK `CPluginFactory::release()` atomically decrements the reference
  count, then returns a plain read of the shared counter. Another thread can
  modify it or release the last reference before that read. The plugin now uses
  a factory subclass that returns the atomic decrement's result without another
  object access. Factory metadata, IDs, interfaces and audio behavior are unchanged.

The original diagnostics are retained in `extended-1791994023529` and
`extended-1791994159783`. After both fixes, a short instrumented lifecycle cycle
passed integration, settings, fuzz and concurrent stress without suppressions.

## Test additions

- `npm run test:media-faults`: two owned plugin instances advertise local UDP
  relays as ICE candidates. Their actual SRTP media encounters approximately 5%
  packet loss, 0–15 ms jitter and a three-second complete media outage, followed
  by recovery. STUN, DTLS and RTCP are forwarded. System network settings are
  untouched. Counters require media in every phase; a measured outage level
  reduction guards against bypassing the relay, and recovered stereo must have
  the expected level/pitch with no silent blocks or non-finite output.
- `WEBRTC_REAPER_TEST_PAIRS=4`: eight plugin instances in four independent live
  Seed/Play pairs. Every pair must produce audio in each measured window, with
  bypass/re-enable, repeated editor opening/closing and saved-project reload.
  Fresh-process reopen uses the same pair count. The project master stays muted.

The initial relay attempt exposed a test-fixture issue: libjuice can reply from
loopback even when its advertised host address is a LAN address. The relay now
handles the local address translation consistently. A subsequent control expected
silence during loss, but Opus PLC retained comfort noise at about 0.064 RMS versus
0.177 clean RMS. The control now requires attenuation relative to its measured
baseline, not digital silence. Neither adjustment changes plugin audio behavior.

## Completed short checks

- Release: all nine native suites, 537 Steinberg extensive-validator tests and
  two randomized strictness-10 pluginval runs passed.
- ThreadSanitizer: 126 PLC partition cases, 21 packet-impairment scenarios and
  concurrent receive/pull passed. The final lifecycle cycle passed all four
  suites in 42.7 seconds, including ten concurrent instances and concurrent
  create/destroy. No suppressions were used.
- Real relayed SRTP: Release dropped 14 packets during the loss phase, applied
  0–15 ms jitter to 301 packets, then dropped all 149 packets in the three-second
  outage. The instrumented repeat counted 15, 301 and 149 respectively.
  Both runs recovered the expected stereo tones without silent recovery blocks
  or non-finite samples. This does not imply loss was inaudible.
- REAPER: all four pairs passed 21 observation windows each during the first
  run and 13 each after a fresh-process reopen. All eight editors were cycled
  five times during the initial run. Peak owned RSS was 333 MiB in the reopen.

The final installed Release binary is
`52f6fb12ce43b7b58cc1828cf6b273e9834e10c16aa82a43ee4a0bef2c112a92`.
The instrumented plugin used for the final media/lifecycle checks is
`19dccff1f23e3027a7714328f9f2061e40dce36e773e3c8bcf391f3744b206e4`.

## Final 30-minute soak

The final Release build passed 1,800 seconds of paced stereo Seed/Play audio at
48 kHz with 256-frame blocks. Two owned signaling connections were interrupted
at 20 and 40 seconds while media remained connected. Both processes exited
normally. After startup, 1,786 measurement windows covered 335,768 audio blocks
with zero silent blocks and zero non-finite samples. Minimum channel RMS was
0.177708 and maximum frequency error was 0.665 Hz for the 1,000/1,500 Hz tones.

The worst measured audio callback was 636.041 microseconds against a
5,333-microsecond block deadline. This measures callback execution, not
end-to-end network latency or scheduler wake-up jitter.

Five-second RSS samples after the first 30 seconds peaked at 19,760 KiB for the
Seed host and 19,456 KiB for Play. Their final readings were 17,216 and 15,584 KiB,
respectively, with no sustained growth. These figures include each CLI host and
plugin; they are not isolated plugin heap measurements or proof of no leaks.
The complete guarded process tree peaked at 175 MiB, with at least 54% system
memory headroom throughout. Detailed measurement windows and logs remain in
`build/test-tools/audio-soak-1791994723750/`; memory samples are in
`build/test-tools/quick-validation/soak-memory.json`.

## Reproduction

```bash
node tools/tests/guarded_run.js npm run test:media-faults

# After building a separate fully instrumented configuration:
TSAN_OPTIONS=halt_on_error=1 WEBRTC_TEST_BUILD_DIR=build/webrtc_vst_mac_tsan \
  node tools/tests/guarded_run.js npm run test:media-faults
TSAN_OPTIONS=halt_on_error=1 WEBRTC_TEST_BUILD_DIR=build/webrtc_vst_mac_tsan \
  WEBRTC_EXTENDED_SECONDS=10 node tools/tests/guarded_run.js npm run test:extended

# Run last, after short checks:
WEBRTC_GUARD_SECONDS=1880 WEBRTC_SOAK_SECONDS=1800 \
  WEBRTC_SOAK_RECONNECT=1 WEBRTC_SOAK_STRICT=1 \
  node tools/tests/guarded_run.js npm run test:audio-soak
```

ThreadSanitizer uses `-fsanitize=thread -fno-omit-frame-pointer -O1 -g` on the
plugin, host, tests and built dependencies. Prebuilt OpenSSL is not instrumented.
The lifecycle runner finishes an entire four-suite cycle, so a requested ten
seconds takes roughly forty seconds under instrumentation. It is not a long soak.

## Scope

These are synthetic tests on the same M1 Mac. They do not exercise independent
physical audio clocks, WAN/NAT traversal, a complete ICE/network outage, arbitrary
bandwidth saturation, sixteen audible peers or another physical Windows DAW.
The relay test measures recovery and bounded levels; it does not promise
inaudible packet loss. REAPER peak windows prove sustained signal presence,
not sample-by-sample perceptual quality. No sanitizer pass proves race freedom.
