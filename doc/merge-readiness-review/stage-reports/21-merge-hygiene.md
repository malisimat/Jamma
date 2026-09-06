# Stage 21 — Build, Test, and Merge Hygiene

> **Status: complete — READY WITH ACCEPTED RISKS.** The initial design below is retained as historical context. Its deferred execution contract has now been completed for every available automated and static check. B001-B017 are independently approved, the final builds and native suite are green, the two-range audit is clean, and remaining manual/tooling limitations are explicit. The branch now stops at the required human merge-decision gate.

## Initial design assignment (historical)

Stage 21 owns build/test command validity, Visual Studio project membership, local-only artifacts, formatting, generated/binary assets, and final changed-file hygiene. The governing Phase 4 specification requires exact commands, evidence locations, limitations, and a final changed-file audit, and explicitly says that initial reconciliation designs these checks while post-cleanup execution updates this same report (`doc/merge-readiness-review/phase-4-reconciliation-and-merge-evidence.md:7`–`:19`).

The initial assignment was bounded to planning and static grounding. It did not re-review production semantics, implement cleanup, execute builds/tests/manual scenarios, inspect post-cleanup output, create batch reviews, or write `merge-brief.md`. That restriction applied to the design pass preserved below; the final authorized execution is recorded at the end of this report.

The protected timing glossary is a hard constraint on every later check. In particular, master phase is not a per-loop cursor, common mapped elapsed time is not a shared loop cursor, scene position is not wrapped Timer geometry, and follow policy is not a coordinate system (`doc/merge-readiness-review/00-scope-and-inventory.md:17`–`:41`). Build success or a clean diff cannot waive the required unequal-length, intentional-offset, reconnect, and `NoSync` evidence.

## Coverage

### Inputs read

- Repository policy and build authority: `AGENTS.md`, local `.vscode/tasks.json`, `doc/build.md`, `.github/skills/builder/invoke-msbuild.ps1`, and `.github/skills/builder/builder.ps1`.
- Review authority: the main plan, Phase 4 specification, `00-scope-and-inventory.md`, all three completed phase packets, `decisions.md`, `findings.md`, `cleanup-backlog.md`, and `verification-matrix.md`.
- Static merge-hygiene surfaces: `Jamma.sln`; the Jamma, JammaLib, and JammaLib_Tests project/filter files; `.gitignore`; `Jamma/resources/ResourceList.txt`; the changed shader/texture inventory; and current project membership around timing tests.

### Commands and queries used for design grounding

- Numbered `Get-Content` reads for the files above.
- `rg --files` and focused `rg -n` queries for project membership, resource registration/copy behavior, build-wrapper references, generated-file policy, and formatting configuration.
- Read-only Git inventory queries: `git status --short --branch`, `git check-ignore -v .vscode/tasks.json`, `git ls-files`, `git diff --name-status master...HEAD`, `git diff --numstat master...HEAD`, and a pre-cleanup `git diff --check master...HEAD` probe.
- No build, test executable, formatter, asset generator, cleanup verifier, or manual Jamma/NINJAM scenario was run.

### Static scope established

- The production baseline contains 199 changed files, including 35 native-test files, 25 app-resource files/19 binaries, six repository-tooling files, and changed app/library/test project files (`00-scope-and-inventory.md:49`–`:69`, `:95`–`:100`). Review-only commits later increase the raw branch comparison, so the final audit must report both the complete `master...HEAD` range and a production/approved-cleanup view; it must not misclassify review artifacts as production changes (`00-scope-and-inventory.md:169`–`:177`).
- Jamma, JammaLib, and JammaLib_Tests are explicit solution members at `Jamma.sln:6`, `:11`, and `:20`. The native project currently registers the active NINJAM timing suites at `test/JammaLib_Tests/JammaLib_Tests.vcxproj:203`–`:216`; the obsolete `RemotePhaseDiscipline_Tests.cpp` is deliberately not a project member, matching F-037's current evidence (`findings.md:450`–`:459`).
- The resource registry contains the live neutral `trigger_back` at `Jamma/resources/ResourceList.txt:76`. Ten green/red variants are unregistered but copied by the app project (`findings.md:241`–`:250`); the later G3-1 timing-only scope decision excludes F-020 from Phase 4 cleanup. The final audit must therefore confirm that no HUD resource is changed by a timing batch.
- `.vscode/tasks.json` is local-only by policy (`AGENTS.md:29`–`:33`) and ignored at `.gitignore:17`; it is not tracked. Build outputs and dependencies are ignored at `.gitignore:3`–`:5` and `:18`–`:20`.

