# Strict structural review criteria for Jamma

Use this to challenge a cleanup candidate, not as a mandate to create abstractions. The user's request sets an aggressive standard for finding simplifications; Jamma's glossary and callback rules constrain the remedy.

## Inspect change, not just size

- List the current 20 largest first-party code files and their prior counts. Inspect both newly large files and the fastest growing files, including headers, so splitting one implementation cannot hide interface growth. A 1,000-line crossing is a strong review signal, not a failure by itself.
- Use the existing `jamma-review` static checker on the bounded Git delta, and separately on relevant dirty code. Its lock/heap/I/O matches are search leads; only callback reachability establishes a real-time concern.
- Trace recent commits/diff hunks for unrelated responsibilities entering a large owner, repeated condition chains, one-off booleans/modes, copied policy, generic helpers with one caller, and persistence or timing logic leaking into GUI/Scene. Ask whether the change can remove a state, branch or layer entirely.
- For every proposed move, show which existing owner has the data and timing authority; which methods/state leave the large class; what small interface callers must learn; and which order, error, thread and performance guarantees remain visible. Prefer a deep module: useful behavior behind a small stable interface, with local implementation detail. Do not reward code motion, extra indirection, or a line-count reduction alone.
- Apply the deletion test to a proposed collaborator: if removing it merely erases a pass-through, the interface is shallow. Avoid a hypothetical interface/adapter when nothing varies. One clear owner and one cohesive implementation are often better than a new abstraction.
- A large class can be justified if its responsibilities are cohesive and the interface hides complexity. Say so when evidence supports it. Conversely, small files can be bloated when their many callers must understand scattered invariants.

## Jamma-specific checks

- Scene owns session composition, lifecycle and cross-subsystem coordination. It must not absorb audio-block advancement, per-loop phase, remote follow policy, persistence reconstruction or control-detail behavior. Before proposing a new Scene collaborator, check existing ones in `doc/glossary.md` and trace call sites.
- AudioHost owns block application/advancement; Station -> LoopTake -> Loop owns entity state. Do not hide allocations, locks, waits, formatting, I/O, exceptions or unbounded traversal behind a new callback helper. Off-callback orchestration can use locks when ownership is explicit.
- Changes in timing/follow/alignment need `doc/loop-alignment-and-ninjam-sync.md`. Preserve unequal lengths, offsets, reconnect, delayed observations and NoSync invalidation.
- Tests should guard observable behavior, failure/cancellation, lifetime, clipping, reachability and hit-testing. Inspect individual assertions within broad useful cases. Challenge exact coordinates, symmetric spacing, style constants, font choices and duplicates when a visual decision is the only contract.

## Evidence standard

For each new finding give the branch, HEAD, previous run/commit, code paths and line numbers, triggering change or current dependency pattern, consequences, proposed smaller design, rejected simpler alternatives and a bounded verification path. Identify a real owner and caller-visible interface before using the phrase “god class.” Avoid recycling unchanged findings. A static match or file-size threshold is only a candidate.

The approach adapts strict structural questions from Cursor's [Thermos maintainability rubric](https://github.com/cursor/plugins/blob/main/thermos/skills/thermo-nuclear-code-quality-review/SKILL.md) and interface depth/locality from Matt Pocock's [codebase-design skill](https://github.com/mattpocock/skills/blob/main/skills/engineering/codebase-design/SKILL.md). These are design lenses, not Jamma policies. The Thermos source is in Cursor's repository; attribution to Cloudflare was not verified.
