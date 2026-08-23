# Musical Transport Architecture Plan

## Purpose

Establish one truthful musical-transport model for local sessions and external
timing followers such as NINJAM. The model should provide plugins with stable
tempo, time-signature, and PPQ information without allowing an external timing
replacement to rewind the host sample timeline or disturb local loop phase.

This is an architecture plan, not a mandate for a particular class layout.
Prefer the smallest design that preserves the ownership boundaries below.

## Core model

The existing master clock is the source of local musical geometry:

- master-loop length;
- quantisation grain length;
- master-loop count;
- master-loop phase; and
- monotonic scene sample position.

When the master length is an exact multiple of the grain, local musical values
are derivable without additional mutable state:

```text
BPI = masterLength / grainLength
PPQ = masterLoopCount * BPI + masterLoopPhase / grainLength
BPM = 60 * deviceSampleRate / grainLength
```

The device sample rate is additionally required for BPM. Invalid or
non-integral local geometry must not be advertised as valid musical timing.

## Ownership boundaries

`NinjamTimingCoordinator` remains responsible for remote observation,
validation, follow-policy decisions, and publishing immutable timing commands.
It must not own live PPQ state or mutate callback-time transport state.

The master clock remains the authority for local master count and phase. Keep
pure local musical derivations close to that clock, so there is one definition
of local BPI, BPM, and PPQ.

Any stateful external-grid alignment is a separate musical-transport concern.
It may be composed by the clock or the audio-boundary transport owner, but its
implementation must not make `Timer` or `AudioHost` a god object. It should be
generic enough to support an external musical grid without being inherently
NINJAM-specific.

`AudioHost` applies a command once at an audio boundary and fans it out to the
Timer and local stations. It should delegate musical policy rather than embed
PPQ arithmetic, remote-wrap bookkeeping, or VST adapter detail.

VST adapters translate an already-computed musical transport snapshot into
their respective SDK fields. They do not independently reconstruct PPQ.

## External-grid adoption

For a local session, publish derived PPQ whenever local geometry is valid.

When an accepted external timing source changes tempo, interval, or phase:

1. Continue the current local musical timeline while the external grid is
   pending.
2. Establish the external alignment at its next confirmed interval wrap.
3. At that boundary, permit only a bounded forward move of PPQ to the next
   whole external interval boundary; never rewind host sample time or PPQ.
4. Publish a one-block musical-position-change indication when a locate really
   occurs.

This is a transport locate/tempo-map update, not a request to unload or reset
VST instances. The VST host context must change coherently: PPQ, tempo, time
signature, and any position-change indication belong to the same audio block.

For the current NINJAM convention, one interval is represented as one VST bar
with a `BPI/4` time signature. Preserve this as an explicit host convention,
not an implicit assumption about conventional four-beat bars.

## Code organisation

Do not introduce a new `timing` namespace for this feature. Use existing
locations and ownership concepts.

Non-trivial musical-transport policy and state-machine behaviour belongs in
`.cpp` files. Headers should contain data contracts, class declarations, and
only genuinely trivial value operations where they improve clarity.

One focused generic musical-transport component is preferable to separate
local-PPQ, remote-PPQ, and VST-specific transport objects. Keep pure local
derivation grouped with the master clock; keep external alignment and
forward-locate policy in the focused component.

## Verification

Cover at least:

- valid and invalid local master/grain geometry;
- exact local PPQ progression across master wraps;
- remote adoption from a mid-interval local position;
- forward-only PPQ alignment at the first accepted remote wrap;
- later phase discipline and reconnect behaviour;
- monotonic VST sample time;
- VST2/VST3 PPQ validity and position-change signalling; and
- preservation of local audio/MIDI LoopTake relative offsets throughout the
  transport transition.

Build and run the focused native timing and VST tests after implementation.
