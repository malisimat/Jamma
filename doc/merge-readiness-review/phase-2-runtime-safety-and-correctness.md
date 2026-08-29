# Phase 2 — Runtime Safety, Real-Time Performance, and Correctness

## Goal and human gate

Prove that the branch respects real-time audio constraints and cross-thread ownership while preserving correct timing behaviour through remote joins. Human gate: approve the concurrency model, hot-path risk register, and required regression evidence before runtime changes.

## Outputs

- Extend the inventory with a thread/ownership matrix: writer, reader, handoff mechanism, lifetime, and hot-path status for each changed shared state item.
- Record all safety and performance concerns in `findings.md`; record acceptable residual risks in `decisions.md`.
- Define focused runtime scenarios and assertions in `verification-matrix.md`.

## Independent review stages

7. **Thread safety** (`stage-reports/07-thread-safety.md`). **Primary ownership:** changed cross-thread state, writer/reader relationships, publication mechanism, memory ordering, compound-state consistency, and race freedom. **Excludes:** cost analysis except to hand off hot-path locks to stage 8; object destruction after ownership has ended goes to stage 12. **Required handoff:** completed thread/ownership rows with exact access sites.
8. **Audio and other hot-path performance** (`stage-reports/08-hot-paths.md`). **Primary ownership:** work performed on proven audio/timing/MIDI/render hot paths—locks, allocation, logging, I/O, waits, exception-capable operations, copies, loop bounds, and repeated calculation. Consume stage 7's handoff rather than repeating concurrency proof. **Required handoff:** hot-path table with call-chain evidence and likely frequency/cost.
9. **Timing and remote-join correctness** (`stage-reports/09-timing-correctness.md`). **Primary ownership:** behavioural invariants across local playback, empty/populated join, accepted tempo change, late observation, reconnect, departure, and each follow policy. Preserve different loop lengths and intentional offsets while checking authority and coordinate conversion. **Excludes:** arithmetic edge mechanics owned by stage 11 and generic transition recovery owned by stage 10. **Required handoff:** transition narrative plus expected and observed invariants.
10. **State-machine and failure paths** (`stage-reports/10-state-machine.md`). **Primary ownership:** transition completeness and recovery for startup, join/leave, connect/disconnect, timeout, malformed/late input, availability changes, and teardown. **Excludes:** timing mathematics and low-level lifetime proof. **Required handoff:** state/transition table identifying idempotence, invalid mixed states, and recovery owner.
11. **Numerical, boundary, and clock domains** (`stage-reports/11-numerics.md`). **Primary ownership:** units and coordinate annotations, conversions, rounding, overflow, sign changes, invalid inputs, long duration, wraparound, and precision. **Excludes:** policy/state intent owned by stages 9–10. **Required handoff:** conversion table with input/output domain, formula, bounds, and rounding rule.
12. **Resource and lifetime review** (`stage-reports/12-lifetimes.md`). **Primary ownership:** construction/destruction and ownership of buffers, queues, registrations, threads, VST objects, resources, and network/session objects, including leaks, stale callbacks, overflow policy, and shutdown lifetime races. **Excludes:** shared-state race analysis while objects are live (stage 7). **Required handoff:** lifetime/cleanup table and any shutdown ordering constraints.

## Human review packet

The integrator writes `phase-packets/phase-2.md` with a single reconciled thread/ownership table, hot-path table, timing-transition narrative, numerical-domain table, lifetime risks, coverage gaps, and only evidence-backed blockers. It must explain the end-to-end timing flow in plain language rather than concatenating reports. The human reviewer explicitly accepts the concurrency approach, especially every non-atomic shared state and every intentional non-real-time allocation/lock.
