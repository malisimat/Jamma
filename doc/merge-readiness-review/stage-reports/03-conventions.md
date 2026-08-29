# Stage 03 — Conventions

## Assignment

- Stage / reviewer: Phase 1 Stage 3, conventions investigator `/root/phase1_stage03_conventions`.
- Primary ownership: objective `AGENTS.md` and neighboring-style violations covering anonymous namespaces, header/implementation placement, naming-form consistency (not domain meaning), RAII/value semantics, and hidden mutable globals.
- Comparison and history: `master...HEAD` for current changes; `master..HEAD` and targeted `git blame`/`git show` for relevant history.
- Explicit exclusions: architecture or subsystem placement; timing/domain vocabulary semantics; runtime correctness; dead-code reachability; general subjective formatting.
- Output/single-writer constraint: this report is the investigator's only write. Production code, tests, build/project files, canonical artifacts, inventory, and other stage reports were not edited.

## Coverage

- Read in full before investigation: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Screened all 154 changed C++ paths in `master...HEAD` (61 headers and 93 `.cpp` files), including app/library code and native tests. Inspected the actual diff and surrounding current code for each high-signal match.
- Compared naming form against both current neighboring types and the merge-base version of the renamed quantiser header. Reviewed history/blame for the candidate lines and their introducing commits.
- Queries/commands used:
  - `git diff --name-status master...HEAD -- '*.cpp' '*.h' '*.hpp'`
  - `git diff --numstat master...HEAD -- '*.cpp' '*.h' '*.hpp'`
  - `git diff --unified=0` / `--unified=2` / `--unified=3 master...HEAD` with `rg` screens for anonymous namespaces, `new`/`delete`, `extern`, `static`, `thread_local`, namespace-scope variables, `using namespace`, field declarations, and acronym casing.
  - `rg -n` across changed/current headers and sources for inline bodies, public aggregate fields, namespace-scope state, raw allocation, and matching neighboring declarations/definitions.
  - `git show master:JammaLib/src/timing/TimingQuantiser.h`, targeted `git blame`, and `git log --oneline master..HEAD -- <path>` / `git show -s` for provenance.
- Negative coverage: the diff introduces no anonymous namespace; no added raw `delete`; the only added raw `new` is immediately adopted by `Steinberg::IPtr` in `JammaLib/src/vst/Vst3Plugin.cpp:840`; no new hidden namespace-scope mutable global was found. Added scoped-handle/context types and smart-pointer/container ownership follow RAII/value-semantics guidance. Existing anonymous namespaces in two unchanged test files and existing raw allocations outside added lines were not attributed to the branch.
- No build or test was run: this was a read-only convention review and made no code change.

## System understanding

- The branch adds a coordinated timing model spanning `AudioHost`, engine loop/take/station state, NINJAM observation/coordinator/command mailboxes, MIDI timing, and VST host-time projection. The protected scene, master-phase, per-loop-phase, mapped-elapsed, local/remote timing, and follow-policy distinctions were treated as fixed review constraints.
- Public parameter/snapshot/config aggregates in the engine and NINJAM neighborhood predominantly use PascalCase members, with initialisms treated as normal words (`Bpm`, `Bpi`). Private object state uses a leading underscore plus lower camel case, and function parameters/local variables use lower camel case.
- Non-trivial non-template NINJAM components generally expose declarations in headers and place computation in paired `.cpp` files, as shown by `ExportLaneTiming`, `NinjamMetronomeTiming`, and `NinjamTimingTracker`. Tiny accessors and compile-time algorithms remain appropriately inline/header-only.
- The new mailboxes use contained atomic state rather than global mutable registries; the VST additions generally adopt SDK objects into `IPtr` or local RAII guards.

## Candidate findings

### S03-01 — Normalize `BPI`/`BPM` aggregate-member casing to `Bpi`/`Bpm`

