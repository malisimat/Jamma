# Stage 01 — Diff and ownership inventory

## Assignment

- Stage: Phase 1, Stage 1 — Diff and ownership inventory.
- Primary ownership: objective branch scope; `master...HEAD` diff and `master..HEAD` commit statistics; subsystem partition; high-churn files; public API/interface surface; and suspected accidental scope growth.
- Explicit exclusions: architectural placement and dependency direction; naming/style and `AGENTS.md` convention judgments; code quality; runtime correctness; thread safety and real-time cost; test quality; and dead-code reachability.
- Required inputs read in full: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Comparison: `master...HEAD`; relevant history: `master..HEAD`.
- Output/single-writer boundary: this report is the only file written by Stage 1. Production code, tests, project/build files, canonical artifacts, the inventory, and other reports were not edited.

## Coverage

### Baseline regenerated

The kickoff identifiers and totals in `00-scope-and-inventory.md` were independently reproduced:

| Item | Result |
| --- | --- |
| Branch | `bugfix/align-remote-join` |
| `HEAD` | `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6` |
| Merge base with `master` | `4941b780f7ff5a46f742167d79338e3ab592a565` |
| `master` tip | `4941b780f7ff5a46f742167d79338e3ab592a565` |
| Commit range | 123 commits, including 3 merge commits |
| Diff | 199 files, 14,691 insertions, 1,507 deletions |
| Change kinds | 71 additions, 124 modifications, 4 renames |
| Kickoff worktree state | No tracked modification; supplied `doc/merge-readiness-review/` input directory untracked |

The current status was not used to reclassify any later review-artifact writes as branch content.

### Exact subsystem/file partition

This is a non-overlapping partition of all 199 `git diff --numstat master...HEAD` rows. Binary assets count as files but contribute zero textual additions/deletions. Unlike the approximate kickoff table, these rows sum exactly to the branch totals.

| Partition | Paths/routing rule | Files | Insertions | Deletions | Binary files |
| --- | --- | ---: | ---: | ---: | ---: |
| Native tests | `test/JammaLib_Tests/**` | 35 | 4,123 | 106 | 0 |
| Engine/local loop state | `JammaLib/src/engine/**`, including both `TimingQuantiser` -> `Quantiser` renames | 12 | 2,438 | 666 | 0 |
| NINJAM/session timing | `JammaLib/src/ninjam/**` | 20 | 2,268 | 107 | 0 |
| VST | `JammaLib/src/vst/**` | 12 | 1,268 | 81 | 0 |
| GUI | `JammaLib/src/gui/**`, including both popup renames | 17 | 1,245 | 46 | 0 |
| Documentation | `doc/**` | 8 | 676 | 17 | 0 |
| MIDI | `JammaLib/src/midi/**` | 11 | 603 | 51 | 0 |
| Audio/timing application | `JammaLib/src/audio/**` | 6 | 557 | 12 | 0 |
| Graphics/model/window | `JammaLib/src/graphics/**` | 10 | 390 | 51 | 0 |
| Utilities/transport | `JammaLib/src/utils/**` | 6 | 301 | 2 | 0 |
| App resources | `Jamma/resources/**` | 25 | 193 | 16 | 19 |
| Repo tooling/policy | `.github/**`, `.vscode/**`, `AGENTS.md` | 6 | 181 | 8 | 0 |
| Persistence/I/O | `JammaLib/src/io/**` | 13 | 177 | 51 | 0 |
| Remaining library/build | `JammaLib/**` not assigned above (`actions`, `base`, `include`, project files) | 9 | 117 | 16 | 0 |
| Library resources | `JammaLib/src/resources/**` | 6 | 100 | 58 | 0 |
| App shell/project | remaining `Jamma/**` (`Main.cpp`, project/filter files) | 3 | 54 | 219 | 0 |
| **Total** |  | **199** | **14,691** | **1,507** | **19** |

Stable handoff ownership map:

