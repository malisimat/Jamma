# NINJAM static-lib integration plan for Jamma

## Worktree

- Repository worktree: `C:\Users\matto\Source\Repos\Jamma-worktree-ninjam`
- Branch: `feature/ninjam-integration-plan`

## Scope

Integrate the static client library at:

- `C:\Users\matto\Source\Repos\NinjamLib\ninjam\bin\x64\Release\MD\njclient.lib`

and add:

1. a connection/session class to own and drive `NJClient`
2. a `StationRemote` class to represent remote NINJAM entities and play them through Jamma's existing station/mixer path

## Approach

- keep a note of tasks and progress in .copilot/ninjam-tasks.md, update it regularly so it can be easily resumed by passing this plan and the ninjam-tasks.md file
- once plan has been fully executed/completed, write this session history to HTML and open in browser
- raise a PR in github, if not already in existence

## Important repo findings

### Jamma layout

- `Jamma` is intentionally thin (`Jamma/src/Main.cpp` only)
- engine/audio/rendering code lives in `JammaLib`
- existing audio path is centered in:
  - `Jamma/JammaLib/src/engine/Scene.h`
  - `Jamma/JammaLib/src/engine/Scene.cpp`
  - `Jamma/JammaLib/src/engine/Station.h`
  - `Jamma/JammaLib/src/engine/Station.cpp`

**`StationRemote` should go in `Jamma/JammaLib/src/engine/StationRemote.*`.**
**`LoopRemote` should go in `Jamma/JammaLib/src/engine/LoopRemote.*`.**

### NINJAM library facts

From `NinjamLib/ninjam/README.md`, `deps.props`, and `ninjam/njclient.h`:

- public API is `ninjam/njclient.h`
- `NJClient::Run()` must be called regularly on a non-audio thread
- `NJClient::AudioProc()` must be called from the audio thread
- `NJClient` manages remote decode/mix internally
- remote state comes from:
  - `GetNumUsers()`
  - `GetUserState()`
  - `EnumUserChannels()`
  - `GetUserChannelState()`
  - `GetUserChannelPeak()`
- remote channel routing can be controlled with `SetUserChannelState(... setoutch ...)`
- `SetWorkDir()` is required because the client writes temp segment files
- the extracted library is GPLv2+ licensed

### Build/runtime facts

- Jamma is MIT licensed, so the GPL dependency is a **merge/distribution gate**
- the provided library is `Release/MD`, so runtime/ABI must match the consuming build
- `njclient` also depends on Ogg/Vorbis libs at final link time. Must first run the following in Jamma repo root to install dependencies `.\vcpkg.exe install libogg:x64-windows libvorbis:x64-windows`
- also link to built static library (MD Multithreaded type) at `C:\Users\matto\Source\Repos\NinjamLib\ninjam\bin\x64\Release\MD\njclient.lib`

## Recommended minimal-change architecture

### 1. New class: `NinjamConnection`

**Location**

- `Jamma/JammaLib/src/io/NinjamConnection.h`
- `Jamma/JammaLib/src/io/NinjamConnection.cpp`

**Responsibility**

- own one `NJClient`
- configure callbacks (`LicenseAgreementCallback`, optional chat/status hooks)
- call `SetWorkDir()`
- connect/disconnect
- pump `Run()` from Jamma's existing job thread
- maintain a lightweight snapshot of remote users/channels
- manage a map of remote user -> assigned output stereo pair
- on the audio thread, call `AudioProc()` into scratch non-interleaved buffers
- expose decoded remote audio blocks to `Scene` / `StationRemote`

**Why this is the smallest change**

- keeps all NINJAM-specific threading and API rules in one place
- prevents `Scene` from becoming full of `NJClient` details
- lets the existing `Scene` audio callback remain the only RtAudio callback path

### 2. New class: `StationRemote`

**Location**

- `Jamma/JammaLib/src/engine/StationRemote.h`
- `Jamma/JammaLib/src/engine/StationRemote.cpp`

**Recommended shape**

- derive from `engine::Station`
- add remote metadata only:
  - remote username / id key
  - current channel count / pair assignment
  - connected/subscribed state
- keep zero `LoopTake`s and zero triggers
- set up exactly the same playback path as a normal station: bus buffers -> station mixers -> scene sink

**Why derive from `Station` instead of inventing a parallel type**

- `Scene::_AddStation(std::shared_ptr<Station>)` already exists
- `Scene` draw/audio loops already iterate `_stations`
- `Station::OnBlockWriteChannel(... AUDIOSOURCE_MIXER)` already writes into bus buffers
- `Station::WriteBlock(...)` already sends mixed audio to the DAC sink

This gives us "play their audio just like an existing Station object" with the least code churn.

