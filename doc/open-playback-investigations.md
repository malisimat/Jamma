# Open playback investigations

Updated 2026-10-12. Branch: feature/midi-loop-seam-merge. Implementation baseline:
**9ff58a2** (Fix MIDI playback fidelity and recorded path latency).

## Status and session instructions

- Original seam plan and fidelity fixes F1–F6 are implemented and committed.
  Details: [seam plan](midi-loop-seam-note-merge-plan.md) and
  [fidelity report](midi-playback-fidelity-investigation.md).
- Baseline verification: incremental Debug x64 engine/tests and app builds passed;
  all 1466 native tests in 179 suites passed; independent Sol review completed.
  These results predate the investigations below and do not prove their resolution.
- Current work is investigation/reporting. No new engine fixes or regressions have
  been applied for the issues below. This file is the handoff requested by the user.
- User requests Luna subagents for code research, Sol for implementation, and a
  separate Sol reviewer; root collates, orchestrates, and verifies. Spawn agents
  with model overrides and fork_turns=none, giving them this file and repo policy.
- Read AGENTS.md, doc/glossary.md, doc/loop-alignment-and-ninjam-sync.md, and
  doc/realtime-audio.md. Before each build/test read local .vscode/tasks.json and
  doc/build.md; incremental affected-project builds only. Apply threading-review
  for cross-thread changes. Preserve immutable publication and callback constraints.

## 1. First master playback feels shorter than two recording presses — OPEN

**User evidence:** press once to start, press again to stop; playback feels shorter.
Audio vs MIDI vs mixed capture, pedal interface, and measured shortening are unknown.

**Source findings:**

- Fresh seed deliberately floors logical duration to a whole grain count:
  Station.cpp:46–71 and :2047–2050; Quantiser.cpp:91–113.
  Logical length = floor(requested/grain) * grain. Default grain inference uses
  repeated integer halving, so ordinary loops lose only a few samples; this is
  not arbitrary rounding by a large pre-existing grain. Under compiled default
  policy and 40,000,000-sample cap, the derived bound is <2048 samples (~46ms at
  44.1kHz). Recalculate for changed policy, sample rate, or cap.
- A preseeded clock is different: Station.cpp:2040–2044 uses Timer::QuantiseLength.
  Timer.cpp:286–335 chooses nearest grain multiple or power-of-two multiple;
  power mode can shorten substantially (nearly one third just below a midpoint).
- Strong audio/mixed suspect: Trigger::StartRecording resets count and queues a
  job (Trigger.cpp:1849–1863), but active RECORDING state begins only upon start
  acknowledgement (:1459–1483). OnTick counts active blocks (:426–450).
  EndRecording sends this counter (:1866–1874). Pending-start time is excluded;
  magnitude needs a runtime trace. This bookkeeping predates 9ff58a2.
- MIDI-only unseeded takes already have a timestamp subtraction workaround
  (Station.cpp:2005–2017), conditional on no audio loops and both timestamps.
  Do not assume audio/mixed takes or preseeded MIDI takes use it.
- Standard activation starts and stops on down edges, not release:
  Trigger.cpp:1723–1777. MIDI NoteOn/nonzero CC and serial nonzero mean down
  (Trigger.cpp:251–271; SerialTriggerProtocol.h:38–48). Hardware messages/binding
  choice remain unverified; keyboard/serial releases rearm bindings.
- Recorded playback delay compensation changes output arrival/phase, not the
  logical repeat period. Instrument tails/empty captured portions can also affect
  perceived duration; no evidence yet identifies these in the reported session.

**Runtime evidence (read-only):** AppData/Roaming/Jamma/defaults.json selects
session.jam, jamload=1, rigload=0 (embedded rig; external default.rig is not active).
Saved session has no takes, quantisesamps=0, quantisation=off, timerticks=0.
This suggests an unseeded startup but does not prove live state at the presses.
Startup logs show 44.1kHz, 64-frame callback, input/output latency 177 samples;
no recording seed/completion trace was found. Do not expose credentials/plugin blobs.

**Next:** capture start/stop input edges and MIDI timestamps, structural start
acknowledgement, first capture sample, trigger counter at stop, requested length,
seed grain/count, logical length, current quantisation mode, and first two wraps.
Compare MIDI-only with audio/mixed and fresh with preseeded clock. Derive the
press interval and shortening before choosing a fix. Obtain actual pedal messages.

## 2. First master MIDI kick at start/end produces double kick — OPEN

**User evidence:** kick drums on beat 1 at start and end of first/master MIDI loop;
always hears a double kick, with the second sounding different. User asks whether
quantisation matches audible loop duration and whether jumps occur only at seams.
Pending clarification: every repeated seam vs first recording-to-playback transition;
MIDI note quantisation on/off; instrument/plugin; any tap-tempo use.

**Confirmed mechanism, not yet a captured reproduction:**

- A near-end onset strictly inside logical L can snap to the next grid boundary
  and wrap to local zero (MidiQuantisation.cpp:241–251). An onset exactly at or
  beyond L is left outside playback, rather than folded to zero (:241 guard;
  MidiLoop.cpp:849–855). Small differences in stop/kick ordering therefore matter.
