# Review areas

## Correctness and lifecycle

Trace the changed behavior from input through state transition to observable result. Check rejection, cancellation, failure, disconnect, replacement and teardown where applicable. Look for stale identities, queued work surviving invalidation, partial publication and persistence/save-target mistakes. Review the complete selected delta, not only files implied by the feature name.

## Ownership and conventions

Map new behavior to `doc/glossary.md`. Check Station -> LoopTake -> Loop ownership, app-shell versus JammaLib responsibilities, and Scene's composition/coordination role. Compare new code with the nearest maintained examples in the same subsystem: method/class/member naming, typed commands, error reporting, value semantics, const use, RAII and explicit side effects. Cite the example when claiming a convention. Existing legacy code is not automatically the preferred pattern when canonical guidance says otherwise.

For each expanded large class, identify the responsibility added. Check for an existing owner before recommending a new helper. Assess both header state/API growth and implementation growth. Line counts are physical lines, including comments and blanks; they indicate review attention, not complexity or correctness. A file crossing 1,000 lines deserves deliberate inspection, not automatic splitting. Inspect new files too, so splitting one large class into poorly bounded files cannot hide growth. Challenge new modes, repeated branches, thin wrappers and duplicate state; prefer removing complexity over moving it. The repo-local [code-clean audit](../../jamma-code-clean/SKILL.md) gives the weekly inventory and deeper owner analysis. Avoid template/macro cleverness, hidden mutable globals and raw ownership unless justified under repo guidance. Anonymous namespaces are explicitly prohibited by AGENTS.md; comment/string matches are not violations.

## Real-time and cross-thread safety

Start at every affected callback root in `doc/realtime-audio.md`, including AudioHost, and trace changed reachable MIDI/VST/helpers. Inspect allocation and deallocation, container growth, shared_ptr ownership changes, exceptions, formatting/logging, locks, waits, I/O, system calls and unbounded work. A call can hide these operations; a newly allocating helper can affect an unchanged callback.

For each new or changed shared state, record owner thread, readers, writers, synchronization/publication mechanism and teardown. Check immutable topology, bounded borrow success/failure, writer serialization, reader shutdown and reclamation against the existing snapshot protocol. A lifetime lease does not synchronize mutable processor state. Off-callback locks require explicit ownership/ordering; their presence alone is not a callback violation.

Static findings do not prove zero allocation or race freedom. Where that claim matters, request or run focused instrumentation using preallocated fixtures and an appropriate callback region; distinguish host work from third-party plugin internals. Preserve functional regressions even when their fake allocates; those tests simply do not establish allocation freedom.

## Tests and validation

Inspect added, modified and removed assertions, not only declaration counts. Require a plausible regression or observable behavior for each proposed new test. Favor audible/persistent behavior, ownership, cancellation, bounded lifecycle and meaningful interaction. Avoid decorative pixel/spacing/symmetry contracts, implementation mirroring and duplicate scenarios unless they protect a stated requirement. Bounds, clipping, hit targets and rendering-state restoration can be functional requirements; do not classify every GUI test as cosmetic.

Check that retained tests are compiled, hermetic and bounded. Investigate lost active coverage; do not restore disabled or duplicate tests merely to preserve counts. Prefer a minimal extension to an existing fixture over a new suite for every branch. Run proportionate affected checks under `doc/build.md` and local `.vscode/tasks.json`; report actual commands and results. A green build is not test execution, and a static scan is not runtime acceptance.
