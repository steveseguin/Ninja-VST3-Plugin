# WebRTC VST for VDO.Ninja

Send audio from your DAW to [VDO.Ninja](https://vdo.ninja), or receive remote
streams and room audio in your DAW. For talkback, remote direction and mix review.

**[Download for Windows, macOS or Linux](https://github.com/steveseguin/Ninja-VST3-Plugin/releases)**
· [Installation guide](https://steveseguin.github.io/Ninja-VST3-Plugin/getting-started.html)

## Screenshots

| Receive with Play | Send with Publish |
| --- | --- |
| ![Play receiving audio in REAPER](media/gallery/reaper-play.png) | ![Publish sending audio in REAPER](media/gallery/reaper-publish.png) |

Actual plugin windows in REAPER. [More screenshots and short videos](media/gallery/README.md).

## Get started

1. Install the complete `.vst3` bundle and rescan plugins in your DAW.
2. Add **VDO.Ninja WebRTC Bridge** and choose **Publish** to send or **Play** to receive.
3. Enter the stream ID or room name and matching password for your session.

Supports Opus stereo, sample-rate conversion and room mixing. Requires a VST3
host with 32-bit float processing; Linux uses X11.

## Documentation

- [Advanced settings and self-hosting](docs/developer/ADVANCED_SETTINGS.md)
- [Build and test](docs/developer/BUILD_AND_TEST.md) · [macOS](docs/developer/MACOS.md) · [Linux](docs/developer/LINUX.md)
- [Release validation and known limits](docs/developer/RELEASE_VALIDATION_0.2.8.md)

[License](LICENSE) · [Contributing / CLA](CONTRIBUTING.md)
