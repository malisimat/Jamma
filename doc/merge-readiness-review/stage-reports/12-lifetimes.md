# Stage 12 — Resource and lifetime review

## Assignment

- Stage: 12 — Resource and lifetime review.
- Primary ownership: construction/destruction and ownership of timing/session buffers, queues, registrations, threads, callbacks, and network/session objects; leaks, stale callbacks, overflow policies, shutdown races/order.
- Scope restriction: NINJAM timing/remote-sync seams only, per the Phase 1 human decision. HUD, VST3 parity, window placement, general tooling, and unrelated resources are excluded.
- Explicit exclusion: shared-state race proof while objects are live belongs to Stage 7. Stage 12 reviews whether objects and borrowed storage remain alive for the full operation and whether shutdown ordering retires callbacks/resources safely.
- Status: read-only investigation. Only this report was created; no source, test, build, project, or other artifact was edited.

## Coverage

### Inputs consumed

- `AGENTS.md`, the merge-readiness plan, Phase 2 specification, `decisions.md`, inventory, Phase 1 packet/findings, verification matrix, timing design, and real-time guide.
- Stage 9 report and its session/generation handoffs.
- Hot-path handoff concerning repeated `NinjamSession::_AcquireConnectionUse` and NJClient getters on the audio callback.

### Implementation and contracts traced

- Scene/job/audio shutdown: `JammaLib/src/engine/Scene.{h,cpp}`, `JammaLib/src/audio/AudioHost.{h,cpp}`, and `AudioDevice.cpp`.
- Controller/session/connection lifetime: `JammaLib/src/ninjam/NinjamController.{h,cpp}`, `NinjamNetworkService.{h,cpp}`, `NinjamSession.{h,cpp}`, and `NinjamConnection.{h,cpp}`.
- Remote output borrower/consumer: `JammaLib/src/audio/AudioHost.cpp` and `JammaLib/src/engine/StationRemote.cpp`.
- Timing command/observation mailboxes, Timer shared ownership, export scratch/delay buffers, NJClient callbacks, and disconnect paths.
- Vendored NJClient thread contract at `lib/njclient/njclient.h:155`-`:172` and timing getter declarations at `:198`-`:205`, `:273`.
- Relevant history: `34e9693` (session extraction/borrowed stereo API), `5f8d33e` (active-user lifetime guard), `12e73ee` (reconnect hardening), `adb7b38` (NINJAM audio wiring), `782b8a8` (latency compensation), `d0d208e` (live timing getter), `d6fdb88` (unified timing command), and `b2016ae` (teardown fix lineage).

### Commands and queries used

- `Get-Content -Raw` and numbered `Get-Content` slices for all governing artifacts and implementation paths above.
- `rg -n` for destructors, start/stop/connect/disconnect, audio callbacks, `NinjamConnectionUse`, `_client`, `AudioProc`, timing getters, pointer-return APIs, threads, callbacks, and existing tests.
- Focused `git log --oneline` and `git blame -L` for connection publication, active-user guards, borrowed output pointers, AudioHost ingestion, timing getters, and AudioProc.
- No build, native test, sanitizer, or runtime shutdown scenario was executed in this read-only stage.

### Deliberate exclusions

- VST teardown ordering in `Scene::Shutdown` is outside the human timing-only scope even though it is adjacent to audio shutdown.
- The detached public-server-directory refresh is not part of timing/session playback and its actual callback is a process-level function; it was noted but not developed as a finding.
- Whether NJClient getters race internally with `Run()` is Stage 7 ownership. S12 records the explicit upstream lifetime/thread-use contract violation and the unsupported ownership boundary.
- The unbounded cost of `_AcquireConnectionUse` and callback traversal is Stage 8 ownership.
- Arithmetic overflow policies in timing coordinates are Stage 11 ownership. Command coalescing correctness is S09-01.

## System understanding

`Scene` owns `AudioHost` and `NinjamNetworkService`. The service owns a shared `NinjamController`; AudioHost also holds that controller so the controller outlives either facade independently (`Scene.h:356`-`:360`; `AudioHost.cpp:26`-`:36`). The controller owns `NinjamSession`, which owns one `NinjamConnection` through `unique_ptr`. The session publishes a raw connection pointer plus an active-user count. Every normal job/audio call acquires `NinjamConnectionUse`; stop exchanges the published pointer to null, waits until active users drain, moves out the unique owner, then disconnects/destroys it (`NinjamSession.cpp:345`-`:430`). This guard protects the connection only until the RAII use object is destroyed.