- App/build/tooling: `.github/**`, `.vscode/**`, `AGENTS.md`, `Jamma/Jamma.vcxproj*`, `JammaLib/JammaLib.vcxproj*`, and `test/JammaLib_Tests/JammaLib_Tests.vcxproj*`.
- Audio/timing application: `JammaLib/src/audio/AudioHost.*`, `NinjamMetronome.*`, `AudioBuffer.*`, plus the transport state in `JammaLib/src/utils/Timer.*` and `MusicalTransport.*`.
- Engine/local loop state: `Scene.*`, `Station.*`, `LoopTake.*`, `Loop.*`, `Trigger.*`, and renamed `engine/Quantiser.*`.
- NINJAM/session timing: all 20 changed `JammaLib/src/ninjam/**` paths, especially the new timing command, map/alignment, coordinator, tracker, observation mailbox, metronome timing, and export-lane timing units.
- MIDI: all 11 changed `JammaLib/src/midi/**` paths; cross-subsystem consumers also expose MIDI cursor/timing state through `LoopTake`, `Station`, and VST interfaces.
- GUI/graphics/resources: 17 GUI files, 10 graphics files, 6 library-resource files, 25 app-resource files, and their app/library project membership.
- VST: 12 `JammaLib/src/vst/**` files, with consumers in engine, MIDI, I/O persistence, and graphics/editor code.
- Persistence/I/O: 13 `JammaLib/src/io/**` files plus `Jamma/src/Main.cpp` for defaults/window persistence and session wiring.
- Tests/docs: 35 native-test files and 8 documentation files are evidence/intent inputs here; test quality remains Stage 14 ownership.

### High-churn concentration

The ten largest files by added lines contain 5,510 additions (37.5% of all branch additions); the first five contain 3,245 (22.1%).

| File | + | - | Objective handoff |
| --- | ---: | ---: | --- |
| `JammaLib/src/gui/GuiHud.cpp` | 721 | 0 | Separate HUD feature lineage; GUI/graphics/resources review surface. |
| `JammaLib/src/vst/Vst3Plugin.cpp` | 720 | 39 | Separate VST3 parity lineage; VST/interface/lifetime review surface. |
| `test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp` | 650 | 0 | End-to-end timing simulation evidence. |
| `JammaLib/src/engine/Scene.cpp` | 607 | 35 | App/engine orchestration, UI, MIDI, VST, and timing integration. |
| `test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp` | 547 | 0 | Coordinator/policy evidence. |
| `JammaLib/src/engine/Station.cpp` | 521 | 57 | Station-level audio/MIDI/timing fan-out. |
| `test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp` | 476 | 0 | Per-take audio/MIDI timing evidence. |
| `JammaLib/src/engine/LoopTake.cpp` | 452 | 40 | Per-take phase/source state and MIDI cursor. |
| `JammaLib/src/ninjam/NinjamConnection.cpp` | 428 | 45 | Remote observation, connection, lane packing, and audio export. |
| `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp` | 388 | 0 | Remote timing validity/policy/command production. |

Other high-churn seams later stages should not miss are the renamed `engine/Quantiser.cpp` (263/249), `audio/AudioHost.cpp` (332/7), `midi/MidiRouter.cpp` (282/29), `graphics/StationModel.cpp` (272/25), and `Jamma/Jamma.vcxproj` (1/209).

### Header and interface surface

The diff changes **61 header paths**: 18 additions, 41 modifications, and 2 renames. This corrects the kickoff inventory's statement of 58. Distribution is: NINJAM 12; GUI 8; MIDI 7; VST 7; engine 6; I/O 6; graphics 4; audio 3; utils 3; actions 2; and one each in base, resources, and `JammaLib/include`.

Only one changed header is in the explicit `JammaLib/include` directory: `JammaLib/include/Constants.h`, which adds NINJAM capacity/plausibility constants at `JammaLib/include/Constants.h:40` and `JammaLib/include/Constants.h:48`–`51`. The other 60 are source-tree interfaces. They are not an installed/public SDK surface in the repository layout, but many are cross-subsystem or test-consumed contracts because the projects add JammaLib source directories to their include paths (`Jamma/Jamma.vcxproj:105`, `JammaLib/JammaLib.vcxproj:103`, and `test/JammaLib_Tests/JammaLib_Tests.vcxproj:71`).

