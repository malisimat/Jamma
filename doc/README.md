# Jamma Documentation

## Developer Resources

- [Build and Test Guide](build.md)
- [Real-Time Audio Guidance](realtime-audio.md)
- [Multi-Device MIDI Trigger Rig Configuration](midi-trigger-mapping.md)
- [Ninjam Integration Guide](ninjam.md)
- [VS Code Tasks Starter](vscode-tasks.example.json)

## Project Structure

- `Jamma`: Application shell, startup, and top-level wiring.
- `JammaLib`: Engine, loop model, audio behavior, rendering/audio support layers.
- `test/JammaLib_Tests`: Native GoogleTest coverage for engine/audio behavior.

## Core Domain Model

The looping model is hierarchical:

- `Station`: Top-level loop station state and coordination.
- `LoopTake`: A take/layer grouping within a station.
- `Loop`: Underlying sample data and playback/record behavior.

This structure helps isolate responsibilities while keeping composition simple.

## High-Level Architecture

![Project architecture](diagrams/architecture.svg)

Notes:

- The app project (`Jamma`) should stay thin and delegate engine behavior to `JammaLib`.
- Tests target `JammaLib` behavior directly for fast feedback and minimal UI coupling.

## Audio Signal Flow

![Audio flow](diagrams/audio-flow.svg)

Capture path:

1. ADC input enters the mixer capture buffer.
2. Monitor and latency-compensated routing are applied.
3. Samples are written into `Station -> LoopTake -> Loop`.

Playback path:

1. Loop model reads/mixes content for output.
2. Audio data is accumulated through buffer/mixer stages.
3. Channel mixer sends final output to DAC.