The connection owns NJClient, all scratch vectors, export delay lines, temporary interleaved buffers, reconnect state, and NJClient callback user data (`NinjamConnection.h:180`-`:230`). Audio-format setup sizes these buffers before callback processing (`NinjamConnection.cpp:406`-`:469`). The connection guard safely encloses `ProcessExportBlock`, including `AudioProc`, and safely encloses value-returning `GetLiveTiming`. It does not enclose AudioHost's use of raw pointers returned by `ConsumeStereoPair`, which is the central lifetime defect.

Normal full shutdown is otherwise ordered conservatively: Scene stops/joins its job thread, then `AudioHost::Close` stops/closes the RtAudio stream before stopping the NINJAM controller (`Scene.cpp:1639`-`:1649`; `AudioHost.cpp:94`-`:106`; `AudioDevice.cpp:73`-`:85`). Consequently the Scene-capturing tick callback and NJClient `AudioProc` should be quiescent before controller/session destruction during full application shutdown. Explicit connect/disconnect while audio remains active instead relies on `NinjamConnectionUse` and is where the borrowed-pointer defect is reachable.

## Lifetime and cleanup table

| Resource / registration | Owner / construction | Live users / handoff | Cleanup and ordering | Assessment |
| --- | --- | --- | --- | --- |
| RtAudio callback and `AudioHost*` user data | `AudioDevice::Open` from `AudioHost::Init` (`AudioHost.cpp:26`-`:40`) | RtAudio invokes static callback, then `_OnAudio`; tick callback captures `Scene` (`Scene.cpp:1582`-`:1593`) | `AudioHost::Close` stops/closes stream before controller stop; Scene calls it before destruction (`AudioHost.cpp:94`-`:106`; `Scene.cpp:1639`-`:1649`) | Lifetime ordering sound for full shutdown. |
| Shared `NinjamController` | `NinjamNetworkService` creates `shared_ptr`; AudioHost retains another (`NinjamNetworkService.cpp:8`-`:12`; `AudioHost.cpp:26`-`:31`) | Job/UI through service; audio through host | Host stops controller after audio stream stops; shared ownership prevents facade-order dangling | Sound. |
| Published `NinjamConnection` | Session `unique_ptr`, atomic raw publication (`NinjamSession.h:145`-`:153`; `NinjamSession.cpp:383`-`:417`) | Each operation uses active-user RAII guard (`:362`-`:380`) | Stop unpublishes, waits for users, then disconnects (`:350`-`:359`, `:420`-`:430`) | Sound only while data use stays inside guard. |
| Remote stereo pointers | `_outScratch` vectors owned by `NinjamConnection` (`NinjamConnection.h:201`-`:204`) | `ConsumeStereoPair` returns `.data()` pointers (`NinjamConnection.cpp:793`-`:813`); session guard ends on return (`NinjamSession.cpp:508`-`:518`); AudioHost consumes afterward (`AudioHost.cpp:419`-`:435`) | Concurrent Stop can destroy old connection/vectors after guard release | Unsafe; S12-01. |
| NJClient object and callback user data | `NinjamConnection::_client` unique owner, chat user points to `this` (`NinjamConnection.cpp:62`-`:74`, `:189`-`:214`) | `Run` on job thread; `AudioProc` on audio thread; other operations through session guard | Stop waits guarded users before `Disconnect` and destruction (`NinjamSession.cpp:350`-`:430`) | Object lifetime itself sound; audio-thread API use unsupported, S12-02. |
| NJClient timing access | Connection methods call `GetPosition`, sample rate, BPM, BPI (`NinjamConnection.cpp:588`-`:605`, `:709`-`:730`) | Both calls originate in AudioHost callback (`AudioHost.cpp:437`-`:453`, `:476`-`:485`) | No separate cached/published owner; relies on NJClient internals despite contract | Unsupported ownership/thread contract; S12-02. |
| Export scratch/delay buffers | Connection vectors/shared AudioBuffers, sized by `SetAudioFormat` (`NinjamConnection.cpp:406`-`:469`) | Audio callback only after setup; guarded with connection | Destroyed with connection after active use; oversize block skips processing (`:546`-`:565`) | No leak/stale-use found inside guarded `ProcessExportBlock`. |
| Timing command and observation mailboxes | `AudioHost` value members (`AudioHost.h:113`-`:127`) | Job/audio latest-value handoffs | Destroyed after audio stream is stopped; no external pointers | Lifetime sound; semantic coalescing is Stage 9. |
| Shared Timer | Quantiser produces shared clock; AudioHost holds atomic `shared_ptr` (`Scene.cpp:1591`-`:1593`; `AudioHost.h:127`) | Audio callback loads a strong ref | Host remains alive until stream stop; strong ref protects Timer | Lifetime sound. |
| Remote Station scratch consumer | Station snapshots retain station; `StationRemote::IngestStereoBlock` synchronously reads pointers (`StationRemote.cpp:141`-`:173`) | Audio callback | Station lifetime retained, but source buffer lifetime is not | Source defect is S12-01. |
| Scene job thread | Started with captured `this` (`Scene.cpp:244`-`:247`) | `OnJobTick`, including connection `Run` | `_isSceneQuitting`, join before resource release (`Scene.cpp:1639`-`:1649`, `:2288`-`:2295`) | Sound. |

