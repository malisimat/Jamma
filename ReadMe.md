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

### Build

Use the Visual Studio build tasks in VS Code or MSBuild directly. See [.github/copilot-instructions.md](.github/copilot-instructions.md) for detailed build commands and project targets.

### Troubleshooting: tests crash silently on startup

If `JammaLib.Tests.exe` exits immediately with code 1 and no output, the Debug output dir likely has stale Release `gtest.dll`/`gtest_main.dll` (a known MSBuild up-to-date skip issue). Fix by copying the correct debug DLLs:

```powershell
$src = ".\vcpkg_installed\x64-windows\x64-windows\debug\bin"
$dst = ".\test\JammaLib.Tests\bin\x64\Debug"
Copy-Item "$src\gtest.dll"      "$dst\gtest.dll"      -Force
Copy-Item "$src\gtest_main.dll" "$dst\gtest_main.dll" -Force
```

After copying, `gtest.dll` should be ~1.8 MB (the Release version is ~448 KB).
