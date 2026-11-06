---
name: jamma-version-bump
description: Increment Jamma's app patch version once, with a checked script that synchronizes the code version and README before surge handover.
---

# Bump Jamma's patch version

Use this skill after a substantial Jamma surge has been integrated into its single `feature/` or `bugfix/` branch, before final combined build, review, self-validation and manual-test handover. The version bump is part of that branch, never a reason to merge it onward. Do not bump on an incomplete or blocked surge, or for a docs-only/test-only change. Record the decision in the surge plan and run record.

In the active worktree, verify `Jamma.sln`, read `AGENTS.md`, and inspect the current version. `JammaLib/include/Constants.h` defines `MAJOR_VERSION`, `MINOR_VERSION`, `PATCH_VERSION` and therefore the app's `LIB_VERSION`; `README.md` repeats `Current Version: v…`. These must agree. Resolve this skill and `scripts/bump_version.py` from the active worktree's `.agents/skills/jamma-version-bump/`, never from another checkout or an installed global copy.

Run `python scripts/bump_version.py --repo <absolute-active-worktree> --expected <current-major.minor.patch>` to preview the exact one-step patch bump. Then run the same command with `--apply`. The expected version is the value recorded for this surge before the first apply; reuse it when resuming. The script returns `already_applied` on a repeat call instead of incrementing again, and fails on divergence or unexpected file format. Do not hand-edit a failed bump; investigate the source and script first.

Inspect the two-file diff, include only those files in one focused version commit, and record old/new version and commit in the surge run. The final build and runtime self-validation must use the bumped source, with the resulting app version checked in the GUI or startup log when available. If a concurrent surge already advanced the version, stop and rebase or reconcile the plan rather than silently choosing another number.
