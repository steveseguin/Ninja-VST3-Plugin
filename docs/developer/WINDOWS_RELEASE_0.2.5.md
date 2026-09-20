# Windows 0.2.5 validation

Validated on 2026-09-19, Windows x64 build 26200, Visual Studio 2022,
VST3 SDK 3.8.0 build 66, REAPER 7.62 and Chrome 152.0.7977.83.
Local logs are retained under `build/windows-0.2.5-validation/`.

## Review and fixes

The existing `mac-v0.2.5` release uses source
`3c008d50a64cbf19d8c1b942316c352103f8c6d6`. The Windows review started at
`4c237e2208d2dc78fe230ce04f3733005898db62`; intervening commits did not alter
the plugin runtime. Existing Mac assets and their tag are preserved.

- Windows ANSI environment strings broke the new UTF-8 validation for Unicode
  salts/passwords. Text configuration now reads the Windows UTF-16 environment
  and converts it to UTF-8 using the existing SDK conversion. Missing and empty
  values remain distinct; other platforms keep their existing `getenv` behavior.
  Regression coverage independently verifies Unicode room/password/salt hashes
  and CLI saved-state injection.
- The previous local build selected OpenSSL import libraries and required DLLs
  that were absent from the bundle. This release uses static OpenSSL 3.6.1 and
  static MSVC runtime libraries. The final plugin imports only Windows system
  DLLs. Release CI now explicitly selects static dependencies; shared CMake
  defaults are unchanged.
- Release CI previously built the installer before signing the plugin. It now
  signs the plugin first, builds the installer containing that signed binary,
  then signs the installer. Signer, timestamp and hash checks are required.
- Windows `sleep_until` caused the optional CLI real-time test clock to burst
  callbacks around 15.6 ms boundaries. A private high-resolution waitable timer
  fixes that test mode. It changes neither the plugin nor the system timer
  period. Non-Windows pacing and ordinary CLI runs are unchanged.

## Results

| Check | Result |
| --- | --- |
| Fresh Release configuration/build, static dependencies | Passed |
| Native settings, integration, stress and fuzz suites | 4/4 passed |
| Full JS integration gate | Passed, including seven advanced-setting cases |
| Live room with owned synthetic publisher | Passed, decoded RMS 0.171738 |
| Crypto compatibility, signaling recovery and resource-limit tests | Passed |
| Self-signed TLS server rejection | Passed |
| Real Chrome / VDO.Ninja, both audio directions | Passed for custom, escaped, automatic and Unicode salts |
| Wrong-salt negative control | No audio packets, as expected |
| REAPER native editor and DAW audio | Seed/play, bypass, five editor cycles per instance, saved-project reopen passed |
| Strict synthetic playback quality | Zero silent/non-finite/clipped blocks out of 6,750 |
| Five-minute stereo soak with two signaling interruptions | Zero silent blocks out of 54,520; worst measured callback 501.8 microseconds |
| Installer and ZIP extraction | Both contain the identical signed plugin |

Audacity scanning found the release plugin in a portable application copy,
but editor/playback validation was not completed. Steve directed this release
to use REAPER validation instead of further Audacity testing.

The initial strict quality run failed because of the CLI clock described above.
With the corrected host clock, the unchanged signed plugin passed: local median
one-way latency 66.77 ms, p95 66.96 ms, marker spread 0.87 ms. Median tone
THD-plus-residual was 0.47% left / 0.70% right (p95 0.49% / 0.75%). These are
same-machine synthetic measurements at 48 kHz with 256-frame host blocks;
they exclude audio-interface and WAN latency.

## Artifact verification and limits

Signed plugin SHA-256:
`55e00cb554776e3616cad4e84713b33a266948c48f5adb622eabde3cfa7c67f7`.

ZIP SHA-256:
`4970079ee0fd1e755c06136367f01e4b461e5259544f38f01fdc05108e9a94c6`.

Both plugin and installer have the Social Stream Ninja / Steve Seguin signer
and a DigiCert timestamp. The local trust store does not trust the signing
root: Authenticode reports `UnknownError`, and `signtool verify /pa /v` reports
an untrusted root. Signature presence, signer identity and timestamp were
verified under the repository's existing fallback policy. This does not imply
public certificate trust or SmartScreen reputation.

VirusTotal submission records and analysis URLs are retained only in local
release logs. Local Microsoft Defender scanning of both packages found no
threats. VirusTotal's Microsoft engine reported `Trojan:Win32/Wacatac.C!ml` on
the installer (69 other engines reported no detection); this disagreement is
not proof of a false positive.
The ZIP completed with zero malicious or suspicious detections (66 engines
reported no detection, two failed and six did not support the file type).
Only the ZIP and its checksum are published; the installer is held back.

Requires an x64 VST3 host using 32-bit float processing. Studio One, other
Windows versions, restrictive NAT/WAN paths, independent device clocks and a
full-stack sanitizer run were not validated here. No TURN relay is configured.
The updated release workflow was syntax checked and its packaging sequence
exercised locally; a GitHub release workflow run was not used for these assets.