### Deliberate exclusions

- HUD, VST3, window/tooling behavior, unrelated MIDI/resources, generic JSON limits, and upstream NJClient bounds remain in the merge but outside timing-only cleanup, as accepted at `decisions.md:7`–`:16`. Stage 21 checks their retained file/project hygiene; it does not redesign them.
- F-049/F-050 are rejected (`decisions.md:34`–`:35`), so no final audit may treat absent credential-redaction/work-directory cleanup as an omission or silently add it.
- No Clean/Rebuild is planned by default. Policy requires incremental affected targets and an absolute `SolutionDir` with exactly one trailing backslash for direct projects (`AGENTS.md:24`–`:31`; `doc/build.md:23`–`:34`).

## System understanding

### Command authority and exact future commands

The local task file is authoritative for the installed executable and explicit build arguments (`AGENTS.md:29`). On this machine it selects `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`, Debug/Release x64 solution builds, a Debug x64 JammaLib build, and Debug x64 test-project builds (`.vscode/tasks.json:23`–`:78`). Repository policy adds one mandatory transport rule: do not invoke that executable directly; pass the task executable and arguments through `invoke-msbuild.ps1`, which creates a child environment with one canonical `Path` (`AGENTS.md:30`; `.github/skills/builder/invoke-msbuild.ps1:12`–`:43`).

Immediately before **each** future build/test, reread `.vscode/tasks.json` and `doc/build.md`. If the task file is absent, changed, or lacks the needed command, stop and record the limitation rather than guessing. The following commands are therefore exact for the current local task file, but must be re-derived if it changes:

```powershell
$repoRoot = (Resolve-Path -LiteralPath '.').Path
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$invokeMsbuild = Join-Path $repoRoot '.github\skills\builder\invoke-msbuild.ps1'
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

# Affected JammaLib production batch: incremental Debug x64.
& $invokeMsbuild -Executable $msbuild -Arguments @(
    (Join-Path $repoRoot 'JammaLib\JammaLib.vcxproj'),
    '/m', '/t:Build', '/p:Configuration=Debug', '/p:Platform=x64',
    $solutionDirArg
)

# Affected native-test batch: incremental Debug x64, including references.
& $invokeMsbuild -Executable $msbuild -Arguments @(
    (Join-Path $repoRoot 'test\JammaLib_Tests\JammaLib_Tests.vcxproj'),
    '/m', '/t:Build', '/p:Configuration=Debug', '/p:Platform=x64',
    $solutionDirArg
)

$testsExe = Join-Path $repoRoot 'test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe'

# Per-major-refactor prerequisite: replace the placeholder with the approved one
# or two closest P1-P10 test names; do not run an umbrella prerequisite batch.
& $testsExe --gtest_filter='ApprovedSuite.ApprovedTest:ApprovedSuite.SecondApprovedTest'

# End-of-cleanup full native suite.
& $testsExe

# Final retained-branch integration builds. These reproduce the current local
# incremental solution task arguments through the required wrapper.
& $invokeMsbuild -Executable $msbuild -Arguments @(
    (Join-Path $repoRoot 'Jamma.sln'),
    '/m', '/t:Build', '/p:Configuration=Debug', '/p:Platform=x64'
)
& $invokeMsbuild -Executable $msbuild -Arguments @(
    (Join-Path $repoRoot 'Jamma.sln'),
    '/m', '/t:Build', '/p:Configuration=Release', '/p:Platform=x64'
)
```

The test-only no-reference task at `.vscode/tasks.json:70`–`:78` is allowed only after a successful current JammaLib build and only when a batch changed test sources without changing JammaLib; otherwise use the reference-building command above. Rebuild remains a recovery action only for demonstrated stale-artifact failures, consistent with `doc/build.md:25`–`:34`.

The currently recorded executable baseline is an incremental JammaLib_Tests build followed by 821 passes out of 822 tests, with `MidiDevice.OpensPreferredDeviceWhenAvailable` skipped for hardware (`verification-matrix.md:3`; `phase-packets/phase-2.md:155`–`:162`). It predates all accepted cleanup and does not satisfy any batch or final gate.

### Evidence locations and pass/fail contract

