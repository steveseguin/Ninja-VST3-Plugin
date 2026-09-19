# macOS v0.2.5 artifact validation

The versioned release was rebuilt from the runtime changes documented in
[HARDENING_VALIDATION.md](HARDENING_VALIDATION.md). Only version metadata changed
after the prolonged validation; the 30-minute run is not claimed to have run on
the newly versioned or newly signed binaries.

Both architecture builds and all five native CTest suites passed again after
the version bump. The initially staged signed ZIPs passed extracted-artifact
tests, but were replaced before publication by notarized DMGs at the user's
request. The final DMGs were mounted read-only, their plugins copied out, and
quarantine flags set on the owned test copies. Native integration (14 cases)
and pluginval strictness 10 passed for both copies. Local Intel execution uses
Rosetta. Clean macOS 15 arm64 and Intel GitHub jobs and CodeQL also passed.

Signing identity: Developer ID Application: Steve Seguin (H3CKR5XB3J), with
secure timestamp and hardened runtime. Both bundle signatures verify strictly.
The archives contain the VST3 bundle, installation instructions and dependency
licenses, with no non-system runtime library requirement.

Apple accepted both final disk images with `issues: null` in the notarization
logs. Each DMG has a validated stapled ticket and passes `spctl --assess --type
open --context context:primary-signature` with `source=Notarized Developer ID`.
This does not replace testing on a separate clean end-user Mac or older macOS.

| Artifact | SHA-256 |
|---|---|
| `webrtc_vst-v0.2.5-macos-arm64-notarized.dmg` | `ba553dae7b450f05dc906ea3900267fb6ee928fa1829734b3a7af3f48e4a1392` |
| `webrtc_vst-v0.2.5-macos-x86_64-notarized.dmg` | `d28ffe1f801fc0b50f5c98454a944fa3d88d61510f2490b2884a2af034200d25` |

Local evidence is in `build/test-tools/release-025-*.log`. Publication uses the
Mac-only `mac-v0.2.5` tag, so it does not trigger the Windows release workflow.
The Mac workflow verifies the exact DMG hashes, signing identity, Apple tickets,
Gatekeeper assessment, version and architecture, and submits both final DMGs
to VirusTotal before publication. It rejects draft assets left over from the
non-notarized preview; only the two DMGs and checksum manifest are permitted.
It leaves the draft private unless its explicit `publish` input is true.
VirusTotal analysis URLs belong in workflow logs, not public release notes.

Future releases use `bash mac.sh` (or `--package-only` for tested builds), with
credentials held in the local `vdoninja-notary` Keychain profile. Submission IDs,
status and scan logs are retained under `build/release/notary-*`; no passwords
are stored in this repository. The runtime source tag remains `mac-v0.2.5`;
the later packaging/website updates do not modify plugin code.
