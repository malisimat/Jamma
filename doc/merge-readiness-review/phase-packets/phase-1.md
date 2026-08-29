# Phase 1 packet — Baseline, intent, and structural review

## Inputs received

- Governing inputs: `AGENTS.md`, the main merge-readiness plan, all four phase files, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`, and `doc/build.md`.
- Kickoff baseline and protected glossary: `../00-scope-and-inventory.md`.
- Complete immutable investigator reports: `../stage-reports/01-diff-inventory.md` through `06-stale-code.md`.
- Comparison fixed at kickoff: branch `bugfix/align-remote-join`, `HEAD` `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, merge base and `master` tip `4941b780f7ff5a46f742167d79338e3ab592a565`.
- Current canonical artifacts: `../findings.md` and `../verification-matrix.md`. `decisions.md` remains pending the human gate; no cleanup was authorized.

All six investigators wrote only their assigned report. No production code, test, build/project file, or unrelated working-tree change was edited. The lead wrote only authorized review artifacts.

## Coverage and exclusions

### Inventory coverage

Stage 1 reconciled every one of the 199 changed files and all 123 branch commits. The final comparison is 14,691 insertions and 1,507 deletions across 71 added, 124 modified, and 4 renamed paths. The exact subsystem partition sums to those totals. There are 61 changed header paths (18 added, 41 modified, 2 renamed), correcting the kickoff estimate of 58.

Largest added-line concentrations are `GuiHud.cpp` (721), `Vst3Plugin.cpp` (720), `NinjamTimingIntegration_Tests.cpp` (650), `Scene.cpp` (607), `NinjamTimingCoordinator_Tests.cpp` (547), `Station.cpp` (521), `LoopTakeTiming_Tests.cpp` (476), `LoopTake.cpp` (452), `NinjamConnection.cpp` (428), and `NinjamTimingCoordinator.cpp` (388). The branch includes 19 new TGA binaries and four new shaders; Stage 6 proved ten of those TGAs unregistered and otherwise unreferenced.

### Stage coverage

| Stage | Actual coverage | Deliberate exclusions handed forward |
| --- | --- | --- |
| 1 — Diff inventory | All files/commits, exact partition, high churn, headers, assets, feature lineage | No layout, quality, correctness, or reachability judgment |
| 2 — Logical layout | 55 changed audio/engine/ninjam/midi/utils files plus include direction and history | GUI/VST/I/O layout except timing seams; no runtime proof |
| 3 — Conventions | All 154 changed C++ paths | No architecture, semantic vocabulary, correctness, or deadness |
| 4 — Vocabulary | All 61 changed headers screened; deep timing/audio/engine/MIDI/NINJAM/VST boundary trace | No style, placement, runtime proof, or reachability |
| 5 — History | All 123 commits and all three merge parents; timing, HUD, VST, diagnostic chains | No current deadness/correctness judgment |
| 6 — Stale code | Both history residues, all added assets, old names, bounded low-reference sweep across inventory | No broad live-abstraction simplification or Phase 2/3 judgments |

Phase 1 did not build, run tests, execute Jamma, prove thread safety, measure hot paths, validate timing mathematics, judge persistence/security, or implement cleanup. Those are explicit Phase 2–4 responsibilities. The VST, GUI, persistence, and tooling surfaces were inventoried and historically scoped but not fully judged for architecture because their inclusion is itself at the human scope gate.

No unowned coverage gap blocks this packet. Explicit handoffs are: callback diagnostic cost to Stage 8; end-to-end join/follow invariants to Stage 9; numerical clock-domain proof to Stage 11; asset/plugin lifetime to Stage 12; live-abstraction simplification to Stage 13; test contract quality to Stage 14; glossary/docs/UI consistency to Stage 15; persistence/VST/window compatibility to Stage 16; retained diagnostics to Stage 17; and project/build/resource hygiene to Stage 21.

## Integrated system model

The branch is not one narrow patch. It combines the remote-join/timing redesign with MIDI timing/routing, NINJAM audio/metronome work, engine refactors, VST3 parity, HUD/resources, window persistence, build tooling, documentation, and a large native-test expansion. The last four areas require explicit scope decisions because their feature lineages are independently identifiable.

The timing system itself has a coherent direction. NINJAM connection/session code produces source-rate remote observations. Those are validated and converted to device-rate timing, then the coordinator decides request/acknowledgement state, accepted remote authority, generation, and one of three follow policies. Scene currently turns that update into one immutable latest-wins timing command and forwards it. At the top of the next audio block, AudioHost consumes at most one command before station playback advances.

AudioHost is the block-boundary integrator. It updates Timer's local master geometry and musical transport, maintains the common remote-to-local source ruler, and applies one mapped correction to every local Station. The fan-out follows the core hierarchy `Station -> LoopTake -> Loop`. Each take/loop retains its own anchor and length; audio body cursors and the MIDI event cursor wrap the common mapped source progress in their own coordinate systems. Different loop lengths and intentional offsets therefore do not become one shared loop cursor.

