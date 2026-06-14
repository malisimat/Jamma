# Jamma

[![Build Status](https://dev.azure.com/mattojones/mattojones/_apis/build/status/Jamma-x64-Release?branchName=master)](https://dev.azure.com/mattojones/mattojones/_build/latest?definitionId=1&branchName=master)

Multichannel loopsampling software for recording and live performance.

![JammaV_Screenshot](https://github-production-user-asset-6210df.s3.amazonaws.com/172774419/606049963-08fd0f48-8f11-495a-83e8-f19a0cad3f1b.png?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAVCODYLSA53PQK4ZA%2F20260610%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Date=20260610T205856Z&X-Amz-Expires=300&X-Amz-Signature=f4e94adf8d931e0c9b1eb3a66073533e202d646cfd62b83fe36cfd1ae235cdf4&X-Amz-SignedHeaders=host&response-content-type=image%2Fpng)

Current Version: v5.0.2

## Features

* Multichannel recording and playback
* No need for click tracks
* Designed to work with just footpedals, if needed
* Overdubbing
* MIDI loops with audio slicing
* MIDI-triggered record and ditch control via rig-file mapping
* Immersive and touch-centric user interface
* VST effects and audio manipulation

## Requirements

* Windows (support for other operating systems planned after beta)
* A soundcard, ideally with more than 2 channels in/out

## Documentation and Guides

Detailed guides on setup, building, testing, and advanced configurations live in the [doc](doc) folder:

- **[Build and Test Guide](doc/build.md)**: Prerequisites, vcpkg dependencies, target builds, and troubleshooting steps.
- **[Real-Time Audio Guidance](doc/realtime-audio.md)**: Guidelines for working within real-time constraints and the hot-path review checklist.
- **[Overlay Control UI](doc/overlay-controls.md)**: Instructions for the Ctrl-held overlay controls, target selection rules, and quantisation handle behavior.
- **[Multi-Device MIDI Trigger Rig Config](doc/midi-trigger-mapping.md)**: Rig file mapping for multiple MIDI devices, channels, and activation mappings.
- **[Ninjam Integration Guide](doc/ninjam.md)**: Vendored files structure, reference dependencies, and update steps.
- **[VS Code Tasks Setup](doc/vscode-tasks.example.json)**: Local configurations for automating MSBuild.