## Candidate findings

### S12-01 — Remote stereo buffer pointers outlive the connection-use guard

- Stage / reviewer: S12 resource and lifetime review.
- Scope reviewed / exclusions: session/connection/output-buffer lifetime across explicit stop/reconnect; live buffer race proof belongs to Stage 7.
- Severity: merge blocker.
- Evidence: `NinjamConnection::ConsumeStereoPair` returns raw pointers into connection-owned `_outScratch` vectors (`JammaLib/src/ninjam/NinjamConnection.cpp:793`-`:813`; API lineage `34e9693`). `NinjamSession::ConsumeStereoPair` creates `NinjamConnectionUse`, forwards those pointers, and destroys the guard when the function returns (`JammaLib/src/ninjam/NinjamSession.cpp:508`-`:518`; guard introduced at `5f8d33e`). AudioHost then computes a frame count and passes the pointers to `StationRemote::IngestStereoBlock` after the guard is gone (`JammaLib/src/audio/AudioHost.cpp:419`-`:435`); ingestion reads both buffers repeatedly at `JammaLib/src/engine/StationRemote.cpp:141`-`:173`. Meanwhile `Stop` may unpublish the connection, observe zero active users, move/destroy it and its vectors (`NinjamSession.cpp:350`-`:359`, `:420`-`:430`).
- Why it matters: disconnect/reconnect can free the remote output storage between pointer return and ingestion, producing a callback-side use-after-free. The existing active-user design gives a false guarantee because it protects the producer call but not the borrow lifetime.
- Recommended disposition: keep the connection-use guard alive through the complete synchronous copy/ingest, or replace the pointer-return API with a caller-owned preallocated destination/copy operation performed under the guard. Do not solve this with per-block heap ownership or destruction on the audio thread. If an API callback is used to keep the borrow scoped, keep it allocation-free and non-throwing and do not add a new class solely for this fix.
- Protected timing concepts affected: none directly; remote audio/session lifetime only. A reconnect fix must not alter local/remote timing authority.
- Verification: prerequisite stress regression that repeatedly starts/stops/reconnects while audio consumes remote stereo; use address sanitizer or page-heap/application verifier if supported; verify silence/clean handoff, no UAF, and no callback allocation. Add a focused lifetime test with a controlled stop attempt between acquire and consumption.
- Human decision: pending.

### S12-02 — Audio callback invokes NJClient APIs outside the supported `AudioProc` contract

- Stage / reviewer: S12 resource and lifetime review; hot-path/API handoff corroborated by Stage 8 investigator.
- Scope reviewed / exclusions: external object's documented thread-use/lifetime contract. Whether individual fields race internally is Stage 7.
- Severity: must fix before merge.
- Evidence: vendored NJClient explicitly says, "call AudioProc, (and only AudioProc) from your audio thread" at `lib/njclient/njclient.h:165`-`:172`. AudioHost calls `GetLiveTiming` from `_OnAudio` (`JammaLib/src/audio/AudioHost.cpp:437`-`:453`), which calls `_client->GetPosition`, `GetSampleRate`, `GetActualBPM`, and `GetBPI` (`JammaLib/src/ninjam/NinjamConnection.cpp:709`-`:730`; live getter introduced at `d0d208e`). Later in the same callback, export latency compensation calls `_client->GetPosition` immediately before `_client->AudioProc` (`NinjamConnection.cpp:579`-`:605`, `:698`-`:704`; getter path `782b8a8`). Human decisions prohibit modifying upstream NJClient.
- Why it matters: NJClient is an externally owned network/audio object with an explicit cross-thread use contract. Calling unsupported getters concurrently with job-thread `Run()` can expose internal storage outside its intended synchronization/lifetime regime and invalidates any claim that shutdown/live use is safe merely because the outer `NinjamConnection` remains alive.
- Recommended disposition: keep only `AudioProc` on the audio thread. Publish a coherent job-thread timing snapshot through the existing fixed-size/latest-value pattern and project it to device/audio boundary coordinates, or identify a supported NJClient audio-callback data path. For export compensation, derive from the supported snapshot/device counter rather than editing NJClient or calling getters around `AudioProc`. Preserve the distinct Timer-absolute, device-audio, and remote phase domains from F-015/S09.
- Protected timing concepts affected: remote timing observation, device-audio observation coordinate, Timer-absolute coordinate, and source-rate/device-rate conversion must remain distinct.
- Verification: static audit proving `AudioProc` is the only NJClient method reachable from `_OnAudio`; delayed-snapshot/phase-projection tests; reconnect and disconnect stress; remote export latency scenario; Stage 7 proof that the replacement publication is coherent.
- Human decision: pending.

