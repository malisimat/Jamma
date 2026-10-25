# Jamma

[![Build Status](https://dev.azure.com/mattojones/mattojones/_apis/build/status/Jamma-x64-Release?branchName=master)](https://dev.azure.com/mattojones/mattojones/_build/latest?definitionId=1&branchName=master)

Jamma is free Windows multichannel loop-sampling software for recording and live performance. Build a session from audio and MIDI loops, layer new takes, and shape them as you play. Its spatial interface puts each instrument's loops around a station, with controls for hands-on use or footpedal operation.

![Jamma playing a session with multiple loop stations](https://github.com/user-attachments/assets/80334e4d-ac59-4e57-9f1d-92420feff840)

Explore individual loops and bring their controls into view while the session plays.

![Loop controls opening around a playing station](https://github.com/user-attachments/assets/01f61a85-62b1-4258-bdf8-f7a06a2b8d34)

Open a loop to work on the details: edit MIDI notes or paint directly into an audio loop's spectrum. The clip below switches between the two editors.

![MIDI note editing followed by a wipe to audio spectral painting](https://github.com/user-attachments/assets/fbe08251-caa0-4dd9-bb86-a642734acd7d)

Current Version: v5.0.3

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
- **[Jamma Concepts and Glossary](doc/glossary.md)**: Plain-language definitions for Scene, Station, LoopTake, Loop, recording actions, timing, quantisation, and NINJAM tempo following.
- **[Overlay Control UI](doc/overlay-controls.md)**: Instructions for the Ctrl-held overlay controls, target selection rules, and quantisation handle behavior.
- **[Loop Grid Editor](doc/loop-grid-editor.md)**: Opening a loop and MIDI editing controls, including note, velocity, pitch-view, and undo gestures.
- **[Console and Controls Reference](doc/performance-console.md)**: Type `/help` or press F1 in the console for all controls, or `/ninjam` (`/n`) for public servers. Ctrl plus backtick in the main window opens the console.
- **[Multi-Device MIDI Trigger Rig Config](doc/midi-trigger-mapping.md)**: Rig file mapping for multiple MIDI devices, channels, and activation mappings.
- **[Ninjam Integration Guide](doc/ninjam.md)**: Vendored files structure, reference dependencies, and update steps.
- **[VS Code Tasks Setup](doc/vscode-tasks.example.json)**: Local configurations for automating MSBuild.