- A head kick and wrapped tail kick can both remain in the published event list.
  Sorting is stable, with releases before attacks at equal time, and does not
  deduplicate attacks (MidiNote.cpp:87–123). MidiLoop.cpp:857–875 suppresses only
  active continuation/pending-release attacks; ordinary paired attacks both emit.
- Quantisation preserves each attack velocity. Different source velocities and
  instrument retrigger/choke/envelope behavior could explain the different sound;
  this has not been measured. Do not claim velocity is modified by quantisation.
- First-record completion adds synthetic releases for still-held captured notes
  at L-1 (LoopTake.cpp:2087–2100). Such tail notes can therefore be paired and do
  not qualify for unmatched-note seam continuation. Existing seam policy does
  not merge ordinary paired percussion attacks or deduplicate coincident attacks.
- Example for investigation: L=1000, grain=1000, quarter division; same pitch head
  on@0/off@100 plus tail on@980/off@990 gives two attacks@0, tail off@10,
  head off@100. With unmatched tail instead, stable head-first ordering can still
  emit both before tail establishes continuation. Add separate regressions for
  paired and unmatched tail, completion-generated off, and different velocities.
- Existing tests cover late attack wrapping (MidiQuantisation_Tests.cpp:368–389)
  and unmatched wrapped attacks (MidiLoop_Tests.cpp:2953–2972), but do not establish
  correct same-pitch head/tail collision policy for this reported recording.

**Grid/period audit:**

- Normal fresh local first-master seeding uses L=grain*count. LoopTake::Play
  publishes that L (:2016–2024), then passes it to MidiLoop::EndRecord (:2117–2118).
  Normal cursor advance is +block samples modulo L (:723–739); MidiLoop::ReadBlock
  segments/wraps using snapshot L (:778–829). Rounded rational grid boundaries
  avoid subdivision drift (MidiQuantisation.cpp:180–208, :230–232).
  No period/grid mismatch was found in this normal path. Duplicate attacks can
  occur even when the grid and loop period agree exactly.
- Playback can move outside seams on intentional relocation and accepted timing
  corrections (LoopTake.cpp:745 onward, :876–914); quantisation itself relocates
  event times in prepared snapshots, not the running cursor. Do not promise that
  every possible cursor change occurs only at a loop boundary.
- Separate conditional tap-tempo propagation concern: Quantiser.cpp:301–320
  accepts a sole MIDI-only master. Accepted tap geometry replays audio loops only
  (:401–404), then changes MIDI base grid and Timer (:405–410). SetMidiBaseGrid
  (LoopTake.cpp:3392–3399) does not update MIDI visual/snapshot length. If tap-derived
  master length differs, MIDI period remains old while grid uses new geometry.
  DeduceTapSeedTimingFromMaster (Quantiser.cpp:924–946) can produce a different
  length: e.g. L=1000 and tap gap=333 selects 3 grains of 333, logical 999.
  Confirm with a regression/trace; tap usage in the user's reproduction is unknown.

**Next:** save one reproducing take and compare raw/quantised/emitted MIDI (sample,
status, channel, pitch, velocity, timing flags, matched/synthetic offs). Record
L, grain, base interval/divisions, fraction, origin, transport start, offsets,
remote/tap authority, cursor per block, plugin latency/destination, and revisions.
Compare grid off/on and end kick just before/exactly at/after stop; first transition
vs subsequent seams; split blocks before/across/exactly at seam. Separately test
local tap with a sole MIDI-only take and a length with nonzero grain remainder.
Avoid globally suppressing repeated same-pitch notes before agreeing collision
policy: legitimate retriggers and duration ownership must remain predictable.

## 3. Remaining validation and deferred limitations

- Physical DAC/ADC loopback and third-party instrument audition remain unperformed.
  Existing impulse tests verify simulated engine/rendering behavior only.
- Direct live audio/MIDI monitoring remains unpadded by user choice; recorded paths
  align to the longest reported plugin path. Shared hardware output latency is
  not added again. Punch-in cancels buffered recorded output; plugin tails remain.
- Dense VST3 mapped controller curves retain bounded-point/coalescing behavior;
  see fidelity report F4. This was documented, not replaced by an unbounded queue.
- User said another separate session issue will be reported; no details yet.

Independent Sol source review confirmed the collision mechanism, normal fresh-seed
grid consistency, and conditional tap propagation defect. No new tests/builds were
run for this report; runtime event capture remains outstanding.

## Resume order

1. Collect pending user answers and reproducing session/events; keep observations
   separate from source-confirmed mechanisms.
2. Delegate Luna research on trigger duration and MIDI seam/grid independently.
3. If authorized to fix, delegate focused Sol implementation and separate Sol
   review; add regressions for confirmed scenarios, then build/test using local
   commands. Root reviews callback/threading safety and collates evidence.
4. Update this small tracker with outcome, test commands/results, remaining manual
   checks, and commit IDs. Do not mark these reports resolved from app launch alone.
