# Merge brief — `bugfix/align-remote-join`

> **Recommendation: READY WITH ACCEPTED RISKS.** The four-phase review and approved B001-B017 cleanup sequence are complete. No technical blocker remains inside the approved scope. Merge remains a human decision because explicitly accepted product risks and unavailable live/manual/tooling evidence remain.

## Decision snapshot

| Item | Value |
| --- | --- |
| Target | `master` |
| `master` / merge base | `4941b780f7ff5a46f742167d79338e3ab592a565` |
| Phase 4 approval gate | `2e770b743d9f2466b2edafff5c92faf139d93108` |
| Verified implementation tip | `2dc6cf8d3feefdcb4a7fcdade518f05d0f9b3b6c` |
| Cleanup batches | B001-B017 complete and independently approved |
| Final Debug solution Build | Passed |
| Final Release solution Build | Passed |
| Full Debug native suite | 844 run; 843 passed; one expected hardware skip; zero failed |
| Full-range / cleanup-range diff checks | Clean / clean |
| Final classification | **READY WITH ACCEPTED RISKS** |

This document is the final synthesis required by `merge-readiness-plan.md`. Detailed execution evidence is in `stage-reports/21-merge-hygiene.md`; per-finding evidence is in `verification-matrix.md`; implementation order and closeouts are in `cleanup-backlog.md`; independent decisions are in `batch-reviews/`.

## What the branch now does

The branch retains a single explicit timing model:

- The NINJAM job side owns physical connection availability, runtime session epoch, validated remote observations, tempo prompts, follow policy, remote grid, and production of one complete desired remote transport value.
- Scene presents prompts and forwards complete values. It does not reconstruct timing authority or format diagnostics on the audio callback.
- AudioHost applies desired state at the audio-block boundary, owns Timer replacement and the single common remote-to-local mapped elapsed calculation, and fans out neutral reset/correction/restore operations.
- `Station -> LoopTake -> Loop` preserves per-entity anchors, lengths, audio cursors, MIDI event cursors, and automation origins. Common mapped elapsed time never becomes a shared loop cursor.
- Epoch and `NoSync` invalidation clear remote authority and generation gates without moving local cursors. `M`, `2M`, `3M`, and non-divisor loops recover against their own geometry and offsets.
- Timing observations and desired/applied receipts cross threads as coherent fixed values. Callback paths remain free of new locks, allocation, waits, formatting, I/O, and unbounded hierarchy traversal.
- Diagnostics have one bounded Coordinator owner, logarithmic suppression after first occurrence, and off-callback Scene presentation.

The cleanup also removed stale APIs and an obsolete uncompiled pseudo-test, consolidated equivalent correction paths, restored signed transport-offset persistence, separated value headers/runtime bodies, made remote BPI mandatory for remote authority, reconciled timing vocabulary, and rewrote the three NINJAM guides to match the implemented system.

## Review closure

Every B001-B017 batch has implementation evidence, canonical evidence, an independent review, and a final closeout. The closure audit found no unresolved batch blocker and no cleanup-range path without an approved owner or amendment. Earlier batch rejections are retained as history and are superseded by their correction approvals.

Notable final approved reviews:

- B006+B007 complete desired-state production/consumption: `b3f2beb`.
- B009 common-map ownership and saturation evidence: `3e50698`.
- B014 bounded diagnostics after correction: `6150f5f`.
- B015 vocabulary cleanup: `b055a20`.
- B016 documentation reconciliation: `65709c1`.
- B017 obsolete-test deletion: `2b76c7c`.

## Final executable evidence

All builds used the authoritative local `.vscode/tasks.json` executable and arguments through `.github/skills/builder/invoke-msbuild.ps1`, after rereading the task file and `doc/build.md` before each invocation.

| Check | Timestamp on 2026-09-05 (UTC-06:00) | Outcome |
| --- | --- | --- |
| Debug x64 solution incremental Build | 22:26:21.711-22:26:22.633 | Passed |
| Release x64 solution incremental Build | 22:26:44.010-22:26:44.936 | Passed |
| Debug x64 native-test project Build | 22:27:10.787-22:27:11.731 | Passed |
| Full Debug native suite | 22:27:27.647-22:27:57.402 | 844 tests/117 suites; 843 passed; one expected skip; zero failed |

The sole skip was `MidiDevice.OpensPreferredDeviceWhenAvailable`, which requires opt-in physical MIDI hardware. The B009 callback saturation benchmark ran and reported `timing_accepted=true`.

The initial Release link found that the prebuilt LTCG NJClient archive was locked to an older v145 compiler revision. Commit `2dc6cf8` refreshes the same x64 Release `/MD` static library from matching single-header source, without LTCG. Its header is byte-identical, required public symbols are present, and both Release consumers link. This is a build-compatibility repair with no source/API change and is intended to preserve behavior; Release runtime execution was not performed.

## Final diff and hygiene

At the verified implementation tip:

| Range | Files | Insertions | Deletions | Added | Modified | Deleted | Renamed | Binaries |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `master...2dc6cf8` | 256 | 23,827 | 1,716 | 127 | 125 | 0 | 4 | 20 |
| `2e770b..2dc6cf8` | 95 | 7,390 | 3,625 | 22 | 72 | 1 | 0 | 1 |

Both ranges pass `git diff --check`. There are no conflicts, nonignored untracked files, submodules, local task files, generated source, build output, logs, or credentials in the diff. Cleanup added no HUD/resource/shader, generic JSON/schema, password, or work-directory collateral.

All cleanup-added compilation units are registered, and B017 leaves no stale entry. No project compilation entry points to a missing physical source. Inherited physical-source/project and filter-presentation discrepancies predate the cleanup gate and do not prevent either solution configuration from building. They are accepted for this merge rather than expanded into an unrelated metadata sweep.

## Accepted risks and limitations

The following are explicit; none is being represented as tested or fixed:

- Live NINJAM join/follow/`Stay local`/`NoSync`, prompt, disconnect, physical loss, retry, reconnect, malformed timing, and differing-rate tail scenarios were not run.
- Saved/default `.jam` interactive launch and older-binary resave were not run. Missing `transportoffsetloopfrac` still defaults to zero; finite values clamp to `[-1,1]`; no schema/version guard exists; older binaries can silently lose the field on resave.
- `.jam` continues to serialize the NINJAM password and work directory under the human-rejected F-049/F-050 cleanup proposals.
- Generic JSON size/depth and unavailable upstream NJClient user/channel/work limits were not hardened; this review is not a broad security certification.
- Normal/verbose live trace volume, complete MIDI/overlay behavior, enabled export, and physical DAC-to-ADC loopback were not exercised.
- Export latency compensation remains disabled, and integer source mapping may repeat or skip samples.
- No configured production hierarchy maximum exists; the 3,072-entity B009 run is a saturation benchmark, not a declared maximum.
- F-020's HUD registration debt and inherited Visual Studio filter/presentation discrepancies remain.
- Release native tests were built but not run because the authoritative local task file provides no Release test-run command.
- ASan, race tooling, profiler, page heap, and Application Verifier were unavailable/not executed.

## Human merge decision

The review recommends merging with the accepted risks above. Before deciding, the human should confirm whether the unavailable live NINJAM/audio-hardware scenarios are acceptable for this merge or should be exercised separately.

No merge, rebase, push, or modification of `master` has been performed. The branch is intentionally stopped at this decision gate.
