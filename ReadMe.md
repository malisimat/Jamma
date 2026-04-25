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

Before building, install the repo dependencies with vcpkg:

```powershell
# Install/bootstrap vcpkg once per machine (skip if already done)
git clone https://github.com/microsoft/vcpkg C:\Users\matto\Source\Repos\vcpkg
& "C:\Users\matto\Source\Repos\vcpkg\bootstrap-vcpkg.bat"

# Enable Visual Studio/MSBuild integration once per machine
& "C:\Users\matto\Source\Repos\vcpkg\vcpkg.exe" integrate install

# From the Jamma repo root, install manifest dependencies into .\vcpkg_installed\
& "C:\Users\matto\Source\Repos\vcpkg\vcpkg.exe" install --triplet x64-windows

# feature/ninjam-integration also needs NINJAM link dependencies
& "C:\Users\matto\Source\Repos\vcpkg\vcpkg.exe" install libogg:x64-windows libvorbis:x64-windows
```

**Note:** The repo-local `vcpkg_installed/` directory is excluded from git. You must install dependencies when setting up a fresh clone or worktree.

### Build

Use the Visual Studio build tasks in VS Code or MSBuild directly. See [.github/copilot-instructions.md](.github/copilot-instructions.md) for detailed build commands and project targets.