### 2b. New class: `LoopRemote`
- Represents visually the last measure of audio received from that ninjam user, up to current time
- Shown as a 3d ring much like the existing Loop, but will be constantly updating its shape
- The visual ring starts off small when no history buffer has yet formed, reaches fixed size based on ninjam reported measure duration
- Current time should visually face the front
- Does not hold a full measure of audio buffer, but does hold a mesh representing the full measure audio envelope, for diplay purposes
- Writes the block of ninjam users audio to the parent StationRemote/LoopTake and out to DAC as if it came from a normal station
- Is coloured a little differently to a normal Station
- There will be one LoopRemote per mono channel (2 per stereo channel) of the ninjam user's audio setup

### 3. Represent one remote **user** as one `StationRemote`

For the first pass, the smallest change is:

- one `StationRemote` per remote NINJAM user
- route all channels for that user to one stereo output pair inside `NJClient`
- feed that stereo pair into the remote station's 2 bus channels

This is simpler than one station per remote channel and still matches the current Jamma station abstraction.

If finer-grained per-channel UI becomes important later, expand the mapping after the basic integration works.

## Critical files / smallest required touch points

### Build plumbing

1. `Jamma/JammaLib/JammaLib.vcxproj`
   - add include path for `NinjamLib/ninjam`
   - add `NinjamConnection.*` and `StationRemote.*` source files
2. `Jamma/JammaLib/JammaLib.vcxproj.filters`
   - add the new files for VS organization
3. `Jamma/Jamma/Jamma.vcxproj`
   - add link settings for:
     - `njclient.lib`
     - Ogg/Vorbis libs used by the NINJAM build
   - add additional library directories as needed
4. `Jamma/test/JammaLib.Tests/JammaLib.Tests.vcxproj`
   - only if tests touch `Scene`/`NinjamConnection`/`StationRemote`

### Engine integration

5. `Jamma/JammaLib/src/engine/Scene.h`
   - add `NinjamConnection` ownership
   - optionally add remote-station bookkeeping
6. `Jamma/JammaLib/src/engine/Scene.cpp`
   - initialize/shutdown NINJAM
   - pump NINJAM from `_JobLoop()` / `OnJobTick()`
   - inject NINJAM audio into `StationRemote`s during `_OnAudio()`
   - Send the audio from all local stations (i.e. not of type StationRemote) out to ninjam server, post mix
   - create/remove remote stations as snapshots change (in a audio / thread safe way)
6b. `Jamma/JammaLib/src/engine/StationRemote.cpp`
   - set up the single LoopTake and all its child LoopRemotes, according to ninjam, users audio setup (how many channels) 
7. `Jamma/JammaLib/src/engine/Station.h`
   - ideally unchanged
8. `Jamma/JammaLib/src/engine/Station.cpp`
   - ideally unchanged

### Files to avoid changing in v1

Leave these alone unless the first integration proves they are truly necessary:

- `Jamma/JammaLib/src/audio/ChannelMixer.*`
- `Jamma/JammaLib/src/engine/Loop*.*`
- `Jamma/JammaLib/src/io/JamFile.*`
- `Jamma/JammaLib/src/io/RigFile.*`
- `Jamma/Jamma/src/Main.cpp`

The goal is to keep the feature runtime-only first, with no save/load file format changes.

## End-to-end flow

### Connection / background flow

1. `Scene` creates `NinjamConnection`
2. `NinjamConnection`:
   - pass configuration params like host, user, pass to constructor
   - sets work dir
   - registers license callback
   - calls `Connect(host, user, pass)`
3. Jamma's existing job thread (`Scene::_JobLoop`) calls `NinjamConnection::Pump()`
4. `Pump()` drives:
   - `while (!client.Run()) {}` style scheduling
   - remote user/channel snapshot refresh
   - station add/remove/update requests

### Remote entity discovery flow

1. `NinjamConnection` enumerates remote users/channels
2. It builds a snapshot keyed by username
3. `Scene` compares snapshot to current remote stations
4. For each new remote user:
   - create `StationRemote`
   - `_AddStation(remoteStation)`
   - assign it a stereo pair in NINJAM output routing
5. For removed users:
   - mute/disable or remove the corresponding `StationRemote`

### Audio flow (minimal-change version)

1. RtAudio calls `Scene::AudioCallback()`
2. `Scene::_OnAudio()` continues to run the existing local Jamma path
3. Inside `_OnAudio()`, before the final station mix-out:
   - call `NinjamConnection::ProcessAudioBlock(...)`
   - this calls `NJClient::AudioProc()` with scratch non-interleaved output buffers
4. For each `StationRemote`:
   - copy its assigned stereo pair from the NINJAM scratch output into the station using `OnBlockWriteChannel(... AUDIOSOURCE_MIXER)`
   - update the LoopRemote children of the one child LoopTake, so they can update their buffers and mesh for display
   - call `EndMultiWrite(... AUDIOSOURCE_MIXER)`
5. Existing `Station::WriteBlock()` then mixes that audio to the scene sink exactly like local stations

That reuse of `Station::OnBlockWriteChannel()` + `Station::WriteBlock()` is the key minimal-change path.

## Concrete implementation plan

### Phase 0 - gates

