# Jamma

[![Build Status](https://dev.azure.com/mattojones/mattojones/_apis/build/status/Jamma-x64-Release?branchName=master)](https://dev.azure.com/mattojones/mattojones/_build/latest?definitionId=1&branchName=master)

Multichannel loopsampling software for recording and live performance.

![JammaV_Screenshot](https://user-images.githubusercontent.com/24556021/69101042-76834100-0a56-11ea-9340-a9e192fb5430.gif)

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

## Building

### Prerequisites

- Visual Studio 2022 with C++ desktop development tools
- Windows SDK 10.0

### Setup (First Time or Fresh Worktree)

Before building, install dependencies with vcpkg from the repository root:

```powershell
vcpkg integrate install
vcpkg install
```

This project uses `vcpkg.json` manifest mode to install dependencies (including Google Test).

Windows builds also compile VST3 hosting support by default via the `vst3sdk` vcpkg dependency declared in `vcpkg.json`.

### Build

Use the Visual Studio build tasks in VS Code or MSBuild directly. See [.github/copilot-instructions.md](.github/copilot-instructions.md) for detailed build commands and project targets.

Direct `*.vcxproj` builds now inherit `$(SolutionDir)` from [Directory.Build.props](Directory.Build.props), so running MSBuild against an individual project from the repo root no longer needs a manual `/p:SolutionDir=...` workaround.

### Troubleshooting: tests crash silently on startup

If `JammaLib_Tests.exe` exits immediately with code 1 and no output, the Debug output dir likely has stale Release `gtest.dll`/`gtest_main.dll` (a known MSBuild up-to-date skip issue). Fix by copying the correct debug DLLs:

```powershell
$src = ".\vcpkg_installed\x64-windows\debug\bin"
$dst = ".\test\JammaLib_Tests\bin\x64\Debug"
Copy-Item "$src\gtest.dll"      "$dst\gtest.dll"      -Force
Copy-Item "$src\gtest_main.dll" "$dst\gtest_main.dll" -Force
```

After copying, `gtest.dll` should be ~1.8 MB (the Release version is ~448 KB).

## Multi-Device MIDI Trigger Rig Config

Rig files can map station triggers to MIDI Note or CC input and can record MIDI loops from multiple configured MIDI devices at the same time.

- `user.midi.devices` lists the MIDI inputs Jamma should open.
- `triggers[].trigger.device` selects the single device that controls trigger activation and ditch.
- `triggers[].midiinput` controls which one-based MIDI channels are recorded into MIDI loops.
- `triggers[].midiinputdevices` controls which MIDI devices are recorded into MIDI loops.

Example:

```json
{
	"name": "rig",
	"user": {
		"audio": { "name": "HDMI", "bufsize": 255, "inlatency": 414, "outlatency": 414, "numchannelsin": 0, "numchannelsout": 10 },
		"midi": {
			"devices": [
				{ "name": "TriggerPad", "enabled": true },
				{ "name": "Keys A", "enabled": true },
				{ "name": "Keys B", "enabled": true }
			]
		}
	},
	"triggers": [
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
	]
}
```

Notes:

- `device` may match one of the names in `user.midi.devices` if the same controller should both play/record MIDI loops and trigger recording.
- Device names are matched exactly. Startup logs report trigger devices or loop-record devices that do not match an active MIDI input.
- `channel` is one-based in the rig file. If omitted, the mapping matches any MIDI channel.
- `kind` supports `note` and `cc`.
- `note` uses Note On as press and Note Off or Note On with velocity 0 as release.
- `cc` uses values greater than 0 as press and `0` as release.
- Velocity is ignored for trigger purposes.

### Ninjam Integration

Jamma now uses vendored Ninjam client files under `lib/`:

- Header include root: `lib\njclient\njclient.h`
- x64 Debug lib: `lib\njclient\x64\Debug\MD\njclient.lib`
- x64 Release lib: `lib\njclient\x64\Release\MD\njclient.lib`

You do **not** need `Directory.Build.local.props` for Ninjam paths anymore.

### Refreshing Vendored Ninjam Files (Only When Updating Them)

You only need this step when you intentionally update the vendored Ninjam artifacts.

1. Build Ninjam in the other repo for `x64` `Debug` and `Release` (`MD` runtime).
2. Copy updated headers/libs into this repo:

```powershell
$ninjam = "C:\Users\<you>\Source\Repos\NinjamLib\ninjam"

Copy-Item "$ninjam\ninjam\njclient.h" ".\lib\njclient\njclient.h" -Force

Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.lib" ".\lib\njclient\x64\Debug\MD\njclient.lib" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.pdb" ".\lib\njclient\x64\Debug\MD\njclient.pdb" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.idb" ".\lib\njclient\x64\Debug\MD\njclient.idb" -Force
Copy-Item "$ninjam\bin\x64\Release\MD\njclient.lib" ".\lib\njclient\x64\Release\MD\njclient.lib" -Force
```

### Troubleshooting: Ninjam include/link errors

If you see errors like:

- `Cannot open include file: 'njclient.h'`
- linker errors for `njclient.lib`

verify the vendored files above exist in `lib\njclient`.

Required Ninjam-related link dependencies:
- `njclient.lib` from `lib\njclient\x64\...`
- `ogg.lib`, `vorbis.lib`, `vorbisenc.lib`, `vorbisfile.lib` (provided via vcpkg)
- `ws2_32.lib` (Windows SDK)
