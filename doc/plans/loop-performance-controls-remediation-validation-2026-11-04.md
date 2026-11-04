# Loop performance controls remediation validation

Date: 2026-11-04. Status: required remediation complete and validated; optional R-004 deferred. Candidate branch: `feature/loop-performance-controls`.

Remediation scope: repairs over `853b3be801efd1fa5a37cb32db46345432beb020`. Intended target and merge base: `feature/alpha-tweaks`, `53aa25512a3ee5b4c6b01982c29a6894cef4db43`. The target has no intervening drift. No merge was performed.

## Implemented

- R-001: `LoopTake::WriteBlock` uses the accepted audio material and pinned local chain to skip sample clearing, peak scanning and output mixing for MIDI-only takes without local DSP. Output mixers, bus width, gain and export configuration remain prepared. Local racks still run for generators and tails, and audio leaves render before the fast-path decision. Station MIDI delivery and playback advancement remain independent.
- R-002: immediate live admission consults the accepted whole-hierarchy snapshot, including Take and Loop destinations. Irrelevant attacks/controllers are rejected before enqueue, while note-offs and sustain-up remain admitted. Each nonrelease entry carries its observed graph generation, so an older in-flight callback leaves it queued for a matching or newer publication. Dispatch retains its 64-event block budget and fixed 1024-slot ingress storage; events now include fixed generation metadata.
- R-003: Take master mute compares the actual mixer raw flag before changing it, avoiding unchanged atomic RMWs without a cache that could become stale.
- Verbose ingress rejection now says `rejected` rather than misreporting every filtered event as a full queue.
- Corrected one stale GUI integration assertion: opening a Take MIDI grid already collapses its Station sibling under candidate HEAD's exclusive-grid behavior. Added immediate setup assertions; retained the active-grid, selection, capture and routing checks. No production GUI behavior changed.

R-004 remains the explicit optional follow-up allowed by the remediation plan: ordinary mute image adoption still visits the complete bounded hierarchy. This repair does not introduce deltas, spread adoption across blocks, or change simultaneous mute semantics. Its large-session deadline cost remains unmeasured.

## Thread ownership and callback review

| State | Writer / reader | Publication and lifecycle |
| --- | --- | --- |
| Accepted material, chain and live destinations | Serialized structural owner / audio and live-ingress readers | Existing immutable `PublishedAudioSnapshot` leases; no mutable UI topology traversal or ownership destruction on audio |
| Prepared MIDI generation | Structural owner / ingress and audio | Immutable generation in each accepted graph; only successfully admitted topology reaches publication |
| Fallback ingress generation | Structural owner / ingress | Atomic release store before graph exchange; acquire load only when producer borrow fails. Monotonic across reset, never advanced by rejected preparation |
| Immediate MIDI queue entries | Existing single live-dispatch producer / audio consumer | Existing fixed SPSC queue, acquire/release handoff. Failed snapshot reads conservatively admit with the generation floor; releases use generation zero |
| Master mute and fade | Existing raw atomic controls / audio-owned fade | Compare actual raw flag before update; no new mutable cached state |

The fallback generation prevents bounded snapshot-read failure from bypassing the newer-graph wait. No lease pointer escapes its scope. The queue wait is a generation eligibility check, not a spin or blocking wait. Synthetic source-loss/recording release queues and physical history/recording observation remain independent. No new callback allocation, deallocation, lock, wait, I/O, exception path or unbounded traversal was found. Existing snapshot reclamation and drained-reader teardown remain unchanged.

Independent source review by a dedicated Sol reviewer found no remaining actionable production findings after the fallback-generation fix. It also checked both normal/failing-borrow test paths and the stale GUI assertion against existing exclusive-grid tests.

## Validation

- Incremental Debug x64 native test project build: passed, including JammaLib.
- Incremental Debug x64 Jamma app build: passed.
- Focused `StationMidiInstrument.*`: 89 passed before the final coverage-only extension; the final full run includes both normal and failed snapshot-borrow cases.
- Final full native suite: **2218 passed**, 236 suites, exit 0 (135.488 seconds). Includes the strengthened transition regression and corrected exclusive-grid integration assertion.
- Combined target-to-candidate static checker and whitespace check: passed, no policy violations. The full scope and content fingerprint are retained in the [machine-local static review JSON](C:/Users/matto/.codex/visualizations/2026/11/04/01a1a64f-6fb8-75a3-8b72-153d5dd23ec8/jamma-remediation/static-review.json).

Build commands used the executable from local `.vscode/tasks.json`, with the process-only Path/PATH preflight from `doc/build.md` and an absolute `SolutionDir` ending in one backslash:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' test\JammaLib_Tests\JammaLib_Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=<absolute-worktree-root>\
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=<absolute-worktree-root>\
& test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe
python .agents/skills/jamma-review/scripts/review_changes.py --base 53aa25512a3ee5b4c6b01982c29a6894cef4db43 --head HEAD --format json
```

Regressions cover 2048 irrelevant controllers before accepted notes/releases; independent child routes; normal and failed producer borrows against an older callback graph; superseding publications; no Take output-mixer calls without local DSP; continued Station recorded playback; local generator activation and rack-removal fast-path restoration. Existing tests cover tails, mute fades, retained Station mute, mixed material, leaf instruments, recorded latency and callback allocation probes.

An initial link failure was caused by editing an ingress signature during an earlier build; the affected source was explicitly recompiled and final builds passed. Initial test failures were corrected by publishing the child test configuration, using stable unload identity, updating intended ingress rejection expectations, and fixing the preexisting exclusive-grid assertion.

No application GUI session, hardware audio deadline measurement, benchmark, Release build or race-detector run was performed. Static review and hermetic native tests support the scoped changes; they do not prove universal race freedom, deadline headroom or third-party plugin behavior.