The consequential changed/new cross-subsystem interfaces are:

- Audio boundary: `AudioHost` and its timing receipt/publication/clock methods at `JammaLib/src/audio/AudioHost.h:24`–`34` and `JammaLib/src/audio/AudioHost.h:64`–`87`.
- Protected timing policy/command: `NinjamTimingCommandType`, `NinjamLocalFollowPolicy`, and `NinjamAudioTimingCommand` at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:16`–`50`.
- Protected remote timing representation: remote/source-rate and canonical/device-rate structures at `JammaLib/src/ninjam/NinjamTiming.h:9`–`37`.
- Protected source mapping: `SyncPhaseMap` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:49` and scene-anchor helper at `JammaLib/src/ninjam/NinjamLoopAlignment.h:93`.
- Local master/scene transport: `Timer::Command` at `JammaLib/src/utils/Timer.h:22`–`37`, `Timer::SceneSamplePos` at `JammaLib/src/utils/Timer.h:83`, and musical-position entry points at `JammaLib/src/utils/Timer.h:87`–`95`.
- Per-loop/per-take fan-out: audio body cursor/anchor methods at `JammaLib/src/engine/Loop.h:252`–`273`; timing command/map methods at `JammaLib/src/engine/LoopTake.h:235`–`251`; and station fan-out at `JammaLib/src/engine/Station.h:115`–`128`.
- MIDI ingress and clock surface: `MidiRouter`'s public routing snapshot and pump methods at `JammaLib/src/midi/MidiRouter.h:48`–`99`, backed by new `MidiBlockTiming.h` and `MidiClockAnchor.h`.
- NINJAM session/connection surface: live timing and split DAC/ADC export entry points in `JammaLib/src/ninjam/NinjamConnection.h:91`–`104` and `JammaLib/src/ninjam/NinjamSession.h:95`–`108`.
- Plugin contract: host time, multichannel processing, MIDI-block, editor, and state methods in `JammaLib/src/vst/IVstPlugin.h:24`, `73`, `84`–`97`, and `132`–`137`; concrete VST3 implementation begins at `JammaLib/src/vst/Vst3Plugin.h:29`.

Stage 2 should decide placement/overexposure. Stage 1 records the surface only and makes no layout judgment.

### Assets and project membership

- The branch adds 19 `trigger_*.tga` binaries totaling 242,752 bytes. Their current resource registrations begin at `Jamma/resources/ResourceList.txt:68`; station-ring shader registration is at `Jamma/resources/ResourceList.txt:20`.
- Four shader files are added (`cable.vert/.frag`, `station_ring.vert/.frag`), and `station.frag` plus the resource list are modified.
- App, library, and native-test project/filter files all change. Project membership is therefore part of the branch payload, not an untracked local artifact.
- No generated provenance is declared by the reviewed inputs; these are classified as binary/source assets, not as generated files.

### History/feature clusters used for inventory

The 123-commit range is not a single linear bug-fix series. Stable intent clusters, without making Stage 5 archaeology judgments, are:

| Cluster | Representative commits |
| --- | --- |
| Initial NINJAM UX/tempo policy | `0125b4c`, `5258f85`, `3ca53c7`, `92fc5e7` |
| Transport phase/MIDI anchor sync | `eb9db69`, `1cba2d6`, `aa76ab0`, `fbc76ba`, `6dc0c73`, `f68f4c8` |
| NINJAM audio wiring/latency/metronome | `adb7b38`, `782b8a8`, `c4c0607`, `e0bc33b` |
| MIDI routing/jitter/quantisation | `7668c29`, `95dea73`, `3db18e9`, `5bfebcd` |
| HUD/station/trigger visuals/resources | feature ancestry merged by `24a43a0` |
| Window placement/default persistence | `efe4962`, `8f50e13`, `704828f` |
| Timing refactor/unified audio command | `9c7ca51`, `d0d208e`, `f36bfe7`, `d6fdb88`, `b9619b9` |
| VST3 parity/state/mapping/resources | feature ancestry merged by `880112d` |
| Local loop alignment/source-coordinate map | `b41b5a8`, `4c1c0e1`, `f74d4ec`, `6abf7c7`, `e0f669c`, `0d90bac`, `bef7943`, `e72f3b0` |
| Local geometry/active-grid migration | `92e8907` |
| Repository build/tooling | `5daefa3`, `b43abbe`, `4a74ca9`, `f37e9c0` |