The monotonic scene coordinate is the durable anchor ruler. Master phase remains the wrapped position within a master interval. `SourceCoordinateAtOrigin` is the monotonic local-source coordinate used by the sync map; wrapped source phase is derived from it. Follow policy chooses whether/how accepted remote authority disciplines the local state, but is not a clock or coordinate. `NoSync` clears the map and anchors and returns local timing to free-run. The automation global-sample origin is separate from the MIDI event cursor. These distinctions are both design-documented and history-backed; none of the proposed changes may collapse them.

The main structural opportunity is to finish boundaries already implied by that flow:

| Concern | Current split | Proposed boundary |
| --- | --- | --- |
| Follow-policy interpretation | NINJAM + AudioHost + core engine APIs | Interpret once at NINJAM/AudioHost boundary; neutral engine operations |
| Common source ruler | AudioHost plus copied geometry in every take | One AudioHost-owned map; takes keep entity anchors and receive source coordinate |
| Local timing values | Bundled with Quantiser/UI controller header | Small value-only engine timing contract |
| Local transport-offset mailbox | Declared under NINJAM | AudioHost-local or proven generic latest-value utility |
| Accepted update -> audio command | Decision in coordinator, materialization in Scene | Complete immutable command from NINJAM integration; Scene forwards/presents |

These are follow-up structural findings, not evidence that the current high-level direction is wrong. In particular, the common-map recommendation replaces currently live copied geometry only after the single-owner path exists.

## Canonical findings added/changed

Twenty canonical findings were added to `../findings.md`: 6 must-fix-before-merge decisions, 14 follow-ups, and no Phase 1 merge blocker.

| IDs | Group | Count | Integration summary |
| --- | --- | ---: | --- |
| F-001–F-004 | Scope | 4 | HUD, VST3, window persistence, and tooling require separate retain/move rationale decisions. |
| F-005–F-009 | Boundaries | 5 | Policy translation, map ownership, value-only timing contract, local mailbox owner, command materialization. |
| F-010–F-012 | Conventions | 3 | Acronym casing, bounded NINJAM aggregate casing, non-template runtime bodies. |
| F-013–F-017 | Vocabulary | 5 | Remote grid vs grain, command authority/coordinates, observation clock domains, automation origin, export cursor units. |
| F-018–F-020 | Removal candidates | 3 | Duplicated stored phase, dead receipt subset, ten dead textures conditional on HUD scope. |

The four must-fix vocabulary findings are ranked by semantic risk:

1. F-013: remote BPI-derived grid step is named/displayed as local grain, directly colliding with the protected glossary.
2. F-014: accepted remote replacement and correction names hide authority and switch coordinate meaning at the audio boundary.
3. F-015: Timer-absolute and device-audio observation anchors have names that do not expose incompatible clock domains.
4. F-016: the MIDI automation global-sample origin is named as though it were a per-loop phase anchor.

F-017 is lower risk/value and conditional on retaining the disabled export-latency helper. F-010/F-011 are mechanical convention work; when F-011 and F-017 overlap, implement one semantic PascalCase rename rather than two changes.

Historical residue is narrowly bounded. `SourcePhaseAtOrigin` survived the phase-to-source-coordinate pivot but has no reader; F-018 removes only that duplicated stored value. Debug-origin diagnostics split into two classes: F-019 covers an eleven-atomic receipt path with no writer and an impossible logger branch, while the before/after snapshots and coordinator counters are reachable/dormant and remain for Stages 8/17. If HUD is retained, F-020 can delete ten copied but unregistered coloured textures (164,020 bytes); if HUD moves, the files move with it.

Potential later moves/deletions are therefore: move four optional feature clusters if humans exclude them; move/consolidate the five structural boundaries; rename the three convention and five vocabulary groups; delete one stored field, one never-published receipt surface, and conditionally ten texture files. Phase 1 authorizes none of these edits.

## Duplicates and contradictions resolved

- Kickoff header count `58` vs Stage 1 `61`: Stage 1's exact 18-added/41-modified/2-renamed enumeration supersedes the kickoff estimate; inventory was corrected.
- S05-01 and S06-01: merged into F-018. Stage 5 supplies the `1d665d2` -> `e72f3b0` direction history; Stage 6 supplies current no-reader evidence.
- S05-02 and S06-02: split. Only the never-published `AlignmentReceipt` subset becomes F-019; live/dormant snapshots and coordinator diagnostics are later performance/observability handoffs, not a stale-code deletion finding.
- S03-02 and S04-05: retained as separate F-011 naming-form and F-017 semantic-vocabulary findings. If both are approved, overlapping export fields are changed once using semantic PascalCase names. The convention finding is intentionally limited to the immediate NINJAM aggregate neighborhood.
- S02-02 versus Stage 6's “live/merely complex” map classification: no contradiction. The current copied per-take map is live; F-006 is a future replacement/consolidation, not a stale deletion. Per-entity anchors remain owned by takes.
- S01-03 was split into F-003 window persistence and F-004 tooling because the human can retain one and move the other.
- S06-03 is not merged into the HUD scope finding: F-020 remains a conditional deletion only if F-001 retains HUD; otherwise it resolves with the moved lineage.