## Shutdown ordering constraints

1. Stop and close RtAudio before destroying/stopping the controller during full shutdown. Current `AudioHost::Close` follows this order (`AudioHost.cpp:94`-`:106`).
2. Join Scene's job thread before destroying the network service/coordinator. Current `Scene::Shutdown` does so (`Scene.cpp:1639`-`:1649`).
3. On live disconnect/reconnect, unpublish the connection before disconnect/destruction and prevent new guards from attaching; wait for every in-flight guarded operation. Current session logic does this, but every borrow from connection-owned storage must remain within that guard.
4. Do not hold `_lifecycleMutex` while invoking NJClient `Disconnect` or destroying the connection. Current Start/Stop move the old owner out before disconnect (`NinjamSession.cpp:383`-`:430`), avoiding lock-callback recursion.
5. NJClient chat callback user data must remain valid through `Run`/disconnect. Waiting for active `Pump` before connection destruction currently provides this, assuming all NJClient calls obey the documented thread roles.
6. Any S12-01 replacement must not transfer final `shared_ptr` release or buffer destruction to the callback; use caller-owned buffers or scoped synchronous access.
7. Any S12-02 replacement must retain observation timestamps so job-thread snapshot age can be projected at the audio boundary; a stale unanchored value is not an acceptable lifetime workaround.

## Handoffs

- **Stage 7:** prove NJClient `Run`/snapshot publication coherence after removing audio-thread getters; assess live data access to connection scratch/state. S12-01 does not require a live-race proof to establish the escaped borrow.
- **Stage 8:** record `_AcquireConnectionUse`'s unbounded retry loop (`NinjamSession.cpp:362`-`:375`) and repeated per-station guard acquisitions as callback cost. S12's fix must not introduce locks, allocation, waits, or callback destruction.
- **Stage 9/10:** reconnect/departure scenarios must include S12-01's scoped remote-audio borrow and S12-02's cached observation authority. Do not allow a new connection to mix old timing/raw buffers into the same logical command epoch.
- **Stage 11:** determine timestamp width/wrap policy for the snapshot/device-counter projection required by S12-02.
- **Stage 14/20:** add lifecycle stress evidence; there are currently no focused `NinjamSession`/`NinjamConnection` lifetime tests under `test/JammaLib_Tests/src`.
- **Phase 4 implementation:** S12-01 is a narrow prerequisite safety fix. Coordinate S12-02 with F-009 command materialization and F-015 coordinate naming instead of adding a parallel timing owner.

## Uncertainties

- RtAudio's `stopStream`/`closeStream` is assumed to quiesce its callback before returning; this is consistent with the wrapper's intended ownership but was not validated against a live ASIO driver in this read-only stage.
- NJClient getter implementations are not present beside their declarations in the vendored tree searched. The explicit header contract is sufficient to reject the calls, but Stage 7 cannot inspect their internal synchronization here.
- A deterministic UAF unit test may need a narrow test seam around scoped connection use; production code should not gain a broad raw-session API for testing.
- `NinjamSession::_UnpublishConnectionLocked` busy-waits only after publishing null, so new users cannot starve it indefinitely. A hung external `AudioProc` could still stall stop; no independent evidence establishes such a hang, so this is a residual shutdown dependency rather than a finding.
- Full Scene shutdown stops the audio device before controller stop, so S12-01 is principally reachable during explicit connect/disconnect/reconnect while audio continues, not ordinary final destruction.

## Conclusion

The high-level owner chain is disciplined: Scene/audio stop ordering, shared controller lifetime, session unique ownership, active-use retirement, Timer strong ownership, mailbox storage, connection buffers, and NJClient callback user data have identifiable owners and cleanup points. No leak was found in the timing/session scope.

Phase 2 cannot accept resource/lifetime safety yet. S12-01 is a concrete callback-side use-after-free window because remote stereo pointers escape the only connection lifetime guard; it is a merge blocker. S12-02 violates NJClient's explicit audio-thread API contract by calling timing getters alongside `AudioProc`; it must be corrected without modifying upstream. The approved direction is scoped synchronous/caller-owned remote audio plus a coherent timestamped timing publication, preserving the existing protected clock and phase distinctions.
