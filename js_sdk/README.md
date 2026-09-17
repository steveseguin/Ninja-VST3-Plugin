# js_sdk (minimal vendor files)

This folder intentionally contains only the files required by local integration tests:

- `vdoninja-sdk.js`
- `vdoninja-sdk-node.js`
- `webrtc-adapter.js`

Demo pages, docs, and upstream test helpers were removed from this repository to keep it focused on the VST plugin and release tooling.

The vendored SDK includes local request-dispatch security fixes; it is not an
unmodified official build. Upstream v1.4.0 license notices are included alongside
the files. See the repository's AGPL license and the scope of the SDK exception.
