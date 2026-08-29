# Stage 04 — Naming and domain vocabulary

## Assignment

- Stage: Phase 1, Stage 4 — Naming and domain vocabulary.
- Primary ownership: whether changed names communicate coordinate system, authority, lifetime, thread ownership, and the protected timing glossary.
- Explicit exclusions: general formatting/style and naming-form consistency; architectural placement; runtime correctness; dead-code reachability; and any proposal to merge or collapse protected timing concepts.
- Required inputs read in full: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/merge-readiness-review/stage-reports/01-diff-inventory.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Comparison: `master...HEAD`; relevant history: `master..HEAD`.
- Output/single-writer boundary: this report is the only file written by Stage 4. Production code, tests, build/project files, canonical artifacts, inventory, and every other stage report were not edited.

## Coverage

The stable Stage 1 inventory was consumed rather than regenerated. I scanned the actual `master...HEAD` additions and modifications across the 61 changed headers, then followed semantically relevant declarations through current implementations, call sites, tests, design documents, and introducing/refining commits. Deep inspection covered:

- Remote observation, accepted authority, and command handoff: `NinjamTiming.h`, `NinjamTimingObservationMailbox.h`, `NinjamTimingTracker.*`, `NinjamTimingCoordinator.*`, `NinjamAudioTimingCommand.h`, `NinjamLoopAlignment.h`, `NinjamConnection.*`, `AudioHost.*`, and the relevant `Scene.cpp` call sites.
- Local transport, scene/source mapping, and per-entity phase: `Timer.*`, `MusicalTransport.*`, `Loop.h`, `LoopTake.*`, and `Station.*`.
- MIDI timestamp, event-cursor, and automation-anchor terminology: `MidiClockAnchor.h`, `MidiBlockTiming.h`, `MidiLoop.*`, `MidiRouter.*`, `IVstPlugin.h`, and the automation dispatch in `Station.*`.
- Other changed NINJAM timing vocabulary with public value types: `ExportLaneTiming.*` and `NinjamMetronomeTiming.h`.
- Relevant history: `f36bfe7`, `d6fdb88`, `b41b5a8`, `bef7943`, `e72f3b0`, `46c7757`, `782b8a8`, and `3db18e9`.

Commands/queries used:

- `git diff --name-only|--unified=0 master...HEAD -- <timing/audio/engine/midi/ninjam/utils/vst paths>` to identify changed semantic surfaces and actual added identifiers.
- `rg -n` over changed headers and surrounding implementations for phase, position, sample, coordinate, anchor, source, remote/local authority, generation, mailbox, owner, thread, and lifetime names; every candidate below was then read in surrounding code rather than accepted from a textual hit.
- `git log --oneline master..HEAD -- <paths>`, `git log -S<identifier>`, `git show --unified=<n> <commit> -- <paths>`, and `git blame -L` to establish when a name entered and whether later commits changed its meaning.
- Line-numbered `Get-Content` reads for current declarations and consumers.

Deliberate exclusions were general casing/style, architecture, runtime behavior, synchronization validity, performance, test quality, and reachability. No build or test was run because this stage changes no executable artifact and makes no runtime assertion.

## System understanding

The branch deliberately carries several different rulers. NJClient first reports a wrapped remote interval phase in its source sample rate. `ToDeviceTiming` converts that geometry to the device rate and preserves two observation anchors: Timer absolute position for observation-age correction and the monotonically increasing device audio-frame counter for applying a remote phase at a later audio boundary. The accepted replacement then crosses `Scene` in a `NinjamAudioTimingCommand`; `AudioHost` applies remote master geometry to `Timer`, maps the correction onto the retained local-source ruler, and fans the mapped amount into each take. The `SyncPhaseMap` and per-entity scene anchors preserve entity-specific audio and MIDI phase. Separately, `MidiClockAnchor` maps wall-clock MIDI arrival onto the device audio-frame counter, while `MidiLoop::LoopPhaseAnchor` is used only as the frozen global-sample origin for automation playback, not as the MIDI event cursor.

Most recent source-coordinate names correctly preserve these distinctions. In particular, `SceneSamplePos`, `BodyPlayIndex`, `SourceCoordinateAtOrigin`, `LocalFollowPolicy`, and the three protected policy enumerators should not be collapsed. The candidate names below target places where the current words conceal one of those distinctions.

## Candidate findings

Required rename handoff, ranked by value and then implementation risk:

| Candidate | Old/current name | Proposed name | Semantic defect | Protected distinction retained |
| --- | --- | --- | --- | --- |
| S04-01 | `NinjamTempoChange::GrainSamps` | `RemoteGridStepSamps` | The value is the remote interval divided by authoritative remote BPI, not the exact local audio construction grain. | Local grain vs active/remote quantisation grid. |
| S04-01 | `NinjamClockSettings::QuantiseSamps` and `NinjamAudioTimingCommand::QuantiseSamps` | `RemoteGridStepSamps` (or `ReplacementGridStepSamps` in the audio command) | The value is derived from accepted remote grid geometry but is named as an unqualified Timer quantisation value. | Remote authority/grid vs local grain and local timing. |
| S04-01 | UI label `Grain:` for `change.GrainSamps` | `Remote grid step:` | User-facing vocabulary explicitly calls a remote beat/grid cell a grain. | Local grain remains reserved for local audio construction. |
| S04-02 | `NinjamClockSettings` / `NinjamTimingUpdate::ClockSettings` | `AcceptedRemoteTimingReplacement` / `AcceptedRemoteReplacement` | This is an accepted remote-authority replacement plus follow policy, not a clock owner or a generic bag of clock settings. | Remote authority and follow policy remain distinct from a clock/coordinate. |
| S04-02 | `NinjamClockSettings::SeedLengthSamps`, `NinjamAudioTimingCommand::SeedLengthSamps` | `RemoteMasterIntervalLengthSamps` / `ReplacementRemoteMasterLengthSamps` | “Seed” is inherited local Timer vocabulary and hides that the accepted replacement length is the remote master ruler. | Remote master length vs old local master/source length. |
| S04-02 | `NinjamClockSettings::PhaseSamps`, `NinjamAudioTimingCommand::AbsolutePhaseSamps` | `ObservedRemoteMasterPhaseSamps` | A wrapped interval phase is neither unqualified nor absolute; its remote-master authority and observation semantics are absent. | Remote master phase vs Timer absolute position, scene coordinate, and per-loop phase. |
| S04-02 | `NinjamAudioTimingCommand::PhaseObservationSample` | `RemotePhaseObservationDeviceSample` | The field is a monotonically increasing device audio-frame position used to age the observed remote phase, not another phase/sample value. | Device audio counter vs remote wrapped phase and monotonic scene coordinate. |
| S04-02 | `NinjamAudioTimingCommand::PhaseDeltaSamps` | `RemoteMasterPhaseCorrectionSamps` | The command carries the remote-master correction; `AudioHost` later derives a different local-source correction for stations. | Remote master correction vs mapped local-source/entity correction. |
| S04-02 | local `AudioHost::stationDelta` | `localSourceCorrectionSamps` after a separate `remoteMasterCorrectionSamps` input | One variable begins as the command's remote correction and is overwritten with mapped local-source movement; “station” identifies a recipient, not a coordinate. | Common mapped elapsed/correction vs remote master phase and per-loop cursor. |
| S04-03 | `NinjamTiming::LocalBlockStartSample`, `NinjamTimingObservation::LocalSample` | `LocalMasterAbsoluteSampleAtObservation` | The value is `Timer::AbsoluteSamplePos(...)`, not the device block-start counter and not a generic local sample. | Timer absolute master position vs device audio-frame counter and scene coordinate. |
| S04-03 | `NinjamTiming::AudioBlockStartSample` and downstream copies | `DeviceAudioSampleAtObservation` | The value is the device audio-frame counter at the remote observation; the current pair “LocalBlock”/“AudioBlock” does not expose why the two differ. | Device-rate observation age vs Timer absolute master position. |
| S04-03 | `MidiClockAnchorSnapshot::Sample`, `MidiClockAnchor::Sample` | `DeviceAudioSamplePosition` | This anchor is paired with `SteadyMicros` to map MIDI arrival onto the device audio-frame counter; `Sample` leaves its coordinate and authority unstated. | Wall-clock timestamp vs device audio sample position. |
| S04-04 | `MidiLoop::LoopPhaseAnchor`, `_loopPhaseAnchor`, dispatch `loopPhaseAnchor` | `AutomationGlobalSampleOrigin`, `_automationGlobalSampleOrigin`, `automationGlobalSampleOrigin` | The stored value is `startGlobalSample`, frozen for automation fraction conversion. It is not the MIDI event cursor and is not itself a wrapped per-loop phase. | Automation-recording anchor vs MIDI event cursor, scene/source anchor, and per-loop phase. |
| S04-05 | `ExportLaneTimingInput::n` | `delayWriteCursorSamps` | `n` is the running pre-write delay-line cursor; the public field does not state its coordinate or lifetime within the block. | Local delay-line cursor vs remote interval phase. |
| S04-05 | `ExportLaneTimingInput::pos` / `length` | `remoteIntervalPhaseSamps` / `remoteIntervalLengthSamps` | NJClient's wrapped remote phase and interval length are exposed under generic algebraic names. | Remote wrapped phase/length vs delay-line cursor and block frame count. |
| S04-05 | state `lastLength` / `predictedPos` | `lastRemoteIntervalLengthSamps` / `predictedRemoteIntervalPhaseSamps` | Persisted prior-observation state is indistinguishable by name from local buffer geometry. | Remote observation generation vs local delay-line state. |

