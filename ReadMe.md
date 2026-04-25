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

Before building, restore NuGet packages for the test suite:

```powershell
& "C:\Users\matto\Downloads\nuget.exe" restore
```

**Note:** The `packages/` directory is excluded from git. You must run `nuget restore` when setting up a fresh clone or worktree.

### Build

Use the Visual Studio build tasks in VS Code or MSBuild directly. See [.github/copilot-instructions.md](.github/copilot-instructions.md) for detailed build commands and project targets.

### Ninjam Integration Prerequisites

Jamma integrates with [NinjamLib](https://github.com/justinfrankel/ninjam) (njclient) for live networked jamming.

The project files contain MSBuild UserMacros with default paths that point to a local developer machine. Override them for your environment — **you do not need to edit the project files directly**. The easiest approach is to set the properties in a `Directory.Build.props` or in Visual Studio's user `.props` file:

```xml
<!-- e.g. %LOCALAPPDATA%\Microsoft\MSBuild\v4.0\Microsoft.Cpp.x64.user.props -->
<PropertyGroup>
  <NinjamRoot>C:\path\to\NinjamLib\ninjam</NinjamRoot>
  <NinjamLibDirDebug>$(NinjamRoot)\bin\x64\Debug\MD</NinjamLibDirDebug>
  <NinjamLibDirRelease>$(NinjamRoot)\bin\x64\Release\MD</NinjamLibDirRelease>
  <!-- vcpkg (ogg/vorbis) – only needed for Jamma.exe link step -->
  <VcpkgMachineLibDirDebug>C:\path\to\vcpkg\installed\x64-windows\debug\lib</VcpkgMachineLibDirDebug>
  <VcpkgMachineLibDirRelease>C:\path\to\vcpkg\installed\x64-windows\lib</VcpkgMachineLibDirRelease>
</PropertyGroup>
```

Required ninjam libraries (linked by Jamma.exe):
- `njclient.lib` — NinjamLib client
- `ogg.lib`, `vorbis.lib`, `vorbisenc.lib`, `vorbisfile.lib` — Xiph codecs (install via `vcpkg install libogg libvorbis`)
- `ws2_32.lib` — Winsock (system library, no installation needed)