For each approved batch, the implementation owner records the exact command, pre-run task/build-guide read, commit hash, timestamp, exit code, selected test filter, pass/fail/skip counts, and any limitation in that batch's row in `verification-matrix.md`. The independent reviewer cites the same evidence in `batch-reviews/B###.md`. Stage 21's deferred execution section records the final solution builds, full native suite, static audits, final commit hashes, and manual-evidence links. Build products, `.tlog`, binaries, vcpkg state, and local tasks are never committed as evidence.

A major refactor normally starts only after its one or two closest P1–P10 prerequisites pass in the prerequisite commit accepted for that batch. Under the human-approved characterization-first exception, a truthful test that necessarily depends on the batch's approved production behavior may instead be committed, built, and recorded with its exact expected pre-fix failure before bounded implementation. It and every focused check must be green before independent approval or dependent work. The governing matrix lists P1–P10 and their exact targets at `verification-matrix.md:76`–`:91`; the accepted rule is reiterated in `decisions.md`. Any other failed prerequisite, build, test, static audit, or required manual scenario reopens the owning batch and every dependent batch; it is not documented as a harmless limitation.

### Project-membership audit design

At each batch and final audit:

1. Diff all `.sln`, `.vcxproj`, and `.vcxproj.filters` changes in the batch and in `master...HEAD`. Confirm every added/renamed/removed `.cpp`, `.h`, resource, and test has the intended real-project membership and no stale include remains. `.vcxproj` is the build authority; `.filters` must mirror IDE presentation but cannot substitute for membership.
2. Map changed production paths to the smallest project using `doc/build.md:26`–`:31`. Confirm no timing cleanup adds an `engine -> ninjam` dependency and no rejected/out-of-scope file is pulled into a project accidentally.
3. For tests, enumerate `test/JammaLib_Tests/src/**/*.cpp` and compare with `<ClCompile Include=...>` entries. F-037 must remove `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp`; current-owner NINJAM, Timer, LoopTake, MIDI, and metronome suites must remain registered (`test/JammaLib_Tests/JammaLib_Tests.vcxproj:157`–`:216`). New P1–P10 tests are not valid evidence until registered and compiled.
4. Confirm all retained HUD textures, including F-020's ten unregistered variants, are unchanged in the cleanup-only range. Do not remove, rename, or re-register HUD assets in a timing batch.

Useful exact read-only queries:

```powershell
git diff --name-status master...HEAD -- '*.sln' '*.vcxproj' '*.vcxproj.filters'
git diff --name-status master...HEAD -- 'Jamma/**/*.cpp' 'JammaLib/**/*.cpp' 'JammaLib/**/*.h' 'test/JammaLib_Tests/**/*.cpp'
rg -n 'ClCompile Include=|ClInclude Include=|Image Include=|Content Include=' Jamma/Jamma.vcxproj JammaLib/JammaLib.vcxproj test/JammaLib_Tests/JammaLib_Tests.vcxproj
rg -n 'RemotePhaseDiscipline_Tests|NinjamTimingIntegration_Tests|LoopTakeTiming_Tests' test/JammaLib_Tests/JammaLib_Tests.vcxproj test/JammaLib_Tests/JammaLib_Tests.vcxproj.filters
```

### Local-only and generated/binary artifact audit design

- Run `git status --porcelain=v2 --branch --untracked-files=all`, `git ls-files --others --exclude-standard`, `git check-ignore -v .vscode/tasks.json`, and `git ls-files .vscode/tasks.json`. The task file must remain ignored/untracked; no local IDE state, build output, dependency tree, logs, credentials, or scratch evidence may enter the diff.
- Run `git ls-files | rg '(^|/)([Bb]in|[Oo]bj|x64|vcpkg_installed)/|\.(exe|dll|pdb|ilk|tlog)$'`. Investigate every match; do not assume an ignored-looking tracked artifact is acceptable.
- Re-enumerate added/modified binaries with `git diff --numstat master...HEAD` (binary rows show `- -`) and `git diff --summary master...HEAD`. For every retained binary/shader/resource, link its feature provenance, project/registry membership, and runtime consumer. The inventory explicitly forbids classifying a file as generated solely by extension (`00-scope-and-inventory.md:95`–`:100`).
- If a generator is introduced by an approved batch, record generator source/version/command and prove deterministic output or document why the checked-in output is authoritative. No generator exists in current Stage 21 scope, so no regeneration command is invented.