- Stage / reviewer: Stage 3 / conventions investigator.
- Scope reviewed / exclusions: naming form in new engine timing aggregates; no judgment about what BPM/BPI mean or whether the aggregates belong in `engine`.
- Severity: follow-up.
- Evidence: commit `92e8907` added `LocalAudioGeometry::BPI` at `JammaLib/src/engine/Quantiser.h:52` and `RemoteTransportGeometry::BPI` / `BPM` at `JammaLib/src/engine/Quantiser.h:97` and `JammaLib/src/engine/Quantiser.h:99`. The same current header uses the established form `QuantisationParams::Bpm` / `Bpi` at `JammaLib/src/engine/Quantiser.h:135` and `JammaLib/src/engine/Quantiser.h:136`; the merge-base `timing/TimingQuantiser.h` also used `Bpm` / `Bpi`, and current NINJAM timing aggregates use `Bpm` / `Bpi` at `JammaLib/src/ninjam/NinjamTiming.h:16` and `JammaLib/src/ninjam/NinjamTiming.h:17`. This is a form mismatch, not a semantic objection.
- Why it matters: three spellings for the same initialisms inside adjacent public timing aggregates make call sites and aggregate initialization less predictable and violate `AGENTS.md`'s requirement to respect existing naming conventions.
- Recommended disposition: rename the three new members to `Bpi`, `Bpi`, and `Bpm`, updating mechanical uses only.
- Protected timing concepts affected: none.
- Verification: compile affected `JammaLib` and native tests; use `rg -n '\\b(BPI|BPM)\\b' JammaLib/src/engine/Quantiser.*` to confirm remaining uppercase forms are comments rather than member identifiers.

### S03-02 — Use the NINJAM aggregate-member naming form consistently

- Stage / reviewer: Stage 3 / conventions investigator.
- Scope reviewed / exclusions: casing/form of public fields in two new NINJAM helper APIs; short names such as `n` and `pos` are handed to Stage 4 for semantic-quality judgment rather than treated here as semantic defects.
- Severity: follow-up.
- Evidence: commit `782b8a8` introduced lower-camel public members throughout `ExportLaneTimingInput`, `ExportLaneTimingState`, and `ExportLaneTimingResult`, including `n`, `numFrames`, `primed`, `valid`, and `generationReset` at `JammaLib/src/ninjam/ExportLaneTiming.h:19`, `JammaLib/src/ninjam/ExportLaneTiming.h:20`, `JammaLib/src/ninjam/ExportLaneTiming.h:32`, `JammaLib/src/ninjam/ExportLaneTiming.h:48`, and `JammaLib/src/ninjam/ExportLaneTiming.h:54`. Commit `c4c0607` did the same for the public `NinjamMetronomeTiming*` aggregates, for example `intervalPositionSamps`, `bpm`, `offset`, `accent`, `onsets`, and `valid` at `JammaLib/src/ninjam/NinjamMetronomeTiming.h:16`, `JammaLib/src/ninjam/NinjamMetronomeTiming.h:18`, `JammaLib/src/ninjam/NinjamMetronomeTiming.h:27`, `JammaLib/src/ninjam/NinjamMetronomeTiming.h:28`, `JammaLib/src/ninjam/NinjamMetronomeTiming.h:44`, and `JammaLib/src/ninjam/NinjamMetronomeTiming.h:46`. Neighboring NINJAM public aggregates use PascalCase (`NinjamTiming::IntervalLengthSamps`, `Bpm`, `Bpi`, `Generation`) at `JammaLib/src/ninjam/NinjamTiming.h:27` through `JammaLib/src/ninjam/NinjamTiming.h:32`, while private members consistently use leading underscores.
- Why it matters: these headers present public aggregate APIs but visually resemble local variables/private implementation state, making the NINJAM surface internally inconsistent and forcing callers to switch conventions between adjacent timing types.
- Recommended disposition: mechanically rename the affected public aggregate fields to PascalCase, preserving every type and meaning. Coordinate any more descriptive rename of `n`, `pos`, or `length` with Stage 4 rather than bundling semantic changes into this cleanup.
- Protected timing concepts affected: none.
- Verification: compile `JammaLib` and run focused `ExportLaneTiming` and `NinjamMetronomeTiming` native tests; search both headers and call sites for the retired lower-camel member spellings.

### S03-03 — Move substantial non-template NINJAM implementations behind declarations