### Commands/queries used

- `git branch --show-current`, `git rev-parse HEAD`, `git merge-base master HEAD`, `git rev-parse master`, `git status --porcelain=v2 --branch`.
- `git rev-list --count master..HEAD`, `git rev-list --count --merges master..HEAD`.
- `git diff --shortstat|--summary|--name-status|--name-only|--numstat master...HEAD` with path/header filters.
- PowerShell grouping of `git diff --numstat` into a mutually exclusive partition, plus sorting by additions and total churn.
- `git log --reverse --date=short --format=... master..HEAD`.
- `git show --format=fuller --no-patch` and `git diff --stat|--shortstat|--name-only <merge>^1..<merge>` for the three merge commits.
- `git show --stat --oneline` for the window-persistence, resource, VST, HUD, and build/tooling commits.
- `git diff --unified=0|3 master...HEAD -- <interface paths>` for actual interface changes.
- `rg -n` for exact current declarations, includes/consumers, resource registrations, and current feature integration points.
- `Get-ChildItem ... | Measure-Object Length -Sum` for binary asset count/size.

No build or native test was run because this stage is a read-only scope inventory and makes no runtime assertion.

## System understanding

The branch's central timing path is a coherent remote-observation-to-audio-boundary flow: NINJAM connection/session state is validated and coordinated into a `NinjamAudioTimingCommand`; `AudioHost` consumes that command at an audio block boundary and applies it to `Timer` and the station hierarchy; each `Station` fans timing into its `LoopTake`s; and each take restores audio body cursors and the MIDI event cursor from entity-specific anchors plus common mapped elapsed time. The objective interface seams supporting that model are cited above. This inventory preserves the approved distinctions among follow policy, master phase, monotonic scene coordinate, source anchor, mapped elapsed time, and per-entity phase.

That central path is only part of the diff. The branch also contains full product-feature payloads for HUD/station visuals/resources and VST3 parity/state/mapping, as well as MIDI routing, NINJAM audio export/metronome work, window/default persistence, resource resolution, repository tooling, documentation, and broad native-test coverage. These clusters are connected at integration points such as `Scene`, `Station`, `LoopTake`, and project files, so later stages must review the branch as a system rather than assuming all non-NINJAM paths are incidental. Conversely, integration does not by itself prove those clusters belong in this merge; that is the scope decision captured below.

## Candidate findings

### S01-01 — HUD feature lineage is bundled into the remote-join bugfix branch without an explicit merge-scope decision

- Stage / reviewer: Stage 1 — Diff and ownership inventory.
- Scope reviewed / exclusions: objective scope/history and current integration points; no judgment of HUD architecture, implementation quality, or runtime behavior.
- Severity: must fix before merge (scope decision/documentation, not a claim that the feature is defective).
- Evidence: merge commit `24a43a05a53ecfb433452d5e4e517f47e43278aa` explicitly says `Merge branch 'feature/merge-hud' into feature/ninjam-time-align`. Relative to its first parent, it introduced 56 files with 2,213 insertions and 187 deletions, including the HUD, station/trigger visuals, shaders, 19 textures, engine integration, and tests. The final branch still owns `GuiHud` at `JammaLib/src/gui/GuiHud.h:34`, includes it from `JammaLib/src/engine/Scene.h:28`, stores it at `JammaLib/src/engine/Scene.h:372`, and registers its texture/shader assets at `Jamma/resources/ResourceList.txt:20` and `68`–`76`.
- Why it matters: this is a large independently developed UI/resource feature in a branch presented as `bugfix/align-remote-join`. Its inclusion materially expands merge review, asset provenance, project membership, and regression scope; the merge history establishes that breadth but the supplied branch-level review inputs do not record a human decision to ship it with the timing fix.
- Recommended disposition: retain with rationale only if the human gate explicitly accepts the HUD/visual/resource feature as part of this merge; otherwise move its feature lineage to a separately reviewed branch.
- Protected timing concepts affected: none.
- Verification: human confirms intended combined scope; if retained, later phase coverage includes GUI/graphics/resource project membership and manual HUD regression; if moved, regenerate `master...HEAD` inventory.
- Human decision: pending.

