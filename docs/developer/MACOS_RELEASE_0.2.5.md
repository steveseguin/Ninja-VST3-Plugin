# macOS v0.2.5 artifact validation

The versioned release was rebuilt from the runtime changes documented in
[HARDENING_VALIDATION.md](HARDENING_VALIDATION.md). Only version metadata changed
after the prolonged validation; the 30-minute run is not claimed to have run on
the newly versioned or newly signed binaries.

Both architecture builds and all five native CTest suites passed again after
the version bump. The final Developer ID–signed ZIPs were independently
extracted; native integration and pluginval strictness 10 were rerun against
those extracted bundles. Intel execution uses Rosetta, not physical Intel.

Signing identity: Developer ID Application: Steve Seguin (H3CKR5XB3J), with
secure timestamp and hardened runtime. Both bundle signatures verify strictly.
The archives contain the VST3 bundle, installation instructions and dependency
licenses, with no non-system runtime library requirement. These are signed
previews, not notarized artifacts.

| Artifact | SHA-256 |
|---|---|
| `webrtc_vst-v0.2.5-macos-arm64-signed.zip` | `4a62a2ea5fc7f7d45d237690ee5e91df66c5cb05fffdbc41a5987fd958934a16` |
| `webrtc_vst-v0.2.5-macos-x86_64-signed.zip` | `f5e2e5919ae5852004938ceba4ac29d2a182e5e96f4a81ad89a5767145689be2` |

Local evidence is in `build/test-tools/release-025-*.log`. Publication uses the
Mac-only `mac-v0.2.5` tag, so it does not trigger the Windows release workflow.
The preview workflow verifies exact archive hashes, signing identity, version
and architecture, and submits both ZIPs to VirusTotal before publication.
It leaves the draft private unless its explicit `publish` input is true.
VirusTotal analysis URLs belong in workflow logs, not public release notes.
