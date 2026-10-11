# MIDI playback fidelity investigation

Investigated 2026-10-12 alongside the MIDI seam continuation implementation.
This is a source and existing-test review, not a claim of physical DAC/ADC
verification. The investigation was delegated into independent grid-origin and
VST delivery/latency reviews. Findings below are separate follow-up work;
production changes for them are not part of the seam note merge.

## Answers to the requested questions

**Starting an ordinary take offbeat does not create a take-relative MIDI grid.**
Local quantisation snaps the event's absolute transport position, then maps the
boundary back into the take. All takes therefore share the same transport grid
([MidiQuantisation.cpp:221](../JammaLib/src/midi/MidiQuantisation.cpp#L221)).
An onset displaced by an exact integer number of selected subdivisions describes
the same grid. With rational boundaries and a grain not divisible by the divisor,
translated rounding can differ by one sample; that is not a large audible grid
shift. The UI's Quarter/1/4 means **one quarter of the inferred grain**, so the
inferred grain must also match the intended musical unit
([MidiQuantisation.h:12](../JammaLib/src/midi/MidiQuantisation.h#L12),
[MidiQuantisation.h:92](../JammaLib/src/midi/MidiQuantisation.h#L92)).

**Promoting an already playing loop to master has a confirmed phase-loss bug.**
The captured master phase is immediately erased while applying its timing. This
can make the new transport grid disagree with unchanged audible loops, including
promotion on an otherwise valid 1/4 subdivision. See F1.

**MIDI events have sample-position mapping into the current output block, but
there is no general guarantee of alignment at your ears.** The engine preserves
MIDI cursor advancement and common signed phase corrections. Output hardware
delays audio rendered in the same block together. However, plugin processing
latency is explicitly not compensated, adapter queues can lose events, and no
end-to-end audible impulse test covers record completion, capture origin,
output latency, intro samples, and MIDI instrument response together. Copying
the audio raw cursor into MIDI would also be wrong: audio includes a fade-prefixed
capture coordinate while MIDI uses event offsets. See F2 and the timing trace.

## Timing trace and what existing tests actually establish

1. Recording captures MIDI event positions either from MIDI trigger timestamps
   (first master) or from elapsed capture samples. The timestamp path subtracts
   the press timestamp; the regular path subtracts event age from the recorded
   sample count ([LoopTake.cpp:1648](../JammaLib/src/engine/LoopTake.cpp#L1648),
   [LoopTake.cpp:1761](../JammaLib/src/engine/LoopTake.cpp#L1761)).
2. For a running clock, Station captures the transport position of the first
   recorded audio block, including the station transport offset
   ([Station.cpp:1237](../JammaLib/src/engine/Station.cpp#L1237)). That anchor
   replaces the provisional trigger transport origin
   ([LoopTake.cpp:3113](../JammaLib/src/engine/LoopTake.cpp#L3113)).
3. At record completion, MIDI starts at recorded-sample-count modulo length when
   that callback anchor exists; otherwise it starts at the quantisation error
   modulo length. It does not use the audio raw index
   ([LoopTake.cpp:42](../JammaLib/src/engine/LoopTake.cpp#L42),
   [LoopTake.cpp:1807](../JammaLib/src/engine/LoopTake.cpp#L1807)).
4. ReadMidiBlock maps event-cursor offsets into the current device output block:
   outputBlockStart + eventTime - midiBlockStart
   ([LoopTake.cpp:1735](../JammaLib/src/engine/LoopTake.cpp#L1735),
   [MidiIndexedOutputSink.cpp:23](../JammaLib/src/midi/MidiIndexedOutputSink.cpp#L23)).
   Station delivers these events before processing its VST audio block
   ([Station.cpp:834](../JammaLib/src/engine/Station.cpp#L834),
   [Station.cpp:908](../JammaLib/src/engine/Station.cpp#L908)).
5. After playback, MIDI advances by the same block size. Direct timing corrections,
   local transport offsets, and mapped source restoration move audio and MIDI by
   the same signed amount, each modulo its own entity length
   ([LoopTake.cpp:609](../JammaLib/src/engine/LoopTake.cpp#L609),
   [LoopTake.cpp:688](../JammaLib/src/engine/LoopTake.cpp#L688),
   [LoopTake.cpp:779](../JammaLib/src/engine/LoopTake.cpp#L779),
   [LoopTake.cpp:845](../JammaLib/src/engine/LoopTake.cpp#L845)). These preserve
   an existing relative phase; they do not establish missing initial calibration.
6. Audio capture uses hardware input latency plus an additional ring read delay
   ([UserConfig.cpp:393](../JammaLib/src/io/UserConfig.cpp#L393),
   [AudioHost.cpp:697](../JammaLib/src/audio/AudioHost.cpp#L697)). Audio playback's
   raw index includes MaxLoopFadeSamps, Trigger.PreDelay, and, on the overdub-end
   path, output latency ([UserConfig.cpp:412](../JammaLib/src/io/UserConfig.cpp#L412),
   [Station.cpp:1589](../JammaLib/src/engine/Station.cpp#L1589)). Ordinary record
   completion instead calls OverdubPlayPos, which supplies zero output latency
   ([Station.cpp:1507](../JammaLib/src/engine/Station.cpp#L1507),
   [UserConfig.cpp:479](../JammaLib/src/io/UserConfig.cpp#L479)). These are different
   capture/source cases and need a common audible-time derivation before changing
   either. Intro/fade storage samples are not a MIDI scheduling delay.

Existing coverage verifies a head MIDI attack maps to the first output sample
even when an arbitrary audio play index contains fade/pre-delay/output-latency
components; it does **not** compare audible audio and MIDI transients
([MidiLoop_Tests.cpp:660](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L660)).
Shared grids and delayed callback start/cursor override are covered at
[MidiLoop_Tests.cpp:1023](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L1023)
and [MidiLoop_Tests.cpp:1069](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L1069).
Common correction and unequal-length phase preservation are extensively covered
in [LoopTakeTiming_Tests.cpp:174](../test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp#L174)
and [LoopTakeTiming_Tests.cpp:477](../test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp#L477).

## F1: Master promotion erases captured phase — confirmed

TrySetMasterFromHover sets Timer phase from the running representative audio Loop,
then calls ApplyTiming ([Quantiser.cpp:523](../JammaLib/src/engine/Quantiser.cpp#L523)).
ApplyTiming calls Timer::SetQuantisation
([Quantiser.cpp:196](../JammaLib/src/engine/Quantiser.cpp#L196)), which resets both
sample offset and loop count ([Timer.cpp:76](../JammaLib/src/utils/Timer.cpp#L76)).
Existing loop cursors are not reset in that operation. At nonzero phase, the
new transport starts at zero while audio keeps playing elsewhere. MIDI-only
master targets also have no representative audio Loop from which to capture phase.

Proposed fix: publish coherent local master geometry plus phase for application
at an audio boundary, including the MIDI-only cursor case. Capture/restore the
phase after the new geometry is established, and define how existing take
transport origins are rebased. Simply swapping two job-thread calls does not
address coherent callback ownership or old take anchors.

Add regressions promoting a running master at nonzero Quarter subdivisions and
at an arbitrary off-grid phase; include MIDI-only targets, changed grain/master
length, and existing quantised takes. Assert both transport phase and actual
MIDI/audio transient positions. Fresh first-master recording is different:
unseeded Timer has no progressing local master grid
([Timer.cpp:42](../JammaLib/src/utils/Timer.cpp#L42)); recording establishes it.
MIDI-only stop-age correction already has explicit coverage
([MidiLoop_Tests.cpp:613](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L613)).

## F2: Plugin latency prevents a general audible-sync guarantee — confirmed

IVstPlugin explicitly documents that latency is not included in LoopPlayPos,
audio cursor, MIDI cursor, or NINJAM export; VST3 latency reporting is still zero
([IVstPlugin.h:117](../JammaLib/src/vst/IVstPlugin.h#L117)). Loop exposes a chain
latency sum but calls it plumbing only
([Loop.h:316](../JammaLib/src/engine/Loop.h#L316)); the overdub playback path has
a hardware-only compensation TODO
([Station.cpp:1600](../JammaLib/src/engine/Station.cpp#L1600)).

If a MIDI instrument path has additional processing delay compared with another
loop's path, equal event/block sample positions arrive at your ears at different
times. Paths with identical added latency can still agree with each other, but
that is conditional. Instrument envelope attack time is a separate musical
response characteristic, not automatically reported PDC.

Proposed fix: implement and publish effective path latency, including VST3 latency
query/change notifications, and choose one output-reference timeline. Compensate
paths once, preserving per-entity anchors and distinct audio/MIDI capture domains.
For routes to different plugin indexes, use the destination's remaining processing
path rather than the entire station-chain sum.
Do not add hardware output latency indiscriminately to every MIDI cursor; shared
DAC latency already applies to all rendered paths.

Add a fake instrument that renders impulses at event offsets and has selectable
reported processing delay, plus a fake audio delay effect. Compare rendered
transients across station/take/loop paths for record and overdub completion,
nonzero input/output latency, pre-delay, fade prefix, quantisation error, local
shift, block splits, load, and phase corrections. Finish with physical loopback
for actual driver latency and instrument response. Existing event-only/fake-plugin
tests do not prove this outcome.

## F3: Fixed VST event capacity can strand voices — confirmed mechanism

Both adapters accept at most 256 MIDI events per block; excess events return
silently ([Vst2Plugin.h:151](../JammaLib/src/vst/Vst2Plugin.h#L151),
[Vst2Plugin.cpp:529](../JammaLib/src/vst/Vst2Plugin.cpp#L529),
[Vst3Plugin.cpp:181](../JammaLib/src/vst/Vst3Plugin.cpp#L181),
[Vst3Plugin.cpp:230](../JammaLib/src/vst/Vst3Plugin.cpp#L230)). Multi-loop events,
live input, and seam/full-flush releases can share that capacity. Station clears
held ownership before SendMidiEvent, whose return type is void
([Station.cpp:716](../JammaLib/src/engine/Station.cpp#L716),
[IVstPlugin.h:92](../JammaLib/src/vst/IVstPlugin.h#L92)). MidiLoop similarly updates
its held state after calling a sink that cannot report rejection. A dropped
release can therefore leave a plugin voice sounding after the engine believes
it released it. This limitation predates seam continuation.

Proposed fix: retain bounded allocation-free delivery but reserve/prioritise
release capacity, expose acceptance/overflow recovery to the route owner, and
provide a reliable bounded release-recovery path. Test a dense burst exceeding
256 events, especially a release after the queue is full and a many-note flush.

## F4: VST3 editor transition discards a MIDI block — confirmed mechanism

VST3 skips ProcessBlockMulti while its editor is opening
([Vst3Plugin.cpp:1692](../JammaLib/src/vst/Vst3Plugin.cpp#L1692)). BeginMidiBlock
clears input events on the next block
([Vst3Plugin.cpp:1856](../JammaLib/src/vst/Vst3Plugin.cpp#L1856)). Events submitted
in a skipped block can disappear after engine ledgers have changed. Mapped
parameter points are instead cleared after processing, so a skipped processing
block can retain those points with stale offsets
([Vst3Plugin.cpp:1714](../JammaLib/src/vst/Vst3Plugin.cpp#L1714)). Frequency and
plugin-specific audible impact need a live repro.

Proposed fix: define explicit skipped-block delivery/recovery policy without
blocking the callback. Test attacks, releases, and mapped CC during editor-open
suppression. VST3 also bounds parameter automation to 32 points per parameter,
coalescing the final point when full
([Vst3Plugin.cpp:344](../JammaLib/src/vst/Vst3Plugin.cpp#L344),
[Vst3Plugin.cpp:390](../JammaLib/src/vst/Vst3Plugin.cpp#L390)); this is another
fidelity limit for dense controller curves.

## F5: Velocity scaling can disagree with seam continuation state — confirmed mechanism

MidiIndexedOutputSink scales velocity to an integer in [0,127]
([MidiIndexedOutputSink.cpp:24](../JammaLib/src/midi/MidiIndexedOutputSink.cpp#L24)).
At zero or a sufficiently low master level, a source NoteOn becomes a
velocity-zero NoteOn, which is a MIDI release. MidiLoop nevertheless records its
held/continuation state from the unscaled source event
([MidiLoop.cpp:810](../JammaLib/src/midi/MidiLoop.cpp#L810)). For an unmatched attack,
future same-pitch attacks can be suppressed although no attack sounded. Raising
level later cannot sound that pitch until a played release or explicit flush
clears continuation. IsMuted and UnmutedLevel are separate checks
([LoopTake.cpp:1733](../JammaLib/src/engine/LoopTake.cpp#L1733),
[LoopTake.cpp:1746](../JammaLib/src/engine/LoopTake.cpp#L1746)).

Proposed fix: make effective attack eligibility explicit before continuation is
activated, or ensure nonmuted attacks retain a positive MIDI velocity while
true mute uses the existing full flush. Choose intended low-level musical
semantics first. Add a zero/tiny-level unmatched attack, raise level before the
next iteration, and assert actual routed events and both held ledgers agree.
The existing master-level test checks ordinary 0.5 scaling only
([MidiLoop_Tests.cpp:684](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L684)).

## F6: Cursor jumps do not reconstruct MIDI voice state — confirmed policy gap

Direct correction and mapped restoration move the MIDI cursor without flushing
or reconstructing notes ([LoopTake.cpp:688](../JammaLib/src/engine/LoopTake.cpp#L688),
[LoopTake.cpp:813](../JammaLib/src/engine/LoopTake.cpp#L813)). ReadBlock emits only
events in the selected intervals and flushes on revision/seam/explicit request;
it does not detect a skipped attack or release caused by a cursor relocation
([MidiLoop.cpp:732](../JammaLib/src/midi/MidiLoop.cpp#L732)). A forward jump over a
release extends a voice; a jump into a held span after its attack does not
reconstruct that attack. New unmatched continuation can survive seams until a
release is actually visited, as designed. Common timing-map corrections preserve
cursor relationships but cannot promise equivalent note envelopes or source
waveforms. Integer mapped source advancement can additionally repeat or skip
samples; the alignment design document explicitly accepts this limitation.

Proposed fix: define a separate relocation/chase policy, distinguishing ordinary
seam continuity from deliberate seek and small discipline corrections. Prepare
bounded chase metadata outside the callback if required. Test jumps across
paired/unmatched attacks/releases, both signs, different block partitions, and
remote map changes. Avoid silently treating every transport correction as a
full flush, which would introduce its own audible retriggers.

## Validation status and priority

The seam implementation was committed as `8c76364`. Incremental Debug x64
engine/native-test build passed with zero warnings or errors. The native run
`--gtest_filter=*Midi*:LoopTakeTiming.*` passed all 382 tests across 52 suites,
including eight new seam regression groups and updated editor seam assertions.
The callback hot-path audit and `git diff --check` passed. No physical playback
or dedicated failing regressions for F1–F6 were run; those findings come from
source review and identify follow-up work.

The new raw/quantised unmatched-start arrays are written only by the existing
serialized job/MIDI publication owner. Callback readers access them through
the unchanged immutable triple-buffer snapshot and seq_cst reader lifetime
protocol. They are reclaimed with the containing MidiLoop, under its existing
teardown contract. The continuation bitset is callback-owned and has no job
readers/writers or new synchronization. Played releases/full flushes clear it;
the existing atomic held-note mirror continues to expose emitted state. Manual
review found no new callback allocation, locks, waits, or logging.

Recommended order: fix/test F1 and F5 first, implement reliable release delivery
for F3/F4, then complete F2 with rendered impulse and physical-loopback evidence.
Define F6's musical policy independently so phase correction does not silently
change the seam continuation contract. Until then, sample-position correctness
within a rendered block is supported by code/tests, while audible synchronisation
across arbitrary plugin paths remains conditional.
