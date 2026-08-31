# Stage 11 — Numerical, boundary, and clock domains

## Assignment

- **Primary ownership:** units and coordinate annotations, conversions, rounding, overflow, sign changes, invalid inputs, long duration, wraparound, and precision in the changed NINJAM timing/sync path.
- **Explicit exclusions:** timing-policy and state-transition intent owned by Stages 9–10; cross-thread race/publication proof owned by Stage 7; callback cost owned by Stage 8; HUD, VST3 parity, window placement, and tooling retained but excluded by the Phase 1 human decision.
- **Required inputs read:** `AGENTS.md`; `merge-readiness-plan.md`; `phase-2-runtime-safety-and-correctness.md`; `decisions.md`; `00-scope-and-inventory.md`; `phase-packets/phase-1.md`; `findings.md`; `cleanup-backlog.md`; `verification-matrix.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and completed Stage 7–9 reports.
- **Report status:** investigation only at HEAD `a202d27`. No production source, test, canonical finding, decision, backlog, or verification artifact was edited.

## Coverage

### Commands and queries used

The investigation used read-only `rg`, `Get-Content`, `git status --short`, `git log --oneline`, `git log -S`, `git show --stat`, and `git blame -L`. Representative queries included:

- `rg -n "AbsoluteSamplePos|SceneSamplePos|LocalBlockStartSample|AudioBlockStartSample|PhaseObservationSample" JammaLib test`
- `rg -n "ScaleSampleRate|IntervalSampsFromTempo|SignedCircularDifference|PositiveModulo|MapRemoteElapsedToLocal" JammaLib test`
- `rg -n "static_cast|llround|fmod|numeric_limits|uint32|uint64|unsigned long|long long" JammaLib/src/{audio,engine,midi,ninjam,utils}`
- `git blame -L 69,85 JammaLib/src/utils/Timer.h`
- `git blame -L 40,58 JammaLib/src/utils/Timer.cpp`
- `git blame -L 45,174 JammaLib/src/ninjam/NinjamTiming.h`
- `git blame -L 216,238 JammaLib/src/ninjam/NinjamTimingCoordinator.cpp`
- `git blame -L 437,453 JammaLib/src/audio/AudioHost.cpp`

The 32-bit wrap horizons were calculated from `2^32 / sampleRate`: about 27:03:11 at 44.1 kHz, 24:51:18 at 48 kHz, 12:25:39 at 96 kHz, and 06:12:49 at 192 kHz. Arithmetic counterexamples below were checked directly against the formulas in the current headers; no source was executed or modified.

### Files and subsystems actually covered

- Local Timer widths and phase advancement: `JammaLib/src/utils/Timer.h/.cpp` and `Timer_Tests.cpp`.
- Source-rate to device-rate conversion, tempo conversion, circular differences, boundary projection, and validity: `JammaLib/src/ninjam/NinjamTiming.h`, `NinjamTiming_Tests.cpp`, and `NinjamTimingIntegration_Tests.cpp`.
- Observation and command clock fields: `NinjamTimingObservationMailbox.h`, `NinjamTimingTracker.h/.cpp`, `NinjamTimingCoordinator.h/.cpp`, `NinjamAudioTimingCommand.h`, and `AudioHost.cpp`.
- Source/scene mapping: `NinjamLoopAlignment.h`, AudioHost map creation/rebase sites, and LoopTake restore sites.
- Intentional modular MIDI time: `midi/MidiBlockTiming.h`, `MidiClockAnchor.h`, `MidiTimestampMapper.cpp`, and `MidiTimestampMapper_Tests.cpp`.
- Normalized local transport offsets: `utils/MathUtils.h`, Scene publication, AudioHost sample conversion, and Station/LoopTake use sites.

### Deliberate exclusions

- S09-03 owns whether the initial join compares phase at the correct instant. This report owns only width, zero-value representation, and arithmetic at that seam.
- S07-03 owns the lack of a coherent compound Timer snapshot. This report specifies the necessary numeric domains but does not duplicate the concurrency finding.
- S09-01/S09-02 and Stage 10 own command ordering, generation/session epochs, and recovery. Sequence counter wrap is noted below, but no state-policy prescription is made.
- Upstream NJClient implementation is absent and protected from edits. Its signed `int` position/length outputs are only checked at Jamma's conversion boundary.
- Generic DSP sample arithmetic, file duration fields, VST time structures, and unrelated UI geometry are outside the approved NINJAM timing/sync cleanup scope.

## System understanding

The changed path deliberately uses several different coordinates. NJClient supplies a **wrapped remote source-rate interval position** and length. `ToDeviceTiming` converts those to a **wrapped remote device-rate interval position** and length. AudioHost separately records both a **Timer-absolute local anchor** and a **monotonic device-audio block counter**. Timer also owns a wrapped local master phase and a separate monotonic scene coordinate. Accepted corrections establish a source/scene map whose common mapped elapsed amount is then reduced independently by each audio/MIDI entity's own loop length and anchor.

Those are not interchangeable: local is not remote; Timer absolute is not the device counter; neither is the scene coordinate; master phase is not an entity cursor; and follow policy is not a coordinate. The formulas are generally explicit about these distinctions. Three defects arise where representation or rounding loses them: Timer absolute time is only 32-bit on Windows, zero is used as a missing-value sentinel in two valid zero-based clocks, and separately rounded interval/position scaling can manufacture an early wrap.

### Conversion and boundary table

| Operation / exact site | Input domain | Output domain | Formula | Valid bounds / overflow analysis | Rounding and boundary rule | Assessment |
| --- | --- | --- | --- | --- | --- | --- |
| NJClient observation (`NinjamConnection.cpp:717`–`:724`, `:929`–`:936`) | Signed upstream source-rate position/length; rate; float BPM | Unsigned remote source-rate snapshot | Negative position becomes zero; positive length/rate cast to `unsigned int`; shared validity checks length/rate and BPM/BPI bounds | Upstream values are `int`, so successful casts fit `uint32`; NaN BPM fails ordered bounds | No rounding; negative phase clamps to zero | Numerically bounded at ingress; upstream coherence is S07-01 |
| Sample-rate scalar (`NinjamTiming.h:60`–`:71`, commit `d0d208e`) | `uint32` samples and source/target rates | `uint32` target-rate samples | `(uint64(samples) * targetRate + sourceRate/2) / sourceRate`, saturating at `UINT32_MAX` | Product of two `uint32` values plus half-rate fits `uint64`; zero-rate helper inputs return the unconverted value, although production `ToDeviceTiming` rejects them first | Nearest integer, positive half up | Scalar is safe; coupled phase/length use is S11-03 |
| Remote source timing to device timing (`NinjamTiming.h:140`–`:174`, commit `d0d208e`) | Wrapped source phase/length plus source/device rates | Wrapped device phase/length | Scale length; scale `(position % sourceLength)` independently; then `% scaledLength` | Each scalar is bounded, but the two rounded results do not preserve `position < length` at a downsampled tail | Each scalar nearest/half-up; final modulo converts `scaledPosition == scaledLength` to zero | Violated at tail; S11-03 |
| Tempo to interval (`NinjamTiming.h:126`–`:138`, commit `d0d208e`) | Float BPM, `uint32` BPI/rate | `uint32` interval samples | `rate * 60 * BPI / BPM`, saturating high | Finite positive production BPM is already bounded by `IsValidRemoteTiming`; the helper itself does not reject NaN/Inf | Nearest integer via `+0.5`, positive half up | Production path bounded; public helper invalid-input gap is S11-04 |
| Circular phase delta (`NinjamTiming.h:74`–`:91`) | Two wrapped/possibly unnormalized `uint32` phases and `uint32` interval | Signed shortest delta in samples | Normalize each modulo length, subtract in `int64`, adjust across half interval | With `uint32` length, result is within `[-floor(L/2), ceil(L/2)]`; zero length returns zero | Exact even half-interval tie is deliberately positive | Safe and covered at `NinjamTiming_Tests.cpp:74`–`:128`, `:188`–`:199` |
| Delayed boundary projection (`NinjamTiming.h:100`–`:123`, commit `b41b5a8`) | Wrapped remote device phase; two monotonic device-audio sample coordinates | Remote device phase at callback boundary and signed local correction | `remote = (observed + ((boundary-observation) % interval)) % interval` | `uint64` subtraction is guarded by order; addition is below about `2*UINT32_MAX` | Exact modulo; **projection disabled when observation coordinate is zero** | Zero coordinate is conflated with absence; S11-02 |
| Timer tick (`Timer.cpp:40`–`:58`; wrap math from `c9b57eb`) | Wrapped local master phase, 32-bit callback increment, 32-bit seed length/count on Windows | Next wrapped phase and loop count | `total = phase + increment`; `wraps=total/length`; `next=total%length` | `total` is `unsigned long`; it can overflow before division when phase+increment exceeds `UINT32_MAX` | Exact division/modulo after a potentially wrapped intermediate | Included in S11-01 |
| Timer absolute position (`Timer.h:72`–`:82`, commit `c9b57eb`) | 32-bit loop count, seed length, phase | Purported monotonic Timer-absolute sample coordinate | `loopCount * seedLength + phase` | Return and multiplication are Windows 32-bit `unsigned long`; wraps every `2^32` samples regardless of seed length | Exact modulo `2^32`, unintentionally | Violated after hours-long sessions; S11-01 |
| Observation age (`NinjamTimingCoordinator.cpp:216`–`:237`, commit `d6fdb88`) | Timer phase/length and Timer-absolute observation/now anchors | Wrapped local phase at observation instant; `uint64` age | `elapsed=(now-observation)%seed`; subtract elapsed from live phase | Widening occurs after `AbsoluteSamplePos` truncation; zero observation disables projection; separate atomic reads can be incoherent (S07-03) | Exact modulo after width/sentinel checks | S11-01/S11-02; coherence remains S07-03 |
| Remote elapsed to local source elapsed (`NinjamLoopAlignment.h:21`–`:30`) | `uint64` remote elapsed; Windows 32-bit local/remote master lengths | `uint64` monotonic mapped local-source elapsed | Whole intervals times local length plus rounded proportional remainder | Remainder product fits `uint64` for current 32-bit lengths; whole-interval product can overflow only at an extreme multi-million-year horizon | Proportional remainder nearest/half-up | Acceptable residual at current widths |
| Source/scene map (`NinjamLoopAlignment.h:47`–`:90`) | Monotonic `uint64` scene coordinate, signed `int64` source origin, 32-bit lengths | Signed monotonic source coordinate and wrapped source phase | Add mapped elapsed to signed origin; reduce with positive modulo for phase/entity cursor | Cast/add reaches `int64` limits only at an extreme roughly million-year audio horizon; practical lengths fit the modulo cast | Map rounding occurs once per proportional remainder; cursor reduction is exact | Acceptable residual; keep coordinate types/names distinct |
| Scene anchor helpers (`NinjamLoopAlignment.h:93`–`:119`) | `uint64` scene/phase/anchor/length | Wrapped per-entity phase/anchor | Signed subtraction followed by positive modulo | Current scene/loop values are far below `INT64_MAX`; generic signature is wider than signed implementation | Exact modulo | Acceptable under documented production bounds |
| Local offset fraction (`MathUtils.h:17`–`:25`; `AudioHost.cpp:332`–`:350`) | Finite UI/persistence fraction | Normalized loop fraction then signed local sample offset | Preserve exactly `1.0`; otherwise `fmod(f,1)` and lift negative; `llround(fraction*masterLength)` | Non-finite becomes zero before publication; current master length is Windows 32-bit | Nearest integer, halfway away from zero; normalized inputs are nonnegative | Safe; distinct intentional local offset preserved |
| MIDI block timestamp delta (`MidiBlockTiming.h:12`–`:42`) | Modular `uint32` event and block-start device sample timestamps | Signed in-block sample delta / bounded offset | `int32(event - blockStart)` with clamp to block | Correct if compared timestamps differ by less than `2^31` samples; normal event/block distances satisfy that | Intentional two's-complement modular subtraction and clamp | Retain; wrap crossing is tested in `MidiTimestampMapper_Tests.cpp:63`–`:92` |

## Candidate findings

### S11-01 — Widen Timer absolute time before long-session observation projection

- **Stage / reviewer:** Stage 11 — numerical, boundary, and clock domains.
- **Scope reviewed / exclusions:** numeric width and overflow only. S07-03 retains ownership of publishing length/count/phase as one coherent snapshot; S09-03 retains ownership of the initial join-time invariant.
- **Severity:** must fix before merge.
- **Evidence:** On Windows, `unsigned long` is 32-bit. `Timer::AbsoluteSamplePos` returns `unsigned long` and performs `_loopCount * loopLength + _sampOffset` in that type (`JammaLib/src/utils/Timer.h:72`–`:82`, introduced in `c9b57eb`). AudioHost casts the already-truncated result to `uint64` after calling it (`JammaLib/src/audio/AudioHost.cpp:445`–`:452`, `d6fdb88`), and the job-side coordinator repeats that pattern (`JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:216`–`:237`, `d6fdb88`). The Timer-absolute anchor therefore wraps at `2^32` samples—about 24:51:18 at 48 kHz and 06:12:49 at 192 kHz—while the adjacent device counter and scene coordinate remain `uint64`. Separately, `Timer::Tick` adds phase and callback increment in 32-bit `unsigned long` before division/modulo (`Timer.cpp:49`–`:57`); a valid `uint32` seed near the maximum can overflow that intermediate and undercount wraps. Current Timer tests use only small positions (`test/JammaLib_Tests/src/engine/Timer_Tests.cpp:88`–`:104`).
- **Why it matters:** After the first wrap, a fresh Timer observation can appear older/newer in an incompatible epoch than the monotonic device counter. Coordinator's `now >= observation` guard can stop phase projection, corrupt observation-age diagnostics, or later resume with an incorrect modulo difference. Widening only the destination does not recover lost high bits. The `Tick` intermediate also fails its own wrapped-phase/count invariant at large but representable interval lengths.
- **Recommended disposition:** correct. Make the Timer-absolute coordinate and its fallback explicitly `uint64`, promote multiplication/addition and Tick's pre-modulo sum before arithmetic, and keep wrapped phase/seed length separate rather than widening every phase field indiscriminately. Incorporate the widened fields into the coherent transport observation required by S07-03; do not add a second snapshot mechanism or conflate Timer absolute with scene/device coordinates.
- **Protected timing concepts affected:** Timer-absolute local coordinate, monotonic device-audio counter, monotonic scene coordinate, wrapped local master phase, and seed length remain distinct.
- **Verification:** add a Timer test crossing `2^32` without losing monotonic absolute time; a near-`UINT32_MAX` seed/phase Tick test that counts the wrap correctly; distinct sentinel values proving Timer-absolute, device-audio, and scene coordinates are not substituted; and delayed-observation projection on both sides of the former 32-bit horizon. Per the human mandate, land one or two strong regression tests before changing the clock boundary.
- **Human decision:** pending.

### S11-02 — Represent observation validity separately from the valid sample-zero coordinate

- **Stage / reviewer:** Stage 11 — numerical, boundary, and clock domains.
- **Scope reviewed / exclusions:** zero-based clock representation and boundary arithmetic. S09-03 owns when join phase must be captured; Stage 10 owns lifecycle availability.
- **Severity:** must fix before merge.
- **Evidence:** Both anchor clocks are zero-based `uint64` counters, yet delayed replacement projects only when `observationAudioSample != 0` (`JammaLib/src/ninjam/NinjamTiming.h:100`–`:116`, `b41b5a8`) and coordinator projects local phase only when `observation.LocalSample != 0` (`NinjamTimingCoordinator.cpp:219`–`:237`, `d6fdb88`). AudioHost legitimately publishes zero at the first callback: the device counter starts at zero and Timer absolute can also be zero (`AudioHost.cpp:445`–`:452`). The focused test currently codifies the sentinel collision by expecting a delayed boundary with observation zero to retain phase 700 rather than project the 9000 elapsed samples (`test/JammaLib_Tests/src/ninjam/NinjamTiming_Tests.cpp:150`–`:160`, `b41b5a8`).
- **Why it matters:** The same physical observation produces different projection solely because the session began at sample zero rather than a later numeric origin. A first-block observation processed after one or more blocks is treated as current, embedding scheduler delay in replacement or discipline calculations. This is a coordinate-representation defect independent of the S09-03 policy defect.
- **Recommended disposition:** correct. Carry explicit anchor validity (or an existing publication/generation validity fact) with the numeric coordinate, and allow zero whenever valid. Do not use the device counter as a Timer fallback or merge the two clock fields. Reconcile this representation with the complete command/observation value contracts already approved under F-014/F-015 instead of creating a new class.
- **Protected timing concepts affected:** remote observation time, device-audio sample coordinate, Timer-absolute local coordinate, and remote/local phase remain distinct.
- **Verification:** replace the zero-sentinel expectation with translation-invariance coverage: identical delayed scenarios at observation/boundary `(0,N)` and `(K,K+N)` must yield identical phase/delta; cover initial join and accepted replacement after multiple blocks; keep a genuinely unavailable-anchor case represented explicitly. This can share one prerequisite test with S09-03.
- **Human decision:** pending.

### S11-03 — Convert wrapped phase without rounding the final source sample into an early device wrap

- **Stage / reviewer:** Stage 11 — numerical, boundary, and clock domains.
- **Scope reviewed / exclusions:** coupled interval/position conversion only; tracker policy for accepting real wraps belongs to Stages 9–10.
- **Severity:** must fix before merge.
- **Evidence:** `ToDeviceTiming` scales interval length and wrapped interval position independently with nearest/half-up rounding, then reduces the scaled position modulo the scaled length (`JammaLib/src/ninjam/NinjamTiming.h:155`–`:164`, `d0d208e`). For source length 1000, source position 999, and 96 kHz -> 48 kHz, length scales to 500 while position rounds to 500, then `% 500` produces zero. Thus the last valid source sample is reported as the next device interval's first sample. The existing conversion test covers only an exactly representable 44.1 kHz -> 48 kHz midpoint (`test/JammaLib_Tests/src/ninjam/NinjamTiming_Tests.cpp:201`–`:223`). Tracker regards a late-quarter to early-quarter backwards transition as a genuine wrap (`JammaLib/src/ninjam/NinjamTimingTracker.cpp:56`–`:85`, `f36bfe7`), so the manufactured zero is behaviorally observable.
- **Why it matters:** Downsampled timing can advance a wrap by one source sample, increment the remote wrap count early, and emit join/discipline work against a boundary that has not occurred. It also violates the conversion invariant that every source phase strictly below its interval maps inside, not through, the corresponding target interval.
- **Recommended disposition:** correct. Define phase conversion relative to the converted interval so a non-wrapped source phase remains strictly below the converted length—e.g. a coupled rational mapping with an explicit endpoint invariant—while retaining nearest rounding for duration conversion if desired. Avoid an ad hoc domain merge: source phase/length and device phase/length should remain separately named value contracts.
- **Protected timing concepts affected:** remote source-rate master phase/length, remote device-rate master phase/length, and wrap count remain distinct.
- **Verification:** add downsampling boundary tests for source length 1000 at positions 998 and 999 under 96 kHz -> 48 kHz; assert monotonic in-interval mapping, result `< convertedLength`, and no tracker wrap before source position actually wraps. Add a non-integer up/down-rate case and preserve the existing exact midpoint case. Land this focused regression before refactoring conversion ownership.
- **Human decision:** pending.

### S11-04 — Make the tempo-to-samples helper total for non-finite BPM

- **Stage / reviewer:** Stage 11 — numerical, boundary, and clock domains.
- **Scope reviewed / exclusions:** helper input arithmetic only; malformed network-state recovery belongs to Stage 10/18.
- **Severity:** follow-up hardening.
- **Evidence:** `IntervalSampsFromTempo` rejects `bpm <= 0`, zero BPI, and zero rate, then casts `samples + 0.5` to `unsigned int` (`JammaLib/src/ninjam/NinjamTiming.h:126`–`:138`, `d0d208e`). NaN fails the comparison guard and the saturation comparison, reaching a non-finite floating-to-integer conversion; positive infinity instead yields zero through division. Direct tests cover zero but not NaN/Inf (`test/JammaLib_Tests/src/ninjam/NinjamTiming_Tests.cpp:130`–`:132`; `Quantisation_Tests.cpp:80`–`:83`). Current production use is behind `snapshot.Timing.IsValid` (`NinjamNetworkService.cpp:101`–`:110`), whose shared BPM bounds reject NaN/Inf (`NinjamTiming.h:45`–`:58`), so no present production failure was demonstrated.
- **Why it matters:** The inline helper has no documented finite-input precondition and is independently tested/used. Its invalid-input result is inconsistent and can be undefined or implementation-dependent at the conversion boundary, weakening a future caller's safety.
- **Recommended disposition:** correct as focused hardening, or explicitly document and enforce a finite-positive precondition at every caller. Prefer a direct `std::isfinite` guard returning zero, matching the existing invalid-input convention, with no new abstraction.
- **Protected timing concepts affected:** remote BPM/BPI and derived source-rate interval length remain distinct.
- **Verification:** focused helper tests for quiet NaN, positive/negative infinity, the plausible finite bounds, saturation, and half-sample rounding.
- **Human decision:** pending.

## Handoffs

- **Stage 7 / S07-03:** a coherent Timer observation must include, from one audio boundary, at minimum the widened Timer-absolute coordinate, wrapped phase, seed length, and the scene coordinate if downstream mapping consumes it. S11 does not prescribe the publication mechanics; a numerical fix that leaves independent loads still fails S07-03.
- **Stage 9 / S09-03:** S11-01 and S11-02 are prerequisites for trustworthy observation-time projection but do not resolve the join path's choice to freeze a delta against live local phase. One translation-invariance regression can expose both the zero sentinel and delayed-initial-observation policy defect without merging their ownership.
- **Stage 10:** if an anchor can genuinely be absent, define that as explicit state in its transition/value contract. Generation/session rollover and sequence-counter exhaustion remain state-machine questions; they must not be inferred from a zero clock value.
- **Stage 14 / verification:** add the prerequisite clock-width, zero-origin translation, and downsampling-tail regressions before F-005/F-006/F-009 refactors. Existing integration helpers that call `AbsoluteSamplePos(0)` at `NinjamTimingIntegration_Tests.cpp:581`, `:635` need 64-bit sentinels after S11-01.
- **Stage 15 / naming:** approved coordinate names should expose source-rate versus device-rate phase/length, Timer absolute versus device/scene counters, and observation validity. Do not shorten names in ways that erase these domains.
- **F-014/F-015 integration:** the conversion/value-contract cleanup should own S11-02/S11-03; the single authoritative clock boundary should own S11-01. Keep Scene thin and use existing Timer/NINJAM owners per the human decision against new classes.

## Uncertainties

- The `2^32` Timer wrap defect is platform-certain for the supported Windows target, but no long-running executable test was run during this read-only investigation. A deterministic seeded/counter test is sufficient; waiting hours is unnecessary.
- The exact desired phase conversion rule at fractional sample-rate ratios is not documented beyond nearest scalar rounding. S11-03 establishes the invariant violation and counterexample, but the human gate should approve whether endpoint-preserving floor/rational mapping or another explicitly tested rule is canonical.
- `MapRemoteElapsedToLocal` and `SyncPhaseMap::SourceCoordinateAt` can eventually overflow `uint64`/`int64`, and the generic scene-anchor helpers cast `uint64` to `int64` (`NinjamLoopAlignment.h:21`–`:30`, `:76`–`:81`, `:93`–`:119`). With current 32-bit lengths and real sample rates, the horizon is on the order of millions of years, so this is an accepted-looking residual rather than a merge finding; document production bounds when consolidating value contracts.
- Atomic sequence/generation counters are `uint64` and can wrap only at similarly unrealistic horizons. Their ordering semantics after wrap are a Stage 7/10 concern, not evidence for a Phase 2 numerical blocker.
- `ScaleSampleRate` returns the input unchanged for a zero source or target rate (`NinjamTiming.h:64`–`:65`), but `ToDeviceTiming` rejects those rates first. No other production call bypasses that guard; retain as an API-contract clarification rather than a separate finding.
- `RemoteWrapCount` and several legacy length/count fields remain `unsigned long`, hence 32-bit on Windows. Wrap count exhaustion requires billions of remote intervals and is not a practical merge risk; length remains bounded by the upstream signed `int` and current audio buffers. Do not use those residuals to justify collapsing wrapped and monotonic coordinates.
- No build, native tests, sanitizer, or runtime session was run because this stage is investigation-only. Existing tests establish ordinary circular-difference ties, modular MIDI wrap, and exact sample-rate conversion, but do not cover the three must-fix boundaries above.

## Conclusion

The branch's source/scene mapping, signed circular difference, local-offset normalization, and modular MIDI timestamp arithmetic are numerically sound within their current production bounds, and their distinct coordinate domains must be preserved. Extreme multi-million-year `uint64`/`int64` horizons are acceptable residual risks, not actionable merge findings.

Phase 2 cannot yet accept the numerical/clock-domain model. Three must-fix defects are evidence-backed: the purported Timer-absolute coordinate wraps after hours because it is computed and returned as Windows 32-bit `unsigned long` (S11-01); valid sample zero is overloaded as “anchor unavailable” (S11-02); and independently rounded downsampled length/phase can manufacture a premature remote wrap (S11-03). Non-finite tempo conversion is a lower-severity hardening item (S11-04). The corrective work should begin with the narrow long-duration, zero-origin translation, and downsampling-tail regressions, then integrate the fixes into the existing authoritative Timer/NINJAM value boundaries and S07-03 coherent snapshot without adding classes or collapsing any protected timing concept.