### Formatting and final changed-file audit design

No repository `.editorconfig`, `.clang-format`, or `.clang-tidy` file was found. Therefore the final gate does not invent a bulk formatter. It requires compiler-clean affected builds, focused human style review against `AGENTS.md:59`–`:67`, and zero output from both:

```powershell
git diff --check master...HEAD
git diff --check $approvedPhase4GateCommit..HEAD
```

The pre-cleanup probe currently reports trailing blank lines at `test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp:650` and `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp:156`. The second is consumed by accepted F-037 deletion. The first should be corrected only by the approved batch that already owns that test file (normally the production-faithful P1–P4 test work), avoiding a collateral formatting sweep.

Before final synthesis, capture the immutable batch-approval gate commit and audit both the complete merge and cleanup-only ranges:

```powershell
# Run this at the approval gate and persist the resulting 40-hex hash in the
# cleanup backlog/Phase 4 packet; restore that literal value in later sessions.
$approvedPhase4GateCommit = (git rev-parse HEAD).Trim()

git status --porcelain=v2 --branch --untracked-files=all
git diff --name-only --diff-filter=U
git diff --stat master...HEAD
git diff --summary master...HEAD
git diff --name-status master...HEAD
git diff --stat $approvedPhase4GateCommit..HEAD
git diff --name-status $approvedPhase4GateCommit..HEAD
git log --oneline --decorate master..HEAD
```

Every cleanup-range path must map to one approved batch owner or to the batch's verification/artifact updates. Compare actual files against each batch's owned files and prohibited collateral list; rejected F-049/F-050 and retained out-of-scope feature surfaces must show no silent cleanup. Review deletions/renames separately, inspect submodule state if any appears, rerun project/resource membership queries, and require a clean worktree except for explicitly named review artifacts. Finally reconcile `master...HEAD` against the approved glossary and manual evidence; hygiene checks supplement rather than replace the semantic verification matrix.

## Candidate findings

### S21-01 — Remove two trailing blank lines within their owning timing-test batches

- Stage / reviewer: Stage 21 — Build, test, and merge hygiene initial design.
- Scope reviewed / exclusions: pre-cleanup whitespace only; no bulk formatting and no production-semantic review.
- Severity: follow-up merge hygiene.
- Evidence: `git diff --check master...HEAD` reports a new blank line at EOF in `test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp:650` and in obsolete `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp:156`.
- Why it matters: a non-empty `git diff --check` prevents a clean final changed-file audit, but a standalone formatting sweep would create avoidable collateral.
- Recommended disposition: remove the integration-test blank line in the approved F-038/P1–P4 test batch that owns the file; let accepted F-037 deletion consume the obsolete-test instance. Do not create a cross-repository formatting batch.
- Protected timing concepts affected: none; test assertions and test registration must remain otherwise unchanged except as separately approved.
- Verification: `git diff --check master...HEAD` and cleanup-range `git diff --check` return no output; affected test project builds; registered replacement tests pass.
- Human decision: accepted within the owning timing-test work; both final diff-check ranges are clean and no standalone formatting sweep was needed.

No separate candidate is raised for build tooling, local tasks, project membership, or assets. F-004 already retains the wrapper/tooling contract, G3-1 leaves F-020's copied textures unchanged and outside cleanup, and F-037 owns the obsolete uncompiled test. Static design inspection found no evidence that `.vscode/tasks.json` is tracked or that the current active timing suites are missing from the native project.

## Handoffs

- **Phase 4 integrator / cleanup backlog:** attach S21-01 to the existing F-038 test batch and F-037 deletion rather than creating a broad formatting batch. Add the exact Stage 21 command/evidence contract to every batch that builds/tests, and reserve the final solution/full-suite/diff audit for the last approved batch or final verification step.
- **Stage 20:** ensure every impact row names its affected Visual Studio project, applicable P1–P10 filter, project/resource membership check, manual scenario, and whether Debug-only or Debug+Release integration coverage is required. F-020 requires a no-collateral-change audit only; engine/test batches require JammaLib_Tests.
- **Implementation owners:** record the approval-gate commit before source movement; use the assigned files only; update project/filter membership in the same batch as file adds/removes; never edit local `.vscode/tasks.json` or commit build products.
- **Independent batch reviewers:** compare actual batch paths with owned/prohibited lists, cite verification-matrix evidence, and reject unapproved protected-timing changes or silent F-049/F-050/out-of-scope changes even when builds pass.
- **Final lead:** after all batches pass independent review, return to the deferred section below, perform the full current-range audits, then use the resulting evidence when writing (not before) `merge-brief.md` as required by `phase-4-reconciliation-and-merge-evidence.md:21`–`:38`.