- Stage / reviewer: Stage 3 / conventions investigator.
- Scope reviewed / exclusions: header/implementation placement only; no claim that the mailbox algorithms or timing calculations are incorrect, too slow, or architecturally misplaced.
- Severity: follow-up.
- Evidence: commits `d0d208e`, `d6fdb88`, `b41b5a8`, `4c1c0e1`, and `f36bfe7` leave substantial non-template bodies in public headers: `NinjamAudioTimingCommandMailbox::Publish` / `Consume` at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:61` and `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:83`, `LocalTransportOffsetLoopFracMailbox::Publish` / `ConsumeLatest` at lines 154 and 162, six runtime helpers from `IsValidRemoteTiming` through `ToDeviceTiming` at `JammaLib/src/ninjam/NinjamTiming.h:45` through `JammaLib/src/ninjam/NinjamTiming.h:140`, and `NinjamTimingObservationMailbox::Publish` / `ReadLatest` at `JammaLib/src/ninjam/NinjamTimingObservationMailbox.h:16` and `JammaLib/src/ninjam/NinjamTimingObservationMailbox.h:37`. Neighboring non-template helpers expose declarations and define bodies in paired sources: `ExportLaneTiming.h:70` -> `ExportLaneTiming.cpp:15`, `NinjamMetronomeTiming.h:53` -> `NinjamMetronomeTiming.cpp:15`, and `NinjamTimingTracker.h` -> `NinjamTimingTracker.cpp:5` / `:28`.
- Why it matters: these bodies make public headers carry mutable mailbox mechanics and runtime timing calculations, expanding recompilation and coupling details into every includer contrary to the immediate NINJAM header/source pattern. The types are non-template and do not require header definitions.
- Recommended disposition: retain compile-time `constexpr` algorithms and genuinely tiny accessors inline, but move the listed runtime bodies into appropriately named `.cpp` files without changing signatures or behavior.
- Protected timing concepts affected: none; this is a linkage/placement-only cleanup and must preserve all timing distinctions exactly.
- Verification: incremental `JammaLib` build followed by relevant NINJAM timing/mailbox native tests; compare exported declarations and run a link check to ensure every prior includer resolves the moved definitions.

## Handoffs

- Stage 4 (naming/domain vocabulary): decide whether `ExportLaneTimingInput::n`, `pos`, and `length` need semantically richer names. S03-02 owns only casing/form and must not pre-empt that decision.
- Stage 2 (logical layout): S03-03 does not decide which file/subsystem should own the runtime helpers beyond observing the current neighboring header/source convention; if Stage 2 finds a different owner, its boundary conclusion takes precedence over a purely mechanical same-namespace split.
- Phase 2 runtime reviewers: the atomic protocols, memory ordering, callback cost, and SDK lifetime correctness were deliberately not validated here. The negative convention screen is not runtime-safety evidence.
- Stage 6: no reachability conclusion was drawn for any helper or field.

## Uncertainties

- The repository has older header-heavy areas and file-scope `static` test helpers, so S03-03 is grounded specifically in the immediate non-template NINJAM neighborhood rather than asserted as a repository-wide ban. If there is an undocumented build/link rationale for keeping these files header-only, the integrator should retain that rationale and reject or narrow S03-03.
- `NinjamTiming.h:43-44` says it is self-contained so the test project can include it without `JammaLib/include`. That explains avoiding `Constants.h`, but does not by itself establish that runtime bodies must remain inline; build owners should confirm before accepting S03-03.
- Lower-camel public fields are established in some other domains (for example `midi::MidiEvent` and VST host-time state), so S03-02 is intentionally limited to consistency within the NINJAM public aggregate neighborhood.
- Static free helper functions added in implementation/test files were not reported merely for textual `static` matches: neighboring test files already use that pattern, and the explicit policy violation to detect was introduction of anonymous namespaces, of which there were none.

## Conclusion

Stage 3 reports three follow-up candidates: two objective naming-form inconsistencies and one bounded NINJAM header/implementation-placement concern. No merge blocker or must-fix convention violation was found. The branch introduces no anonymous namespace, no uncovered raw owning allocation in added code, and no new hidden mutable global in the screened C++ diff. This report is complete for the assigned scope, subject to the stated runtime, architecture, semantic-vocabulary, and reachability handoffs.
