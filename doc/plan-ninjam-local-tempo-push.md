# Feature Request: Push Local Tempo on Join and Confirm Incoming Remote Tempo Changes

## Objective
Provide a controlled NINJAM tempo workflow with three properties:

1. When joining a server, Jamma can prefer the current local tempo and push it outward instead of being immediately overridden by the remote interval.
2. When the remote server later changes tempo, Jamma asks the user before applying that change locally.
3. Every relevant tempo decision is logged to the console with enough detail to understand or manually overrule the outcome via NINJAM chat/admin commands.

## Verified Current Behavior

The current flow is:

1. `Scene::OnJobTick()` calls `_QueueLocalTempoFromClock()` every tick.
2. If a NINJAM snapshot is present, the same tick calls `_SendQueuedTempoAtIntervalWrap(snapshot)` and then `_ApplyRemoteTempoToClock(snapshot)`.
3. `TimingQuantiser::ApplyRemoteTempo(...)` exits early only when `_armReclock == true` or `_hasPendingTempo == true`.
4. `TimingQuantiser::SendQueuedTempo(...)` currently sends only on remote interval wrap (`snapshot.IntervalPositionSamps < _lastRemoteIntervalPos`).

That means a newly connected client with no pending local tempo immediately adopts the remote interval, because the first remote timing snapshot is applied before any join-specific local override is armed.

## Design Corrections to the Original Plan

The original idea is close, but a few pieces need tightening:

1. Using `_hasPendingTempo` to block `ApplyRemoteTempo(...)` is correct.
2. Using `_lastRemoteIntervalPos == 0` as an implicit "send immediately" heuristic is too loose. Interval position zero is a legitimate runtime state, not a durable join-intent signal.
3. The remote-tempo confirmation dialog must not be a blocking native dialog invoked from the job/network path. Tempo snapshots arrive via `OnJobTick()`, while scene popups are managed on the main/UI side through `Scene::_popupHost` and `CommitChanges()`.
4. If the user clicks `Cancel` on a remote tempo prompt, the app must suppress repeated prompts for that exact same remote tempo until the server tempo actually changes again or the session disconnects.

## Proposed Implementation Plan

### 1. Add Explicit Tempo-Join Options

Do not add a single naked bool if this feature is already branching into multiple user-facing behaviors. Use a tiny options struct owned by `Scene` and passed through connect:

```cpp
struct NinjamTempoJoinOptions
{
    bool PushLocalTempoOnJoin = false;
    bool PromptBeforeApplyingRemoteTempo = true;
};
```

Recommended ownership:

1. `Scene` owns the current connect-time options because the scene already owns `_popupHost`, the local quantiser, and the user interaction flow.
2. `NinjamNetworkService` and `NinjamController` remain transport-oriented; they should not own UI prompt policy.

The existing `Scene::ConnectNinjam(const std::string& host)` can become either:

```cpp
void ConnectNinjam(const std::string& host,
    const NinjamTempoJoinOptions& options);
```

or:

```cpp
void ConnectNinjam(const std::string& host, bool pushLocalTempo);
```

The struct form is preferable because the remote-tempo confirmation policy belongs to the same feature area.

### 2. Arm a Local Tempo Push Before Connecting

The existing `_hasPendingTempo` gate is the correct local protection point, but the join path needs an explicit helper that marks the current local tempo as pending even when the tempo has not just changed this tick.

Add a `TimingQuantiser` helper along these lines:

```cpp
void TimingQuantiser::ForceQueueCurrentTempoAsPending(bool sendImmediately)
{
    if (_masterLoopLengthSamps.load(std::memory_order_acquire) == 0ul)
        return;
    if (_effectiveQuantiseSamps.load(std::memory_order_acquire) == 0u)
        return;

    _hasPendingTempo.store(true, std::memory_order_release);
    _armReclock.store(false, std::memory_order_release);
    _sendPendingTempoImmediately.store(sendImmediately, std::memory_order_release);
}
```

Notes:

1. This should not mutate the local clock or recompute tempo. It only marks already-known local timing as the outgoing authority.
2. `sendImmediately` should be a dedicated one-shot state, not inferred from `_lastRemoteIntervalPos`.
3. Reset this one-shot state in `TimingQuantiser::Clear(...)`, on disconnect, and after the first successful immediate send attempt.

Connect flow:

```cpp
if (options.PushLocalTempoOnJoin)
    _quantisation.ForceQueueCurrentTempoAsPending(true);

_networkService->Connect(host);
```