### S01-02 — VST3 parity feature lineage is bundled into the remote-join bugfix branch without an explicit merge-scope decision

- Stage / reviewer: Stage 1 — Diff and ownership inventory.
- Scope reviewed / exclusions: objective scope/history and interface expansion; no judgment of VST architecture, lifetime/thread correctness, persistence correctness, or test quality.
- Severity: must fix before merge (scope decision/documentation, not a claim that the feature is defective).
- Evidence: merge commit `880112d438e21983688395f60f970427d7f1d0b9` explicitly says `Merge branch 'feature/vst3-parity' into feature/ninjam-time-align`. Relative to its first parent, it introduced 25 files with 2,079 insertions and 347 deletions. The final branch includes the concrete `Vst3Plugin` at `JammaLib/src/vst/Vst3Plugin.h:29`, expands the common plugin interface for host timing/MIDI/editor/state at `JammaLib/src/vst/IVstPlugin.h:24`, `73`, `84`–`97`, and `132`–`137`, and retains VST3 mapping/state headers at `JammaLib/src/vst/Vst3MidiMapping.h:23` and `JammaLib/src/vst/Vst3StateBlob.h:33`.
- Why it matters: a separate plugin-host parity feature changes engine, persistence, resource, project, UI/editor, and test contracts in the same merge. The branch name and timing design do not communicate that review surface, and the feature's 1,268/81 final VST diff is one of the largest non-timing areas.
- Recommended disposition: retain with rationale only if the human gate explicitly accepts VST3 parity/state/mapping as part of this merge; otherwise move the feature lineage to a separately reviewed branch.
- Protected timing concepts affected: monotonic scene coordinate and plugin-facing musical transport are consumers of timing, but retaining or moving the VST3 parity feature must not collapse their coordinate semantics.
- Verification: human confirms intended combined scope; if retained, later coverage includes VST2/VST3 shared interface, persistence/state, editor/lifetime, MIDI mapping, and musical transport; if moved, regenerate inventory and verify timing still has its intended plugin-facing transport consumer.
- Human decision: pending.

### S01-03 — Standalone window-persistence and repository-tooling work also requires an explicit scope rationale

- Stage / reviewer: Stage 1 — Diff and ownership inventory.
- Scope reviewed / exclusions: objective commit/file scope only; no judgment of persistence correctness, build-wrapper correctness, or tooling quality.
- Severity: follow-up, to be decided at the Phase 1 scope gate before merge scope is frozen.
- Evidence: the consecutive window/default commits `efe4962`, `8f50e13`, and `704828f` add/repair window placement persistence; the behavior remains at `Jamma/src/Main.cpp:349`, `359`–`368`, and `451`–`461`. Repository-specific agent/build work arrives through `5daefa3`, `b43abbe`, `4a74ca9`, and `f37e9c0`; the final branch contains the Jamma worktree skill at `.github/skills/jamma-tree/SKILL.md:2`–`12` and makes the new MSBuild wrapper normative at `AGENTS.md:29`–`31`. These commits are distinct from the local-loop alignment series in subject and file ownership.
- Why it matters: these changes may be valuable prerequisites or housekeeping, but they create persistence/tooling contracts that are not implied by a remote-join bugfix and need an explicit retain/move decision so later review coverage is not accidental.
- Recommended disposition: retain with rationale if these are intentional release/build prerequisites; otherwise move them to separately reviewed changes. Record window persistence and repository tooling as separate human sub-decisions because one can be retained without the other.
- Protected timing concepts affected: none.
- Verification: human records the intended scope of the window-persistence cluster and the tooling cluster separately; retained work remains in later persistence/build-hygiene coverage.
- Human decision: pending.