- Confirm the GPLv2+ implications are acceptable before merging/distributing
- Build a matching NINJAM binary set for the configurations we care about:
  - at minimum `x64 Release /MD`
  - ideally also `x64 Debug /MDd`
- Confirm which codec/import libs the final Jamma link actually needs

### Phase 1 - build wiring

- Add the NINJAM include directory to `JammaLib.vcxproj`
- Add new source/header files to `JammaLib.vcxproj` + `.filters`
- Add final-link dependencies to `Jamma.vcxproj`
- Prefer a property macro or `.props` file over hard-coding absolute paths in multiple places

Suggested property inputs:

- `NinjamRoot=C:\Users\matto\Source\Repos\NinjamLib\ninjam`
- `NinjamLibDir=$(NinjamRoot)\bin\x64\Release\MD`
- `NinjamIncludeDir=$(NinjamRoot)`

### Phase 2 - `NinjamConnection`

Implement a wrapper with roughly these responsibilities:

- `bool Connect(host, user, pass)`
- `void Disconnect()`
- `void Pump()`
- `void SetAudioFormat(sampleRate, numFrames)`
- `void ProcessAudioBlock(unsigned int numFrames, unsigned int sampleRate)`
- `RemoteSnapshot Snapshot() const`
- `bool ConsumeRemoteMixForUser(const std::string& user, float*& left, float*& right, unsigned int& numFrames)`

Internally:

- allocate scratch `float*[]` output buffers for remote stereo pairs
- optionally prepare local input buffers later for transmit support
- keep `NJClient` access synchronized so snapshot reads do not race `Run()`

### Phase 3 - `StationRemote`

Implement as a thin `Station` subclass:

- constructor sets name and a fixed 2-bus layout (will be made configurable in later updates)
- disable triggers, single take (each remote ninjam user has one stream of audio)
- show scaled vertically *2 that of typical stations
- expose methods like:
  - `SetRemoteUserName(...)`
  - `IngestStereoBlock(const float* left, const float* right, unsigned int numFrames)`

`IngestStereoBlock()` should simply write into bus channels 0 and 1 through the normal station sink API.

### Phase 4 - scene integration

Add to `Scene`:

- create `NinjamConnection` during scene startup or audio init
- call `Pump()` from `_JobLoop()` / `OnJobTick()`
- reconcile remote snapshots to live `StationRemote` objects
- in `_OnAudio()`, ingest remote blocks before the existing station playback pass

Recommended placement in `_OnAudio()`:

- after input capture handling
- before iterating `_stations` for final `WriteBlock()` output

This lets remote stations participate in the same final mix pass as local ones.

### Phase 5 - optional local transmit

After receive/playback is stable:

- map selected Jamma ADC channels into `NJClient::SetLocalChannelInfo(...)`
- feed deinterleaved local input into `NJClient::AudioProc()`
- notify server on channel changes

This should be a second step, not part of the first merge.

## Test strategy

### Low-risk unit tests

Add tests for `StationRemote` and `LoopRemote` only:

- writing stereo samples into a remote station produces the expected mixed output
- remote station uses existing station bus/mixer path unchanged

This can follow the pattern in:

- `Jamma/test/JammaLib.Tests/src/engine/FlipBuffer_Tests.cpp`

### Manual/integration checks

1. launch Jamma
2. connect to a known NINJAM server (web search if needed - store host in jamma config)
3. verify remote users appear as stations
4. verify remote audio is audible through the normal output path
5. verify user join/leave updates stations cleanly
6. verify the app exits cleanly with disconnect + workdir cleanup

## Risks / open questions

### 1. License compatibility

Biggest non-technical issue:

- Jamma = MIT
- `njclient` extraction = GPLv2+

If the plan is to distribute a linked binary, this needs an explicit decision before implementation proceeds.

### 2. Debug vs release compatibility

The provided library is specifically:

- `...\Release\MD\njclient.lib`

That is not enough for a robust day-to-day dev workflow unless we also build the matching debug flavor.

### 3. Codec/link dependencies

Need to verify the final linker inputs used by the extracted NINJAM client build. Expect some combination of:

- Ogg import/static lib
- Vorbis lib
- Vorbis encoder lib

and possibly runtime DLL copy steps.

### 4. Work directory lifecycle

`NJClient` writes temp files. We need:

- a stable work dir under roaming/local app data or project temp
- cleanup rules
- protection against stale files after crashes

### 5. Remote-user identity

For v1, keying by username is probably enough.
If the server allows collisions or rename edge cases, we may need a stronger identity key later.

## Proposed first implementation cut

If we want the quickest path to something working:

1. wire build for `x64 Release`
2. add `NinjamConnection`
3. add `StationRemote : Station`
4. create one remote station per remote user
5. route each user to one stereo pair
6. inject that pair into the station via `AUDIOSOURCE_MIXER`
7. leave persistence, transmit, chat UI, and per-channel remote UI for later

That gives the smallest diff across the current Jamma architecture while still respecting `NJClient`'s threading/audio contract.
