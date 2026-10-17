# Temporary playback during delayed capture

Ordinary audio-only ADC takes can render unavailable selected-pass samples from
retained input history while delayed capture finishes. AudioHost lends a read-only
history view synchronously to the DAC render; Station and LoopTake forward it only
to eligible loops. NINJAM receives that same rendered output, as a FOH-style stereo
feed. Bounce, recording feedback, persisted samples and file exports continue to
read recorded storage only. No capture readiness or completion flag includes history.

The resolver operates at Loop's original-source read point. It selects covered
recorded storage, including immutable prefix aliases before normalization, then
history from the original physical pass, otherwise silence. Existing seam weights,
effects, playback delay, output advance and phase advancement still apply once.
An acquire observation of normalization permits normal body-tail reads; before
that observation the resolver never reads normalization's destination tail.

## Scope and limitations

- Audio-only ordinary ADC captures with explicit admitted trigger timestamps and
  stable published channel mapping qualify. Mixed MIDI, spectral variants,
  overdub, replacement/composite captures, active punch and cancelled takes retain
  existing behavior. MIDI-only scheduling is unchanged.
- Requests whose initial seam leg touches pre-pass padding below `b` retain
  the recorded-only readiness gate. That padding has no published coverage, so
  its eventual contribution cannot be proved equivalent to provisional zero or
  a wrapped sample. This is a per-request restriction: an 800-sample fade with
  a 441-sample selected prefix can still render ordinary body blocks promptly.
  Requests spanning a second wrap, invalid body phases and fades longer than
  the loop remain recorded-only because existing seam addressing is unproved
  for provisional substitution there. Existing seam and offset processing are
  unchanged.
- Only retained, physically arrived samples can be heard. Future samples, gaps,
  absent channels and overwritten history produce silence for the missing leg.
  Existing input history capacity is unchanged; no whole loop is retained/copied.
- Stream preparation invalidates history metadata. An admitted trigger must lie
  within the current stream epoch; wrapping timestamps use bounded signed deltas.
  A requested physical sample or take older than the supported half-range is
  outside this feature's contract and must not rely on temporary playback.
- Borrowed history and all source-resolution scratch remain callback-local.
  Nothing is retained after render returns or persisted for reload.

## Ownership and callback audit

`ChannelMixer::_historyChannels` is allocated/resized only during stream preparation
with callback admission stopped. Its sample pointers, contiguous-valid count,
64-bit ingress head/epoch, and consumed-ingestion frame count are read/written only
by the audio callback until the next quiescent preparation. The view borrows these
fields and the ring storage for synchronous rendering; null/partial/discontinuous
input invalidates the separate history coverage without resetting capture cursors.

The structural owner sets ordinary-capture eligibility before publishing immutable
LoopTake AudioState; callbacks read it under the existing topology lease. Existing
ADC release watermarks publish immutable recorded samples. Normalization writes
only its unpublished destination tail before release-publishing completion.
Closing take/loop states are accepted across the existing ordered normalization
traversal, so publication between stereo channels does not remove fallback.

New work is bounded by input channels and requested frames/seam legs, with no new
callback allocations, ownership transfers, locks, waits, logging or I/O. Fully
recorded requests take the recorded gate and perform zero history reads. The
static threading audit is supplementary evidence, not a real-time/thread-safety
proof. Hardware listening and device loopback remain separate validation.

## Automated validation

New tests compare production additive ADC capture with a selected-pass reference
for signed pre-delay, remapped stereo ramps, short/long loops, hardware latency,
ring/device/loop boundaries and nonzero seams. A causal per-frame mask checks
arrived versus future samples; an independent FIFO checks the serial loop plugin
and playback delay across partial silence and normalization. Ready reads assert
zero history reads. Deterministic stereo closure tests cover publication between
channel normalizations.

Feature-on/off punch comparisons include exact boundary impulses and the real
Trigger action/state machine, Station structural actions, prepared bounce writer,
delayed punch scheduling and ADC/DAC routing. They compare DAC output, stored
exports and exclusive completion edges. A finishing ordinary ADC source is also
rendered through both DAC and bounce, proving positive history use only for DAC.
History/lifecycle tests cover missing channels, gaps/resume, stream replacement,
retention edges, long-running epochs, cancellation, mixed MIDI and spectral/fade
ineligibility. Existing serial loop/take/station effect and MIDI/latency regressions
exercise the unchanged processing chain; new provisional-effect oracle coverage
is specifically at Loop level.

The real Trigger fixture splits blocks at controlled physical and delayed edges.
It does not exhaust arbitrary asynchronous job arrival or hardware callback jitter.
No hardware listening, loopback or callback deadline profiling is claimed.

## delly9 follow-up

The session log records a 44.1 kHz, 64-frame stream, I/O latency 177, and an
ordinary stereo Mic first take with `L=64018` and no MIDI loops. The referenced
rig currently specifies 86 ms pre-delay and an 800-sample fade. That gives
`D=3793`, selected seam `s=4234`, and selected prefix `F-b=441`. The original
whole-loop fade guard rejected every temporary request. Normal recorded
readiness then becomes available about another loop later, consistent with the
reported listening result; the log does not contain a first-audible timestamp.
Rig values were read after the session and are not a frozen log snapshot.

The follow-up regression reproduces those settings through production Station,
Timer and ChannelMixer, including delayed structural publication and stereo
channel remapping. It checks prompt body playback, original-pass sample identity
and unchanged first-master activation while capture remains incomplete. Exact
unproved/proved seam boundaries keep the unsupported requests recorded-only.
No capture calculation or first-master activation rule was changed.
