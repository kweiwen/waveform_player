# Waveform Player

A compact media player for exploring waveforms, repeating passages, and adjusting playback speed. Use it as a VST3 or Audio Unit plug-in, or as a standalone macOS app.

![Waveform Player standalone app with an audio file loaded](Resource/Images/waveform-player.png)

## Features

- **WAV, MP3, and FLAC playback** — Open mono or stereo audio files and see the filename, sample rate, playback position, and duration.
- **MP4 and MOV playback on macOS** — Open a movie with a supported video track and mono or stereo audio track. Its audio goes through the existing plug-in output and controls; the picture appears in a separate, resizable Video window. Close that window and use the **Video** button to show it again.
- **Waveform navigation** — Left-click or drag on the waveform to seek while keeping the current playing or paused state. Stereo files display the left-channel waveform.
- **Playback controls** — Play from the current position, pause without losing your place, or stop and return to the beginning of the file or selected A/B range. Rewind and fast-forward move by five seconds.
- **Whole-file and A/B looping** — Repeat an entire file or a selected passage. With Loop off, playback stops at the end and returns to the beginning of the active range.
- **Four playback speeds** — Cycle through **1×, 2×, 4×, and 8×**. Changing speed keeps your position and playback state; pitch changes along with speed.
- **Volume and mute** — Adjust output from **−60 dB to +6 dB**, or double-click the slider to reset to **0 dB**. Mute and unmute without changing the volume setting.
- **Independent playback** — The plug-in's playback controls operate independently of the DAW's transport.
- **Compact dark interface** — Waveform, file information, volume, and icon controls in one view, with hover tooltips for controls and their current states.

## A/B Range Playback

1. **Right-click** the waveform to place the first marker.
2. **Right-click a different position** to place the second marker. The earlier point becomes **A**, the later point becomes **B**, and the range is highlighted. The playhead returns to A while keeping the current playing or paused state.
3. Turn on **Loop** to repeat the selected passage, or leave it off to play through the range once.

While an A/B range is active, waveform seeking and five-second skips stay within the range, and **Stop** returns to A.

**Right-click again** to begin a new pair of markers. **Shift + right-click** anywhere in the waveform clears both markers without changing your position or playback state. Loading a new media file clears the markers.

## Video MVP limits

MP4/MOV files need an audio track and codecs that macOS can decode. Video playback follows the audio source position, including seeking and A/B looping; the Video window's own audio is muted. There is no user delay setting or measured output-to-display latency calibration yet, so frame-accurate audio/video sync is not guaranteed. Video memory use includes the entire decoded audio track, as with existing audio files. MKV is not supported.

## Available Formats

| Format | File | Use |
| --- | --- | --- |
| **VST3** | `WaveformPlayer.vst3` | Instrument plug-in for compatible DAWs and plug-in hosts. |
| **Audio Unit (AU)** | `WaveformPlayer.component` | Audio Unit generator plug-in for compatible macOS hosts. |
| **Standalone App** | `WaveformPlayer.app` | A macOS app for playback without a DAW. |

## Sharing the macOS App

Build and package the standalone app on macOS:

```sh
Resource/Scripts/build-macos.sh Release
Resource/Scripts/package-macos.sh
```

Share `Builds/MacOSX/distribution/WaveformPlayer-macOS.zip`. The packaging script preserves executable permissions and checks the app's code signature before packaging and after extracting the ZIP, so recipients using macOS's built-in ZIP extraction should not need `chmod`. The archive contains the complete app; `libWaveformPlayer.a` and `juce_vst3_helper` are not needed. Packaging an existing build does not rebuild it or add support for another processor architecture.

Recipients can extract the ZIP and run the app from their own folder, or move it to `~/Applications`. This avoids needing permission to install in the system `/Applications` directory. Gatekeeper approval is a separate step: the current project uses an ad-hoc signature, so downloaded copies may still be blocked on first launch. For a trusted test copy, follow Apple's [Open Anyway instructions](https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac) in System Settings > Privacy & Security; macOS may request authentication. The packaging script does not remove quarantine attributes or change security settings.

For distribution that passes the default Gatekeeper checks, use a **Developer ID Application** certificate, enable **Hardened Runtime**, sign with a secure timestamp, submit the app for **Apple notarization**, and staple the accepted ticket to the app before packaging the final ZIP. A Developer ID certificate requires Apple Developer Program membership. See Apple's [Developer ID guide](https://developer.apple.com/developer-id/) and [notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution). A normal first-launch confirmation that the app was downloaded from the internet may still appear.

The default Release build targets the current Mac's architecture. To build for both Apple Silicon and Intel, use:

```sh
Resource/Scripts/build-macos.sh Release ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO
Resource/Scripts/package-macos.sh
```

The build script regenerates the Xcode project from `WaveformPlayer.jucer`, so persistent signing and architecture settings belong in the Projucer project rather than only in the generated Xcode project.