### S04-01 — Remote grid cells are named and displayed as local grains

- Stage / reviewer: Stage 4 — Naming and domain vocabulary.
- Scope reviewed / exclusions: accepted remote tempo/grid vocabulary through coordinator, Scene prompt, command, Timer boundary, and design glossary; no judgment of the still-pending active-grid migration's runtime correctness.
- Severity: must fix before merge.
- Evidence: `NinjamTempoChange::GrainSamps` is declared at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:56`; the current authoritative-BPI path calculates it as rounded `remote interval / remote BPI` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:306`–`313`; `_AcceptTempoChange` copies it into `NinjamClockSettings::QuantiseSamps` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:316`–`327`; and the prompt calls it `Grain` at `JammaLib/src/engine/Scene.cpp:378`–`382`. Commit `f36bfe7` introduced the fields and `e72f3b0` made the server BPI authoritative while retaining the grain name.
- Why it matters: the protected glossary says local grain is an exact local audio construction unit and is not a remote beat or necessarily the active quantisation step. The current code and UI assign the protected local term to the remote grid step, obscuring the migration boundary and inviting future code to treat remote authority as local loop-construction geometry.
- Recommended disposition: rename the remote-derived fields and UI label to `RemoteGridStepSamps` / “Remote grid step”; retain `GrainSamps` only for actual local audio geometry.
- Protected timing concepts affected: local grain; active quantisation grid; remote timing; local timing.
- Verification: symbol/search audit shows every remote-BPI-derived consumer uses remote-grid vocabulary and every remaining `GrainSamps` denotes local audio construction; prompt wording is manually checked.
- Human decision: pending.

### S04-02 — The accepted remote timing command hides authority and overloads phase coordinates

- Stage / reviewer: Stage 4 — Naming and domain vocabulary.
- Scope reviewed / exclusions: coordinator-to-Scene-to-audio command vocabulary and surrounding conversion code; no assertion about the mathematical correctness of replacement or discipline.
- Severity: must fix before merge.
- Evidence: the accepted packet is generically named `NinjamClockSettings` and exposes `SeedLengthSamps`/`PhaseSamps` at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:69`–`84`; `_AcceptTempoChange` constructs it only after selecting a follow policy for an accepted remote change at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:316`–`332`. Scene transfers its wrapped remote interval position into a field named `AbsolutePhaseSamps` and its device observation counter into `PhaseObservationSample` at `JammaLib/src/engine/Scene.cpp:432`–`449`. The command declarations are at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:35`–`50`. At the audio boundary, those fields are passed as observed remote phase plus device observation/boundary samples at `JammaLib/src/audio/AudioHost.cpp:224`–`244`, while the initial `stationDelta` is overwritten with a mapped source-ruler delta at `JammaLib/src/audio/AudioHost.cpp:169` and `241`–`247`. Commit `d6fdb88` introduced the unified command and the misleading `AbsolutePhaseSamps` name; `b41b5a8` added the observation-time field; `bef7943` later made the downstream source coordinate monotonic without revisiting this command vocabulary.
- Why it matters: this boundary is where remote authority, remote master phase, device observation age, local-source mapping, and per-entity restoration deliberately meet. Generic “clock,” “seed,” and “phase” names—and especially “absolute” for a wrapped phase—make it easy to pass the correct numeric type in the wrong coordinate or mistake follow policy for clock ownership.
- Recommended disposition: apply the S04-02 rename group together. Keep separate remote-master input and mapped local-source variables in `AudioHost`; do not rename them to a single shared “phase” or “cursor.”
- Protected timing concepts affected: remote timing; follow policy; master phase; sync phase map; mapped elapsed/local-source correction; per-loop phase.
- Verification: focused command tests and integration simulations are updated only for names, then reviewers trace one replacement and one phase-discipline command to confirm that remote master phase, device observation sample, scene coordinate, and mapped local-source correction remain distinct.
- Human decision: pending.