No other Stage 1 finding is asserted. Breadth in engine, audio, NINJAM, MIDI, tests, and timing documentation has direct commit/history linkage to the branch's timing, join, audio-export, or loop-alignment work; later stages own whether its placement or implementation is acceptable.

## Handoffs

- Lead/integrator: correct the canonical header count from 58 to **61** (18 added, 41 modified, 2 renamed). The kickoff branch/HEAD/merge-base/master/stat/change-kind totals otherwise reproduce exactly.
- Lead/integrator: if replacing the kickoff approximate concentration table, use the exact mutually exclusive 16-row partition under Coverage; it reconciles all 199 files and all textual additions/deletions.
- Stage 2 (logical layout): consume the stable partition and interface list. In particular, assess the only changed `JammaLib/include` surface (`Constants.h`) and the cross-subsystem source headers; Stage 1 does not claim overexposure or misplacement.
- Stage 3 (conventions): the 61-header list is the complete header scope. Stage 1 did not inspect header/implementation placement, style, RAII, globals, or anonymous namespaces.
- Stage 4 (vocabulary): use the listed timing interfaces and preserve the approved protected glossary. The interface inventory does not propose combining command policy, Timer geometry, scene coordinates, source anchors, mapped elapsed time, or per-loop phase.
- Stage 5 (history): investigate the three merge lineages and the stable commit clusters. Specifically determine superseded/residue paths inside HUD and VST3 ancestry; Stage 1 only establishes their incoming size and current presence.
- Stage 6 (stale/dead code): consume Stage 5's residue candidates. The assets, new headers, and project entries listed here are inventory, not reachability findings.
- Later runtime stages: the changed hot-path files are `Scene.cpp`, `Loop.cpp`, `LoopTake.cpp`, `Station.cpp`, `Trigger.cpp`, and `NinjamConnection.cpp`, plus timing application through `AudioHost.cpp`, `Timer.cpp`, `MidiRouter.cpp`, and NINJAM coordinator/command/map code. Stage 1 makes no safety/correctness claims.
- Phase 1 gate: require separate approve/reject/defer decisions for S01-01 (HUD/visual/assets), S01-02 (VST3 parity/state/mapping), S01-03a (window persistence), and S01-03b (repository tooling), because these clusters are separable in intent even when their current integration overlaps.

## Uncertainties

- The repository contains no packaging/install manifest that definitively defines an external C++ SDK. I treated `JammaLib/include/**` as the explicit public include directory and `JammaLib/src/**/*.h` as internal-but-often-cross-subsystem interfaces. Stage 2 should refine that boundary if the project has an unstated compatibility contract.
- Asset origin/licensing/provenance is not stated in the reviewed inputs. The 19 TGAs are confirmed branch binaries and project resources, but Stage 1 cannot determine whether they are generated or hand-authored.
- Merge-parent diff sizes describe what each feature merge brought into its then-current first parent, not an additive decomposition of the final 199-file diff. Later commits modify/remove parts of those payloads, and the feature merges overlap shared integration files.
- Commit subjects and merge topology establish feature lineage, but only a human can say whether bundling was intentional. S01-01 through S01-03 therefore request explicit scope decisions rather than declaring the code unrelated or removable.
- `master` equaled the merge base at kickoff. If `master` advances before the gate, the inventory must be regenerated against the new merge base before Phase 2 relies on it.
- No runtime/build/test evidence was produced because that is deliberately outside this read-only inventory stage.

## Conclusion

Stage 1 covered all 199 changed files and all 123 branch commits at the inventory level, reproduced the kickoff identifiers and overall statistics, produced an exact non-overlapping subsystem partition, enumerated the changed interface surface, identified the high-churn and asset/project seams, and left a stable ownership handoff for Stages 2–6 and later phases.

There are three candidate scope findings: the independently merged HUD feature, independently merged VST3 parity feature, and standalone window/tooling clusters need explicit retain/move decisions before the merge scope is considered intentional. No architecture, style, runtime, test-quality, or dead-code conclusion is made here.
