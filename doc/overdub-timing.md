# Overdub Capture And Playback Timing

This note describes the current code path for overdub recording in Jamma, with emphasis on how the new take captures the correct audio window and how playback is aligned when the overdub is finished.

## Goal

An overdubbed take needs to do two things at once:

1. Record the original loop into a fresh target buffer so the new take is a full replacement layer.
2. Insert live input into the correct sample positions when punch-in is active.

The implementation achieves that by separating three concerns:

- delaying ADC input before it is written into loops
- reading the source loop `MaxLoopFadeSamps` early when bouncing into the overdub target
- starting playback from a logical loop start that already includes the fade lead-in

## Important Terms

Let:

- `F = constants::MaxLoopFadeSamps`
- `P = cfg.Trigger.PreDelay`
- `I = input latency` from runtime stream params, or `cfg.Audio.LatencyIn`
- `O = output latency` from runtime stream params, or `cfg.Audio.LatencyOut`

Relevant code:

- `UserConfig::AdcBufferDelay`: `R = max(0, P + F - I)`
- `Trigger::CalcInputAlignedDelaySamps`: `I + R`
- `UserConfig::LoopPlayPos`: base play position is `F + P + outLatency`, then quantisation error is applied

So the ADC audio written into a loop is effectively this many samples old when it lands in the target buffer:

`I + R = max(I, P + F)`

That is the key alignment rule for live input.

## Where `_writeIndex` Starts

For both normal record and overdub, a new `Loop` starts with:

- `_writeIndex = 0` from `Loop::Reset`
- `_bufferBank.Resize(F)`

That means the first samples written into the new loop occupy the lead-in region, not the audible body of the loop. The loop's logical playback domain is later treated as:

- playable indices in `[F, F + loopLength)`
- crossfade data stored before `F`

Every block write goes to `_bufferBank[_writeIndex + writeOffset + i]`, and `Loop::EndWrite` advances `_writeIndex` by the block size. Nothing special happens for overdub here; the alignment comes from what data is written at each step.

## Overdub Recording Path

During each audio callback in `Scene::_OnAudio`:

1. ADC is written into the channel mixer capture ring.
2. Monitor audio is written into stations with zero extra delay.
3. ADC audio is written into stations using `AdcBufferDelay(I)`.
4. Station bounce then copies loop audio from source takes into overdub targets.

The important detail is that the target overdub `Loop` behaves differently by state:

- `STATE_OVERDUBBING`: accepts only `AUDIOSOURCE_BOUNCE`
- `STATE_PUNCHEDIN`: accepts bounce and ADC writes
- `STATE_OVERDUBBINGRECORDING`: continues recording while already playing

So before punch-in, the overdub target continuously records a copy of the original loop. While punched in, the live ADC is written into the same target buffer at the current `_writeIndex`, replacing the bounced source wherever the trigger mixer is faded down.

## Why Source Bounce Is Read `F` Samples Early

Source-loop bounce happens here:

```cpp
(*sourceMatch)->WriteBlock(*targetMatch, trigger, -((long)constants::MaxLoopFadeSamps), numSamps);
```

That `-F` offset is the core compensation for the large logical lead-in.

The overdub target starts writing at physical index `0`, but once the loop is finalized it will play from a logical start near `F`. If the source loop were copied from its current play position with no offset, the copied material would land `F` samples late inside the new buffer.

Reading the source loop `F` samples earlier cancels that. In effect:

- target write position starts at `0`
- future playback starts near `F`
- source read position is shifted by `-F`

So the sample that should later appear at the audible loop start is written into the correct place during overdub capture.

This part of your model is correct: the source loop is read `MaxLoopFadeSamps` earlier than its current play head.

## How `_playIndex` Is Set When Overdub Ends

When overdub finishes, `Station::OnAction(TRIGGER_OVERDUB_END)` does:

- quantise the recorded length if the clock is active
- compute `playPos = cfg.LoopPlayPos(errorSamps, loopLength, outLatency)`
- compute `endRecordSamps = cfg.EndRecordingSamps(errorSamps)`
- call `loopTake->Play(playPos, loopLength, endRecordSamps)`

Two details matter here:

### 1. The overdub path now includes output latency

For normal recording end, `LoopPlayPos` receives `O`.

For overdub end, it now also receives `O` after resolving it from runtime stream params or config:

```cpp
auto playPos = cfg.has_value() ?
	cfg.value().LoopPlayPos(errorSamps, loopLength, outLatency) :
	0;
```

So the overdub base play position is:

- `F + P + O`, then adjusted for quantisation error

