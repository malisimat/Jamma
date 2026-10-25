---
name: threading-review
description: Review Jamma cross-thread ownership, atomic-vs-mutex decisions and callback safety using the real-time area of jamma-review.
---

# Jamma threading review compatibility entry point

Read `../jamma-review/SKILL.md` from the active checkout and apply its scope/evidence procedure with the real-time and cross-thread checklist. Preserve the owner/readers/writers/synchronization/teardown table for new shared state and inspect affected callback bodies and transitive helpers manually.

The existing `audio-hotpath-audit.ps1` command delegates to the shared checker. Prefer explicit `-Base <sha> -Head <sha>` for committed review, or `-Base <sha> -WorkingTree` for current files. With no arguments it preserves the old local-edit scope, warns about that limitation and cannot approve a committed branch.
