# WebRTC VST Docs

This folder is intended for GitHub Pages (`/docs` source) and contains:

- Marketing landing page for the audio-only VDO.Ninja VST plugin.
- Direct links to the latest Windows installer, signed Mac previews for Apple Silicon and Intel, and all GitHub releases.

## Files

- `index.html`: Marketing + release download page.
- `getting-started.html`: REAPER-first install/use walkthrough.
- `styles.css`: Site styling and layout.
- `developer/BUILD_AND_TEST.md`: Consolidated engineering guide for local build and test.

## Branding and SEO baseline

- Keep visible VDO.Ninja attribution on docs pages (`Powered by VDO.Ninja`).
- Keep favicon set to `https://vdo.ninja/media/favicon.png`.
- Keep Open Graph and Twitter metadata aligned with VDO.Ninja branding.
- Keep primary installer link fixed to `releases/latest/download/webrtc_vst-windows-setup.exe`.
- Keep Mac links versioned to the uploaded signed ZIPs; update both pages when replacing them.
- State whether Mac builds are notarized, and keep architecture/install guidance accurate.

## Publish

If GitHub Pages is enabled for this repository, set source to:

- Branch: `main`
- Folder: `/docs`