not:

- `F + O`

That means the current code does take output latency into account again, but it still does **not** implement the simpler model "set play position to `MaxLoopFadeSamps + OutputLatency`".

The actual model is still:

- `MaxLoopFadeSamps + PreDelay + OutputLatency`, then quantisation error compensation

### 2. `Loop::Play` sets `_playIndex`, but leaves `_writeIndex` alone

`Loop::Play`:

- sets `_playIndex = playPos` (clamped against current physical length)
- sets `_loopLength = loopLength`
- switches to `STATE_OVERDUBBINGRECORDING` if `endRecordSamps > 0`
- pre-allocates buffer capacity if more recording will continue

It does **not** rewind `_writeIndex`.

That is important. The loop starts playing from the logical loop start while recording can continue from the current physical write head for `EndRecordingSamps(error)` more samples. This is how the implementation preserves the fade lead-in and any quantisation correction without throwing away already captured tail samples.

## Punch-In Alignment

Punch-in is handled in two layers.

### State changes happen immediately

On punch-in start/end, the `TriggerAction` is sent to `Station` immediately. That changes the target loop between `STATE_OVERDUBBING` and `STATE_PUNCHEDIN` right away.

So the target loop starts accepting ADC writes immediately from a state-machine point of view.

### Bounce muting is delayed to match the ADC age

At the same time, `Trigger` computes:

- `CalcInputAlignedDelaySamps = I + R`

and delays the trigger's overdub mixer fade by that amount.

That matters because the ADC samples arriving at the target loop are already old by `I + R` samples. If the bounced source were muted immediately, the live input would be inserted too early in the overdub buffer.

Instead, the implementation keeps source bounce flowing into the target until the delayed mute expires. The result is:

- bounced source audio fills the overdub target up to the aligned punch point
- live ADC then replaces it at the correct sample offset inside the new loop

On punch-out the reverse happens: the state flips back immediately, and the bounce contribution is faded back in after the same input-aligned delay.

## End-To-End Mental Model

The cleanest way to think about the overdub target is:

1. `_writeIndex` starts at `0`, which is the physical start of a buffer that includes a large lead-in region.
2. The source loop is copied into that buffer from `F` samples earlier than the source play head.
3. ADC is written only after the capture path has been delayed enough that its age matches the desired trigger pre-roll plus the lead-in.
4. When overdub ends, `_playIndex` is moved to roughly `F + P` so playback starts at the logical loop body, not at physical index `0`.
5. Output latency is included in that playback start position.
6. Quantisation error shifts `_playIndex` forward or backward, and `EndRecordingSamps` keeps recording long enough to complete the aligned loop body.

## What Is Correct In The Current Implementation

- The `MaxLoopFadeSamps` lead-in is explicitly accounted for in both capture and playback.
- Source bounce is read `F` samples early, which is necessary because the target write head starts at `0` while playback later starts near `F`.
- Live input timing is aligned with `I + R = max(I, P + F)`.
- Overdub finish now includes output latency when computing the new loop's play position.

## Issues And Risks Identified

### 1. The code still does not match the "`F + OutputLatency`" playback model for overdub

If that model is the intended design, the implementation is still different. Overdub end now uses `LoopPlayPos(..., outLatency)`, so DAC latency is included, but `PreDelay` is also still part of the base playback position.

### 2. Punch-in likely causes an audible playback gap

`Station::TRIGGER_PUNCHIN_START` immediately mutes the source take, but `Loop::ReadBlock` does not treat `STATE_PUNCHEDIN` as playable. That means:

- the original source take stops being audible immediately
- the overdub target does not play while in `STATE_PUNCHEDIN`

The recording alignment logic can still be correct, but the audible result during punch-in is likely silence or a discontinuity.

### 3. The implementation couples two different timelines in a subtle way

The target loop state changes immediately, but the bounce mixer fade is delayed by `I + R`. That is valid for alignment, but it is easy to misread when debugging because:

- loop state
- source-take mute state
- trigger bounce-mixer state

do not change at the same sample time.

This is worth keeping in mind if further punch-in or monitor changes are made.

## Direct Answer To The Current Hypothesis

Your first statement is right:

- yes, the code reads from the original loop `MaxLoopFadeSamps` earlier during overdub bounce

Your second statement is not what the current code does:

- no, the overdub path still does not set playback to `MaxLoopFadeSamps + OutputLatency`
- it sets playback to roughly `MaxLoopFadeSamps + PreDelay + OutputLatency`, then applies quantisation error compensation
- normal recording end and overdub end now both use output latency