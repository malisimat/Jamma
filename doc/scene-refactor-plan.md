# Plan: Subsystem Extraction & Service Injection

## Overview
The `Scene` class is currently a "God Object" managing everything from hardware routing to VST OS windows, making it enormous and hard to maintain. Plan A focuses on vertical slicing: extracting distinct subsystems into independent managers, and demoting `Scene` to a pure "Workspace Tree" manager. `Scene` will only handle visual fan-out to `Station`s, `Overlay`s, and high-level scene graphics.

## Extracted Subsystems

### 1. `AudioEngine`
- **Responsibilities**: Absorbs `AudioDevice`, `ChannelMixer`, master audio callback loop, sample counters, and latency checks.
- **Goal**: Abstract the audio callback. `Scene` no longer manages audio hardware initiation or teardown (`InitAudio()`).

### 2. `InputSubsystem`
- **Responsibilities**: Owns the `MidiRouter`, `_PumpMidi()`, serial hardware `_PumpSerial()`, global touch/mouse dispatcher.
- **Goal**: Cleans up hardware polling from `Scene::Update()`. `InputSubsystem` translates hardware inputs into generic actions sent to the current scene/stations.

### 3. `NetworkService` / `NinjamController`
- **Responsibilities**: Manages the Ninjam session connections, chat dispatches, and incoming tempo-sync payloads.
- **Goal**: Removes networking and multi-threading complexities from `Scene`.

### 4. `WindowSubsystem` 
- **Responsibilities**: OS-level windowing logic, tracking and pruning OS windows (e.g., `_vstEditorWindows`, `_OpenVstEditorForPlugin()`).
- **Goal**: GUI-agnostic window lifecycle management moves out of the core tree logic.

## Changes to `Scene`
- Functions like `InitAudio()`, `CloseMidi()`, and `SendNinjamChat()` move entirely out of `Scene`.
- `Scene` is injected with access to these isolated services as needed, but remains focused internally on rendering and fanning-out visual actions.
- **Code Reduction**: Combines repeated loops iterating over stations/children for drawing vs. hit-testing by introducing generic traversals. 

## Advantages
- **Surgical Risk**: Less disruptive than a total rewrite. Leaves the `Scene` conceptually intact but severely trims its fat.
- **Encapsulation**: Eliminates shared lock contention across fundamentally unrelated concerns (e.g., MIDI vs Window handles). 