## Uncertainties

- Exact GoogleTest suite/test names for newly approved P1–P10 prerequisites do not yet exist in a settled cleanup diff. Each approved batch must replace the placeholder filter with one or two concrete names before implementation; an unfiltered full suite is not a substitute for the prerequisite gate.
- The eventual batch-approval commit is not yet known. Capture it at the human gate so cleanup-only audit commands have a stable base.
- A Release native-test run is not present in the local task file. The planned final Release command therefore builds the retained solution but does not claim Release test execution. If Release native tests become required, the local task file must first supply an applicable command or the limitation must return to the human gate.
- Hardware/network/manual evidence remains environment-dependent. The one historical hardware skip must be reported by exact name; new skips/failures require a gate decision, not automatic acceptance.
- Phase 2's 821/822 run is the latest executed baseline, but accepted test additions/deletions will change the final count. Success is determined by registered intended tests and zero unexpected failure/skip, not by preserving 821 numerically.
- Review and phase artifacts make raw `master...HEAD` larger than the 199-file production baseline. The final report must show both views and explicitly account for every review-only path.

## Final post-cleanup execution and results

### Immutable baselines and batch closure

- Branch: `bugfix/align-remote-join`.
- `master` and merge base: `4941b780f7ff5a46f742167d79338e3ab592a565`.
- Human-approved Phase 4 gate: `2e770b743d9f2466b2edafff5c92faf139d93108`.
- Verified implementation tip: `2dc6cf8d3feefdcb4a7fcdade518f05d0f9b3b6c`.
- B001-B017 are implemented, canonically evidenced, independently approved, and finally closed. Earlier rejections remain useful history and are superseded by their recorded correction reviews. The independent closure audit found no unresolved batch blocker and mapped every cleanup-range path to an approved batch, evidence update, review, or human amendment.

### Final build and native-test evidence

Immediately before every build or native-test invocation, the local `.vscode/tasks.json` and `doc/build.md` were reread. All MSBuild invocations used the task-selected Visual Studio 18 executable through `.github/skills/builder/invoke-msbuild.ps1`; direct-project invocation used the absolute `SolutionDir` with exactly one trailing backslash.

| Check | Time on 2026-09-05 (UTC-06:00) | Result |
| --- | --- | --- |
| Debug x64 solution incremental Build | 22:26:21.711-22:26:22.633 | Passed; Jamma, JammaLib, JammaLib_Tests, and LatencyMeasure produced their Debug outputs |
| Release x64 solution incremental Build | 22:26:44.010-22:26:44.936 | Passed; all four solution projects produced their Release outputs |
| Debug x64 JammaLib_Tests incremental Build | 22:27:10.787-22:27:11.731 | Passed with project references and canonical absolute `SolutionDir` |
| Full Debug native suite | 22:27:27.647-22:27:57.402 | Exit 0; 844 tests from 117 suites, 843 passed, one expected hardware-dependent MIDI skip, zero failed |

The sole skip was `MidiDevice.OpensPreferredDeviceWhenAvailable`; the test itself reported that `JAMMA_ENABLE_MIDI_HARDWARE_TESTS=1` is required. The inherited B009 saturation benchmark executed inside the full suite and reported `timing_accepted=true`. The authoritative local tasks contain no Release native-test execution command, so the successful Release test executable build is recorded but no Release test run is claimed.

### Release dependency recovery

The first Release incremental Build failed at link with `C1047`/`LNK1257`: the vendored x64 Release `njclient.lib` contained LTCG intermediate code from a different v145 compiler revision. A justified one-time Release solution Rebuild refreshed Jamma-owned objects but reproduced the failure because NJClient is a prebuilt archive rather than a solution project.

Archive metadata identified the matching source repository and its `origin/feature/single-header` revision `d261c9156d151ce91ce9226b90af1757faf734cd`. Stage 21 rebuilt that exact pimpl-compatible source as an x64 Release `/MD` static library with the installed compiler and whole-program optimization disabled for the archive. Validation established:

- the generated public `njclient.h` is byte-identical to Jamma's vendored header;
- archive members are x64 and carry `RuntimeLibrary=MD_DynamicRelease` plus `MSVCRT` directives;
- the required out-of-line NJClient API symbols, including timing and user/status getters, are present;
- no LTCG marker remains, removing the compiler-minor lock while retaining normal Release optimization;
- both Jamma and JammaLib_Tests link against the refreshed static archive.

The compatible archive is commit `2dc6cf8`; no NJClient source or API changed in this repository. The refresh is intended to be behavior-preserving, but Release runtime execution was not performed. Its SHA-256 at verification was `28251FC342A28C0403791EAD0BB0F1C9F6F6BEBCB249A686E073B6E05B2B6BB7`.

### Final static and changed-file audit

At verified implementation tip `2dc6cf8`:

| Range | Files | Insertions | Deletions | Added | Modified | Deleted | Renamed | Binary rows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `master...2dc6cf8` | 256 | 23,827 | 1,716 | 127 | 125 | 0 | 4 | 20 |
| `2e770b..2dc6cf8` | 95 | 7,390 | 3,625 | 22 | 72 | 1 | 0 | 1 |

- `git diff --check master...HEAD` and `git diff --check 2e770b..HEAD` returned no output.
- No conflicted path or nonignored untracked file remained. The branch was 40 commits ahead and zero behind its configured upstream at this checkpoint.
- The full range contains 285 commits including three merges; the cleanup range contains 121 commits and no merges.
- The four full-range renames are the approved `TimingQuantiser` to `Quantiser` pair and `GuiPopupHost` to `GuiPopupManager` pair. B017's obsolete test is the cleanup-only deletion. The cleanup-only binary is the Stage 21 NJClient compatibility refresh; the other 19 full-range binary rows are approved pre-gate HUD TGA additions.
- Cleanup introduced no HUD/resource/shader collateral, password/work-directory persistence change, generic JSON/schema change, submodule, generated source, local task file, build output, or credential/log artifact.
- Every cleanup-added production/test unit is registered in its `.vcxproj` and filters. B017's deleted uncompiled test has no stale project/filter entry.

The project/filter audit also found inherited IDE metadata debt that predates the approval gate: physical duplicate/unused test files, stale/duplicate filter entries, and JammaLib project/filter presentation mismatches. Five compiled tests lack filter entries; three of those were branch-added before the cleanup gate (`StationVisualState_Tests.cpp`, `NinjamAudioTimingCommand_Tests.cpp`, and `NinjamTimingIntegration_Tests.cpp`). No compiled entry points to a missing source, the cleanup itself registered its new units correctly, and both final solution configurations build. These presentation discrepancies are explicitly accepted for this merge rather than expanded into a broad project/filter cleanup.

### Manual and tooling evidence not executed

No claim is made for the following environment-dependent checks:

- live NINJAM Continuous/Block/`NoSync`/`Stay local`, prompt, disconnect, physical loss, retry, reconnect, malformed timing, or differing-rate tail scenarios;
- a saved/default `.jam` interactive launch or older-binary resave;
- normal-versus-verbose bounded live diagnostics traces;
- complete live remote-grid/local-inference/MIDI/overlay behavior;
- enabled export and physical DAC-to-ADC loopback;
- physical MIDI hardware execution;
- ASan, race tooling, profiler, page heap, or Application Verifier.

Automated tests cover the underlying deterministic contracts, but they do not convert these unavailable manual/tooling checks into passes. The accepted residuals also remain: F-020 resource-registration debt; F-045 zero-default/no-schema downgrade behavior; F-049/F-050 password and work-directory persistence; unhardened generic JSON/upstream NJClient bounds without broad security certification; disabled export latency compensation and integer repeat/skip behavior; and the B009 saturation benchmark's explicit ceiling rather than a configured production maximum.

## Conclusion

Stage 21 is complete. All approved cleanup batches are closed, current-content Debug and Release solution builds pass, the Debug native suite has no failure or unexpected skip, static/two-range audits are clean, and the merge brief exists. No technical cleanup blocker remains inside the approved scope.

Final classification: **READY WITH ACCEPTED RISKS**, subject to the human merge decision. This is intentionally not an unqualified `READY` because the accepted persistence/resource/project-metadata risks and unavailable live/manual/tooling evidence remain real and explicit.
