# MIDI loop seam note merge plan

## First change

Once a NoteOn with no matching source NoteOff sounds, keep that channel/pitch
active across loop iterations. Suppress subsequent NoteOns for the same
channel/pitch until the next played NoteOff or an explicit flush. Preserve the
original attack's playback time and velocity.

This replaces the earlier finite-overlap proposal. Continuation also applies
when the next matching attack occurs later in the following iteration.

## Playback behaviour

For a loop of 1000 samples, suppose a paired note starts at offset 20 and ends
at 100, and an unmatched note of the same channel/pitch starts at 950:

- On the first iteration, the paired note plays normally at 20 and 100.
- The unmatched note sounds at 950 and survives the seam at 1000.
- The attack at 1020 is suppressed; the note keeps sounding through that point.
- The NoteOff at 1100 releases it and ends continuation.
- The unmatched attack at 1950 can sound again.

With no later played NoteOff, the note continues until an explicit flush.
A velocity-zero NoteOn counts as a NoteOff. Channels and pitches are independent.
A cold start at the head plays its attack because no continuation is active.

Continuation begins when an eligible unmatched attack actually sounds, at its
effective raw or quantised playback time. It suppresses all subsequent attacks
of that channel/pitch, including within the same iteration. Ordinary paired
notes retain their behaviour when no continuation is active.

## Confirmed code and constraints

- `MidiLoop::ReadBlock` flushes held notes both within a wrapping block and at
  offset zero after a block ending exactly at the seam
  (`JammaLib/src/midi/MidiLoop.cpp:732`). Both paths need selective seam flushing.
- `EmitEventsInRange` already scans snapshot events for each loop segment and
  tracks held channel/pitch slots in a fixed bitset (`MidiLoop.cpp:793`).
  This scan remains; the feature adds constant-time checks per event.
- `PublishCompletedEvents` builds immutable snapshots outside the callback
  using three fixed buffers (`MidiLoop.cpp:914`,
  `JammaLib/src/midi/MidiLoop.h:178`). Classification belongs here.
- Source pairing uses FIFO order within `MidiEvent::PairingSlot()`: timing
  class, channel, and pitch. Quantisation already follows this rule
  (`JammaLib/src/midi/MidiQuantisation.cpp:210`,
  `JammaLib/src/midi/MidiEvent.h:36`). Sounding-note suppression uses only
  channel/pitch; a played release ends continuation regardless of timing class.
- Display extraction closes a span on a repeated attack and truncates unmatched
  notes at the seam; it cannot supply source pairing
  (`JammaLib/src/midi/MidiNote.cpp:28`). Editor notes ending at the seam omit
  their NoteOff and qualify for continuation
  (`JammaLib/src/midi/MidiEditOperations.h:142`).
- A retained source NoteOff beyond the logical loop still makes its NoteOn
  paired. Do not mistake a skipped or clamped release for an absent endpoint.
  Recording retains capture tails, raw playback/export skip them, and
  quantisation can clamp paired releases into the loop
  (`MidiLoop.cpp:246`, `MidiLoop.cpp:270`, `MidiQuantisation.cpp:210`).

## Required changes

1. **Precompute unmatched attacks.** Pair source endpoints before quantisation
   changes their order. Use one source scan with fixed-capacity FIFO bookkeeping
   per pairing slot, then mark the remaining unmatched NoteOns. Avoid searching
   all events separately for each attack. Carry this classification with its
   event through quantisation and sorting, separately from MIDI bytes and
   source timing flags.
2. **Publish fixed metadata.** Extend `PlaybackSnapshot` with unmatched-start
   metadata aligned to its raw and quantised arrays. Build it through
   `PublishCompletedEvents` for completion, replacement, edit, restore, and
   quantisation changes. Keep the existing immutable publication/lifetime rules.
3. **Track continuation inside MidiLoop.** Add a callback-owned fixed bitset
   alongside `_held`, using the existing `NoteSlot()` helper. An emitted
   unmatched NoteOn sets continuation. Active continuation suppresses another
   same-slot NoteOn. A played same-slot NoteOff clears both states. Published
   held-note state must reflect the emitted output.
4. **Distinguish seam and full flushes.** Both wrap paths release held notes
   except active continuations. Use one shared flush implementation with an
   explicit policy or mask to avoid repeated emission/state-clearing code.
   Revision changes, empty playback, mute/cancellation, requested flushes, and
   `FlushPlaybackHeldNotes` fully release notes and clear continuation.
5. **Add regressions.** Extend
   `test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp` for the example, head
   attacks at zero and after zero, cold starts, multiple iterations without
   releases, same-iteration suppression, channel/pitch independence,
   velocity-zero releases, and timing classes. Compare identical timelines
   split before, across, and exactly at the seam. Cover raw/quantised playback,
   genuinely unmatched notes through save/load, retained out-of-window matching
   releases, ordinary paired notes, revisions, and explicit flush paths.

## Performance and scope

Classification costs O(N + S) per snapshot: N is the bounded source event count
and S is the fixed number of pairing slots. Use preallocated arrays, bitsets,
explicit helpers, and value types. Avoid repeated endpoint scans and container
growth.

Playback adds O(1) bitset operations per visited event. It does not edit arrays,
search ahead for releases, pair notes, or rebuild metadata. Flushing remains
bounded by the fixed note-slot count. The callback stays allocation-free,
exception-free, lock-free, and free of waits. Replacing the existing event scan
with an indexed scheduler is a separate optimisation requiring profiling.

Keep the implementation in `MidiLoop.h/.cpp`, with only quantisation/sorting
support needed to preserve event-to-metadata association. Raw data, export
format, quantisation timing/clamping, and source-note display keep their current
representation. The display may show separate source attacks that playback
joins. Existing capture-tail export filtering remains; save/load coverage here
uses genuinely unmatched source attacks.

Playback time remains in the MIDI event cursor's domain.
`LoopTake::ReadMidiBlock` and `MidiIndexedOutputSink` map it into output time
(`JammaLib/src/engine/LoopTake.cpp:1726`,
`JammaLib/src/midi/MidiIndexedOutputSink.cpp:23`). Preserve the ownership and
timing boundaries in [loop alignment and NINJAM sync](loop-alignment-and-ninjam-sync.md).

This feature needs no deferred-release scheduler, later-release comparison, or
recovery of unclamped releases. Broader trigger UX and finite-span seam overlap
handling remain separate work.

## Implementation outcome

The five required changes are implemented. Source FIFO pairing and raw/quantised
unmatched-start metadata remain separate from exported MIDI bytes. Both seam
paths preserve only emitted continuations; full flush paths release them.
Regression coverage includes completion and overdub publication, replacement,
editor notes ending at the seam, mute/cancel, retained capture tails, save/load,
and different block partitions.

The follow-up fidelity fixes in [the investigation](midi-playback-fidelity-investigation.md)
make sink acceptance part of the emitted-state contract. A rejected or scaled-to-zero
attack does not establish continuation. Rejected releases remain pending in fixed
callback state until accepted, and prevent a replacement attack in that slot.
Deliberate relocation releases old voices and chases destination notes; ordinary
seams and small timing discipline corrections retain continuity.

Final Debug x64 engine/test and app builds passed. The complete native suite
passed all 1466 tests in 179 suites; see the investigation for rendered coverage,
threading review, and the physical-loopback limitation.