### S04-03 — Observation anchors do not name their two incompatible sample coordinates

- Stage / reviewer: Stage 4 — Naming and domain vocabulary.
- Scope reviewed / exclusions: observation timestamp/anchor flow and MIDI timestamp mapping; no judgment of seqlock correctness or observation-age arithmetic.
- Severity: must fix before merge.
- Evidence: canonical timing declares adjacent `LocalBlockStartSample` and `AudioBlockStartSample` fields at `JammaLib/src/ninjam/NinjamTiming.h:21`–`37`. `AudioHost` proves the first is `Timer::AbsoluteSamplePos(blockStartSample)` while the second is the raw audio counter at `JammaLib/src/audio/AudioHost.cpp:444`–`452`. The coordinator then renames the first again to generic `LocalSample` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:99`–`105` and uses it in the Timer absolute domain at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:219`–`235`. Separately, `MidiClockAnchor` calls the same device audio-frame coordinate merely `Sample` at `JammaLib/src/midi/MidiClockAnchor.h:8`–`28`; it is published from the device counter at `JammaLib/src/audio/AudioHost.cpp:545`–`549` and consumed to create absolute MIDI event timestamps at `JammaLib/src/midi/MidiRouter.cpp:473`–`483`. `d0d208e` introduced `LocalBlockStartSample`, `b41b5a8` introduced the second audio counter, and `3db18e9` introduced the MIDI clock anchor.
- Why it matters: both values are local and audio-related, but Timer absolute geometry can reset/change under accepted timing while the device frame counter is monotonic. Names that distinguish only “Local” from “Audio” do not protect callers from substituting one for the other or confusing either with `SceneSamplePos`.
- Recommended disposition: consistently name the Timer value `LocalMasterAbsoluteSampleAtObservation` and the device value `DeviceAudioSampleAtObservation`; name the MIDI anchor's field `DeviceAudioSamplePosition`.
- Protected timing concepts affected: local master transport/absolute position; monotonic scene coordinate; device-rate observation timing; remote timing.
- Verification: compile-time rename plus tests preserving distinct sentinel values for both coordinates; search audit rejects unqualified `LocalSample`/`Sample` at these cross-thread timing boundaries.
- Human decision: pending.

### S04-04 — `LoopPhaseAnchor` names an automation global-sample origin as per-loop phase

- Stage / reviewer: Stage 4 — Naming and domain vocabulary.
- Scope reviewed / exclusions: MIDI automation anchor naming and its current producers/consumers; no claim about automation playback correctness or lifetime safety.
- Severity: must fix before merge.
- Evidence: the declaration says the value is the “Global sample that maps to loop-relative position 0” at `JammaLib/src/midi/MidiLoop.h:175`–`183`; `EndRecord` stores its `startGlobalSample` parameter directly at `JammaLib/src/midi/MidiLoop.cpp:253`–`259`; and Station freezes it into automation dispatch at `JammaLib/src/engine/Station.h:334`–`344`. The protected design explicitly says this automation-recording anchor is not the MIDI event cursor (`doc/loop-alignment-and-ninjam-sync.md:48`–`52`). Commit `46c7757` introduced `LoopPhaseAnchor` specifically to correct automation fraction calculation, not loop event playback.
- Why it matters: the name reads as a wrapped per-loop phase anchor and competes with the branch's actual scene/source anchors and MIDI event cursor. That ambiguity is especially risky where automation correction is deliberately adjusted opposite to event-cursor translation.
- Recommended disposition: rename the accessor/member/dispatch copy to `AutomationGlobalSampleOrigin` (with conventional member casing applied by Stage 3 as appropriate); retain the existing separate MIDI event cursor and scene-anchor names.
- Protected timing concepts affected: MIDI automation-recording anchor; MIDI event cursor/per-loop phase; source/scene anchor.
- Verification: compile-time rename and an automation test/search audit showing that the renamed value is only used in global-sample-to-automation-fraction calculations; timing integration tests continue to treat `_midiVisualPlayIndex` as the MIDI event cursor.
- Human decision: pending.

### S04-05 — Export-lane timing exposes remote and local cursors as `n`, `pos`, and `length`

