---
name: jamma-self-validation
description: Validate a verified Jamma worktree build by checking its exact binary and process, gathering runtime logs and inspecting the GUI with computer-use.
---

# Jamma self-validation

Use this skill from the active Jamma checkout on Windows. Verify its root with `git rev-parse --show-toplevel`, `Jamma.sln` and `AGENTS.md`; locate every script relative to this `SKILL.md`, never from another checkout. Read `doc/diagnostics.md`. Require the successful JSON build record from the active worktree's task wrapper, then run `scripts/resolve-jamma-build.ps1` to check `Jamma/bin/x64/<Configuration>/Jamma.exe`, task-file hash and binary hash. No script contains a user-specific path. Never use Start menu, PATH, an installed copy or the main checkout for worktree validation.

Before launch, inspect all Jamma processes and stop the runtime check if another instance may hold unsaved work. Use `scripts/launch-test-session.ps1` only with the validated record and a fresh disposable `defaults.json` path in the ignored run directory. It sets `JAMMA_DEFAULTS_PATH` only for that process and returns PID, path and start time. Confirm the running process executable path matches the exact build before interacting. Use the `computer-use` skill and tool to target that window, exercise the stated scenario and inspect the rendered UI for clipping, dialogs and behavior. Capture a screenshot through computer-use; pass its saved path to `capture-window.ps1` for PID, window, timestamp and hash checks. A screenshot proves one frame, so report interaction evidence separately.

Use `collect-logs.ps1` with the session start time and explicit defaults path to collect timestamp-bounded startup and worker-failure logs. `inspect-runtime.ps1` checks the saved evidence manifests and hashes; the agent must still inspect behavior and log content. Record successes and concrete failures. Close only the disposable Jamma session you launched, without disturbing another process. If a capture is unavailable, say so; do not substitute an older binary. Real ASIO, MIDI/serial hardware, plugin sound, timing and subjective UX remain manual gates when no reliable probe exists. Do not disturb a live performance or add callback logging.

For cross-thread/audio changes, invoke the checkout's `threading-review`; for texture work use `tga-icon-gen`. Resolve repo-local skills from this checkout.