If no valid local tempo exists, the helper simply no-ops and the join behaves as today.

### 3. Send Join-Time Local Tempo on the First Valid Snapshot

Current `SendQueuedTempo(...)` waits for remote interval wrap. That is fine for ordinary local edits during an established session, but it is too slow and too ambiguous for join-time push behavior.

Add explicit join-push behavior:

1. Preserve existing wrap-based send behavior for normal local tempo changes.
2. If `_sendPendingTempoImmediately == true`, allow `SendQueuedTempo(...)` to send as soon as all of the following are true:
   - `_hasPendingTempo == true`
   - connected session exists
   - remote snapshot has timing/sample-rate information sufficient to derive BPM/BPI from the local grain/master values
3. After this immediate send attempt succeeds, clear `_sendPendingTempoImmediately`.
4. If the immediate send cannot run yet because the snapshot is incomplete, leave the flag armed and retry on the next snapshot.

This removes the need for a `_lastRemoteIntervalPos == 0` heuristic and makes the behavior deterministic.

### 4. Preserve Fallback When the Server Refuses the Tempo Change

The fallback logic from the original plan is correct and should be kept.

Flow:

1. Local join push marks `_hasPendingTempo = true`.
2. Remote tempo application is skipped while that flag is true.
3. `RequestServerTempo(...)` sends both admin and vote-style commands.
4. On successful send, `_hasPendingTempo` becomes false.
5. If the server accepts the change, later snapshots match the requested tempo and the local session remains aligned.
6. If the server does not accept the change, later snapshots still report the original remote tempo; once `_hasPendingTempo` is false, local policy should hand off to the confirmation flow described below instead of silently forcing the remote tempo.

### 5. Add a Non-Blocking Remote Tempo Confirmation Flow

This is the major new feature change.

#### 5a. Do Not Prompt from `TimingQuantiser::ApplyRemoteTempo(...)`

Do not call `MessageBox(...)` or any blocking modal directly from `ApplyRemoteTempo(...)` or from the job thread.

Reason:

1. Remote snapshots arrive through `Scene::OnJobTick()`.
2. UI popups already live at the scene layer via `Scene::_popupHost`.
3. `CommitChanges()` already consumes pending NINJAM snapshots on the scene/UI side and is the natural place to open or refresh a popup.

#### 5b. Split "propose remote tempo" from "apply remote tempo"

Refactor the current apply path into two stages:

1. A pure or near-pure helper that evaluates the snapshot and produces a proposal object if a meaningful remote tempo change exists.
2. A second helper that takes an accepted proposal and mutates the clock/quantiser exactly as current `ApplyRemoteTempo(...)` does.

Suggested proposal shape:

```cpp
struct PendingRemoteTempoChange
{
    unsigned int IntervalLengthSamps = 0u;
    unsigned int SampleRate = 0u;
    unsigned int GrainSamps = 0u;
    unsigned long MasterLoopLengthSamps = 0ul;
    float Bpm = 0.0f;
    unsigned int Bpi = 0u;
    unsigned int IntervalPositionSamps = 0u;
};
```

The proposal should be created only when:

1. `snapshot.HasTiming` is true.
2. `_armReclock` is false.
3. `_hasPendingTempo` is false.
4. The proposed remote tempo actually differs from the last applied/known remote tempo.
5. Timing derivation succeeds and yields a non-zero grain.

#### 5c. Scene owns the prompt lifecycle

Store prompt state in `Scene`, not `TimingQuantiser`, because the scene owns UI.

Suggested scene state:

```cpp
std::optional<PendingRemoteTempoChange> _pendingRemoteTempoPrompt;
std::optional<PendingRemoteTempoChange> _ignoredRemoteTempoPrompt;
bool _remoteTempoDialogOpen = false;
```

Behavior:

1. `OnJobTick()` detects or receives a remote tempo proposal and stores it under scene state.
2. `CommitChanges()` notices a pending proposal and opens a popup-hosted modal with:
   - label text describing the incoming tempo
   - `Yes` button
   - `Cancel` button
3. `Yes` applies the proposal immediately on the scene side.
4. `Cancel` leaves the local tempo unchanged and records the proposal as ignored.
5. While the remote server keeps advertising the same tempo, do not reopen the prompt.
6. If the remote tempo changes again, clear the ignored marker and allow a new prompt.
7. Clear all prompt/ignore state on disconnect.

This avoids an infinite nag loop while still preserving user control.

