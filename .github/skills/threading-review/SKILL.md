---
name: threading-review
description: Use for cross-thread state changes, atomic-vs-mutex decisions, and hot-path lock regression review.
allowed-tools:
  - view
  - rg
  - powershell
---

# Jamma thread-state review guardrails

Start with `doc/realtime-audio.md`, then run:

`powershell -NoProfile -ExecutionPolicy Bypass -File .github/skills/threading-review/audio-hotpath-audit.ps1`

- For every new shared variable, write down: owner thread, readers, writers, synchronization primitive, and teardown path.
- Prefer single-owner handoff or atomics for scalar/handle state that crosses threads.
- Mutexes are only acceptable off the audio hot path, with explicit ownership and lock ordering.
- Never introduce locks or blocking waits inside the callback-owned functions listed in `doc/realtime-audio.md`.
- After edits, run the audit script, then manually inspect the callback-owned bodies for new lock, wait, allocation, or logging regressions.

Refs: `doc/realtime-audio.md`, `.github/skills/threading-review/audio-hotpath-audit.ps1`
