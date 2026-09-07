---
name: threading-review
description: Use for cross-thread state changes, atomic-vs-mutex decisions, and hot-path lock regression review.
---

# Jamma thread-state review guardrails

Use the host harness's file inspection and search tools as available. Start
with `doc/realtime-audio.md`, then run the audit script from the repository
root using PowerShell:

`powershell -NoProfile -ExecutionPolicy Bypass -File .agents/skills/threading-review/audio-hotpath-audit.ps1`

If PowerShell is not the host's shell, invoke the script through an equivalent
Windows PowerShell-compatible runner or perform the same checks manually; do
not treat the tool name as part of the review rules.

- For every new shared variable, write down: owner thread, readers, writers, synchronization primitive, and teardown path.
- Prefer single-owner handoff or atomics for scalar/handle state that crosses threads.
- Mutexes are only acceptable off the audio hot path, with explicit ownership and lock ordering.
- Never introduce locks or blocking waits inside the callback-owned functions listed in `doc/realtime-audio.md`.
- After edits, run the audit script, then manually inspect the callback-owned bodies for new lock, wait, allocation, or logging regressions.

Refs: `doc/realtime-audio.md`, `.agents/skills/threading-review/audio-hotpath-audit.ps1`