#### 5d. Modal content

The dialog label should include at least:

1. BPM
2. BPI
3. grain
4. master loop length / interval length

Example label:

```text
Remote NINJAM tempo changed to 120.0 BPM, 16 BPI
Master loop: 352800 samples
Grain: 22050 samples
Apply this tempo locally?
```

### 6. Add Console Logging for Every Tempo Decision

Add one shared logging helper so all tempo-related console output has the same shape.

Minimum fields requested:

1. master loop length
2. grain
3. BPI
4. BPM

Recommended additional field:

5. sample rate

Log at these moments:

1. Local tempo armed for join push.
2. Local tempo actually sent to NINJAM.
3. Remote tempo proposal detected.
4. Remote tempo accepted and applied locally.
5. Remote tempo ignored/cancelled by user.
6. Join push fell back because the server stayed on another tempo.

Recommended message shapes:

```text
[NINJAM] Local tempo queued for join push: master=352800 grain=22050 bpi=16 bpm=120.0 sr=44100
[NINJAM] Local tempo sent to server: master=352800 grain=22050 bpi=16 bpm=120.0 sr=44100
[NINJAM] Remote tempo proposed: master=352800 grain=22050 bpi=16 bpm=120.0 sr=44100
[NINJAM] Remote tempo applied locally: master=352800 grain=22050 bpi=16 bpm=120.0 sr=44100
[NINJAM] Remote tempo ignored by user: master=352800 grain=22050 bpi=16 bpm=120.0 sr=44100
```

To support manual override via NINJAM chat/admin commands, also log the exact command forms that correspond to the local tempo whenever a local push is queued or ignored remote tempo leaves the user wanting to overrule the server:

```text
[NINJAM] Manual server tempo commands: /bpm 120 /bpi 16 | !vote bpm 120 | !vote bpi 16
```

That gives the user enough information to manually push or campaign for the tempo through chat if they intentionally keep ignoring incoming remote changes.

### 7. Do Not Auto-Push Based on `Users.empty()` in the First Implementation

The original "solo server" heuristic is plausible but should not be part of the first implementation.

Reasons:

1. User list timing may lag initial timing snapshots.
2. The local user can appear in NINJAM enumeration, which complicates occupancy semantics.
3. The explicit connect option already solves the main problem with less policy magic.

If this heuristic is added later, it should be a separate follow-up built on top of the explicit connect option, not a hidden replacement for it.

## Suggested Implementation Sequence

1. Add join option plumbing in `Scene` connect flow.
2. Add `TimingQuantiser::ForceQueueCurrentTempoAsPending(...)` plus one-shot immediate-send state.
3. Update `SendQueuedTempo(...)` to support deterministic first-snapshot send for join push.
4. Split remote-tempo detection from remote-tempo application.
5. Add scene-owned pending/ignored remote tempo prompt state.
6. Implement popup-hosted modal UI with label + `Yes` and `Cancel` buttons.
7. Add shared tempo logging helper and wire it into all decision points.
8. Clear pending prompt/ignore/join-push state on disconnect.

## Validation Checklist

Implementation should be considered complete only after these cases work:

1. Join with `PushLocalTempoOnJoin = false`: current behavior remains unchanged.
2. Join with `PushLocalTempoOnJoin = true` and valid local tempo: first valid snapshot does not immediately overwrite local tempo.
3. Join with `PushLocalTempoOnJoin = true` and no valid local tempo: connection falls back to normal remote adoption/prompt flow.
4. Server accepts pushed local tempo: later snapshots match, no remote prompt appears.
5. Server rejects or outvotes pushed local tempo: user sees the incoming remote tempo prompt instead of silent forced adoption.
6. User clicks `Yes`: local quantiser/clock updates exactly once.
7. User clicks `Cancel`: local tempo remains unchanged and the same remote tempo does not prompt again every tick.
8. Remote tempo changes again after a cancel: a fresh prompt appears.
9. Disconnect/reconnect: old prompt/ignore/immediate-send state does not leak into the new session.

## Summary

The core strategy is still sound: use `_hasPendingTempo` to shield local tempo during a join-time push. The two critical fixes are:

1. make the immediate join push explicit with its own one-shot flag instead of inferring it from interval position, and
2. handle incoming remote tempo changes as scene-owned popup proposals with ignore-state dedupe, not as unconditional direct applies or blocking job-thread dialogs.

With those corrections, the plan is internally consistent and ready for implementation.