The Stage 2, Stage 3, and Stage 6 investigators explicitly confirmed these reconciliations in follow-up. No unresolved evidentiary contradiction remains. The main uncertainty is human product/release scope, not investigator disagreement.

## Verification additions

`../verification-matrix.md` now maps every canonical finding to static/build, native-test, runtime/manual, and later-owner obligations. No check is marked executed.

The minimum protected timing regression envelope carried forward is:

- unequal loop lengths and intentional relative offsets;
- empty and populated remote joins;
- accepted close-tempo (`ContinuousSync`) and materially different-tempo (`BlockSync`) changes;
- `Stay local`, invalid timing, disconnect, and `NoSync` anchor/map invalidation;
- late observation, rebase, remote wraps, reconnect, and generation gating;
- distinct Timer-absolute, device-audio, monotonic scene, remote master phase, local source coordinate, per-loop cursor, and automation-origin values;
- audio body cursor and MIDI event/automation phase coherence.

Retained optional scope adds HUD/resource, VST2/VST3, window persistence, and build-wrapper verification. Future builds/tests must first read `.vscode/tasks.json` and `doc/build.md`, use the repository wrapper, and run incremental affected targets.

## Gate questions

Approval accepts a finding and its recommended disposition for later reconciliation; it does **not** authorize source edits. Rejection requires a rationale in `decisions.md`. Deferral requires an owner/phase and explicit risk. For scope findings, “approve” must also choose retain or move.

### Baseline decisions

| Gate | Exact decision required |
| --- | --- |
| G1 | Approve, reject, or defer the branch/commit/diff statistics, exact 199-file subsystem inventory, 61-header interface inventory, hot-path map, asset inventory, and declared coverage/exclusions. |
| G2 | Approve, reject, or defer the protected glossary in `00-scope-and-inventory.md`, including the separation of follow policy, local/remote timing, master phase, scene/source coordinates, mapped elapsed time, per-entity phase, local grain, and remote/active grid. |

### Canonical finding decisions

| Finding | Exact approve/reject/defer decision |
| --- | --- |
| F-001 | Approve **retain HUD with combined-scope rationale** or approve **move HUD lineage**; otherwise reject or defer. |
| F-002 | Approve **retain VST3 parity with combined-scope rationale** or approve **move VST3 lineage**; otherwise reject or defer. |
| F-003 | Approve **retain window persistence** or approve **move it**; otherwise reject or defer. |
| F-004 | Approve **retain tooling as prerequisite** or approve **move it**; otherwise reject or defer. |
| F-005 | Approve neutralizing the engine timing API boundary; reject; or defer to a named later phase/owner. |
| F-006 | Approve future single-owner map consolidation with all protected invariants; reject; or defer. |
| F-007 | Approve extracting a value-only local timing contract; reject; or defer. |
| F-008 | Approve moving the local-only mailbox; reject; or defer. |
| F-009 | Approve moving command materialization to NINJAM integration; reject; or defer. |
| F-010 | Approve the `Bpi`/`Bpm` casing normalization; reject; or defer. |
| F-011 | Approve bounded NINJAM aggregate PascalCase normalization; reject; or defer. |
| F-012 | Approve moving non-template runtime bodies behind declarations; reject; or defer. |
| F-013 | Approve reserving “grain” for local construction and renaming remote grid-step fields/UI; reject; or defer with explicit glossary risk. |
| F-014 | Approve the accepted-remote-command authority/coordinate rename group; reject; or defer with explicit coordinate-confusion risk. |
| F-015 | Approve distinct Timer-absolute/device-audio observation names; reject; or defer with explicit clock-domain risk. |
| F-016 | Approve renaming the automation global-sample origin; reject; or defer with explicit phase-vocabulary risk. |
| F-017 | Approve semantic export-lane cursor names if the helper remains; reject; or defer. |
| F-018 | Approve deleting only stored `SourcePhaseAtOrigin`; reject; or defer. |
| F-019 | Approve deleting only the unwritten receipt subset while retaining live diagnostics for later review; reject; or defer. |
| F-020 | If F-001 retains HUD, approve deleting the ten dead textures; if HUD moves, approve resolving them with the lineage; otherwise reject or defer. |

Phase 2 must not start until G1, G2, and F-001–F-020 have explicit outcomes in `decisions.md`. A concise response can use IDs, for example `G1 approve; G2 approve; F-001 approve-retain; ...` with rationales for rejections/deferrals.

## Human outcome

Pending. Phase 1 is stopped at the human gate. No cleanup implementation, Phase 2 investigation, build, or test is authorized by this packet.