- Stage / reviewer: Stage 4 — Naming and domain vocabulary.
- Scope reviewed / exclusions: value-type vocabulary and formulas in the pure export timing helper; no claim about latency formula correctness or whether dormant compensation should ship.
- Severity: follow-up.
- Evidence: the public input names are declared at `JammaLib/src/ninjam/ExportLaneTiming.h:12`–`25`; only the comment reveals that `n` is a pre-write delay-line cursor and `pos`/`length` follow NJClient's remote interval convention. The implementation mixes them in modular delay calculations at `JammaLib/src/ninjam/ExportLaneTiming.cpp:20`–`66`, and the current producer copies `_exportTick`, NJClient position, and NJClient interval length at `JammaLib/src/ninjam/NinjamConnection.cpp:592`–`596`. Commit `782b8a8` introduced the helper and these names.
- Why it matters: the formula combines a local delay-line cursor, wrapped remote interval phase, remote interval length, device block size, and device latencies. Algebraic single-letter fields make coordinate substitution hard to see at call sites and make the retained state's lifetime across remote observations opaque.
- Recommended disposition: apply the S04-05 rename group if the helper remains in merge scope; keep the formula comment but make every public field self-identifying.
- Protected timing concepts affected: remote timing/phase; local device-rate delay-line timing. None are merged.
- Verification: compile-time rename plus existing pure-helper tests; review call-site initialization without relying on positional aggregate order.
- Human decision: pending.

## Handoffs

- Stage 2 / integrator: S04-02 establishes a vocabulary defect only. Whether accepted remote replacement, follow-policy selection, and Timer command translation are placed in the correct owners remains logical-layout ownership.
- Stage 3: apply neighboring naming form/casing separately. This report's proposed semantic words do not judge existing PascalCase/camelCase consistency.
- Stage 5: the relevant direction changes are already commit-backed above, especially `bef7943` changing phase-based restoration to source coordinates and `e72f3b0` making remote BPI authoritative. Stage 4 does not classify historical residue.
- Stage 6: `SyncPhaseMap::SourcePhaseAtOrigin` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:54` and `NinjamTimingObservation::LocalSample` should be checked for current reachability/redundancy. This report does not develop a dead-code finding.
- Stage 9: determine whether the overloaded `Timer::Command::PhaseDeltaSamps` contract—absolute replacement phase for `ReplaceTiming`, signed delta otherwise, as visible at `JammaLib/src/utils/Timer.cpp:217`–`235`—creates a correctness hazard. Stage 4 records the semantic overload but makes no runtime claim.
- Stage 15: if S04-01 is accepted, align prose and UI vocabulary so “grain” remains local construction geometry and “remote grid step” remains remote timing.
- Integrator: keep S04-01 separate from S04-02 during deduplication. S04-01 is the protected local-grain/remote-grid collision; S04-02 is authority and coordinate naming at the accepted-command boundary.

## Uncertainties

- The kickoff glossary is treated as the protected review constraint, but it remains subject to the Phase 1 human gate. If humans change a term, the rename table must be reconciled without collapsing the underlying distinctions.
- Most affected headers are source-tree cross-subsystem interfaces rather than an installed SDK. Stage 1 found no repository packaging contract; rename compatibility cost is therefore compile-time repository churn plus any unstated external consumers.
- `Timer`'s legacy “seed source” terminology predates much of this branch. I proposed renaming only newly added cross-boundary fields that import that ambiguity; a wholesale Timer vocabulary migration would exceed this stage and branch scope.
- Export latency compensation is currently disabled by `ExportLatencyCompensationEnabled` at `JammaLib/src/ninjam/NinjamConnection.h:59`–`64`. S04-05 is conditional on retaining that helper and is not a reachability verdict.
- Thread/lifetime terminology on the reviewed mailbox, VST interface, snapshot, and raw-observer surfaces was generally explicit in adjacent contracts. I found no additional thread/lifetime naming candidate strong enough to assert without expanding into Phase 2 correctness or Stage 3 style review.

## Conclusion

Stage 4 found five bounded vocabulary candidates. Four are merge-time naming corrections because they obscure protected concepts at active cross-subsystem boundaries: remote grid step vs local grain, accepted remote authority vs clock ownership, Timer absolute observation position vs device audio counter, and automation global-sample origin vs MIDI per-loop phase. The export-lane single-letter fields are a lower-priority follow-up if that currently disabled path is retained.

The branch's core source/scene-map vocabulary is otherwise directionally sound after `bef7943` and `e72f3b0`: the report does not propose merging scene coordinates, source coordinates, master phase, entity phase, mapped elapsed time, follow policy, local timing, or remote timing. No files other than this report were written.
