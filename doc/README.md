# Jamma Documentation


## Build Instructions

* Prerequisites:
 - Windows
 - Visual Studio 2026 (C++ desktop workload), or VS Code with the C/C++ extension and Visual Studio 2026 Build Tools installed
 - Windows SDK 10.0

* Configuration:
 - Toolset: v145
 - Language standard: stdcpplatest
 - Platform: x64

* Building:

  1. Locate MSBuild using vswhere
```bat
for /f "usebackq tokens=*" %%i in (
  `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`
) do set MSBUILD=%%i
```

2. Build solution

```bat
"%MSBUILD%" Jamma.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64
"%MSBUILD%" Jamma.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
```

If you have visual studio installed, then through PowerShell:

```powershell
$msbuild = Join-Path $env:VSINSTALLDIR "MSBuild\Current\Bin\MSBuild.exe"
& $msbuild JammaLib\JammaLib.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild test\JammaLib_Tests\JammaLib_Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
```

Solution build (sparingly):

```powershell
$msbuild = Join-Path $env:VSINSTALLDIR "MSBuild\Current\Bin\MSBuild.exe"
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
```


## Project Structure

- `Jamma`: application shell, startup, and top-level wiring.
- `JammaLib`: engine, loop model, audio behavior, rendering/audio support layers.
- `test/JammaLib_Tests`: native GoogleTest coverage for engine/audio behavior.

## Core Domain Model

The looping model is hierarchical:

- `Station`: top-level loop station state and coordination.
- `LoopTake`: a take/layer grouping within a station.
- `Loop`: underlying sample data and playback/record behavior.

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

## Multi-Device MIDI Trigger Mapping

Rig files support multiple configured MIDI input devices. MIDI trigger activation uses one device per trigger, while MIDI loop recording can subscribe to one or more devices independently.

Example rig fragments:

```json
{
  "user": {
    "midi": {
      "devices": [
        { "name": "TriggerPad", "enabled": true },
        { "name": "Keys A", "enabled": true },
        { "name": "Keys B", "enabled": true }
      ]
    }
  }
}
```

Example trigger block:

```json
{
  "name": "Trig1",
  "stationtype": 0,
  "midiinput": [1],
  "midiinputdevices": ["Keys A", "Keys B"],
  "trigger": {
    "type": "midi",
    "device": "TriggerPad",
    "activate": { "kind": "note", "channel": 1, "id": 60 },
    "ditch": { "kind": "cc", "channel": 1, "id": 64 }
  }
}
```

Behavior:

- `user.midi.devices` is the supported MIDI config shape. The old single-device `user.midi.name` / `enabled` shape is rejected.
- Each enabled device gets its own MIDI callback endpoint and ingress queue.
- `trigger.device` selects the input device that can activate or ditch that station trigger.
- `midiinput` selects the one-based MIDI channels recorded into MIDI loops.
- `midiinputdevices` selects the MIDI input devices recorded into MIDI loops. If omitted, MIDI loop recording keeps the legacy channel-only behavior.
- The same MIDI channel from different devices is recorded into distinct MIDI loop streams when both devices are listed.
- `channel` values in trigger bindings are one-based. Omitting `channel` makes the binding match any channel.
- Device names are matched exactly against configured active MIDI input names. Startup logs report unresolved trigger devices and unresolved loop-record devices.
