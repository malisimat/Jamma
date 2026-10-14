# Plan B: immediate sculpting alternatives

## Purpose and status

These are two alternative designs for exploration, not implemented features. Plan A is the lower-risk route: keep current immutable authoring state, separate editing/presentation from reconstruction, and project/upload only changed height regions. Plan B asks whether a different representation can make interaction resemble a sculpting application even during expensive audio work.

Investigation found that authoring, complete display preparation and reconstruction currently share one non-preemptive worker (`JammaLib/src/spectral/AudioSpectralWorker.cpp`, `_Run`). `AudioSpectralModel::BuildDisplay` projects about one million vertices for each revision; validation and upload preparation scan the field again. A second presentation thread alone would still wait for authoring to finish on the audio worker. The existing grid picking traversal is already bounded by time segments plus frequency segments, rather than all triangles.

Both proposals keep the immutable source PCM and its phase reference. Neither synthesizes content from an originally zero coefficient. A zero caused by editing is different: it must remain recoverable. In particular, painting a frequency region after silencing it with an Alt band must recover that region without unsilencing unrelated frequencies. No design can solve this by increasing a spectral multiplier underneath a separate zero-valued final PCM envelope.

Common acceptance goals are input-to-visible feedback within the next display frame for ordinary strokes, audio publication independent of that deadline, stable brush rate across amplitudes and mesh resolutions, bounded storage during a stalled audio worker, correct Undo/cancel, and exact correspondence between the drawn mesh and picking. Actual target timings must be established on the user's GPU; none are claimed here.

## B1: transactional GPU coefficient pages

### Representation

Retain coefficient-space editing, but move interactive application to GPU-resident pages. Each page stores the immutable original magnitude reference and mutable authored target magnitude in dB, with a separate edited-silence flag. Original phase can stay in the CPU analysis; it need not consume GPU storage unless a later GPU compiler requires it. The rendered displacement is sampled from these pages. A stroke is an ordered transaction of brush exposure commands, not a stream of full replacement height fields.

For a nonzero original coefficient, increasing an authored zero starts at the authoring floor and adds the brush's integrated dB exposure. Attenuation can set an explicit zero. Original zero coefficients remain zero. This gives additive height behavior without an unbounded gain division at silence.

Broadband bands must belong to this same coefficient-domain target representation. Applying a band writes all affected frequency bins; a later ordinary brush writes just its selected bins and can lift their silence flags. The audio compiler reconstructs the resulting coefficients once, without an additional zero-valued PCM envelope applied afterwards.

This is a deliberate difference from current sample-domain Alt envelopes. Coefficient bands can smear transitions over the FFT window. Existing saved states with sample-domain envelopes require an explicit compatibility conversion: render their current composite PCM off the audio thread and analyze it into the new target representation, while retaining the original source and conversion provenance for Undo. Do not silently discard their existing envelope or claim sample-for-sample equivalence. If this change is unacceptable, B1 needs a more complicated ordered audio-operator representation and should not proceed beyond the prototype.

### Interactive path and reconciliation

1. UI assigns each input transaction a source epoch, stroke ID and monotonic sequence number. Brush exposure follows the existing elapsed-time and path-integration semantics.
2. Render thread executes commands into a destination page through fragment shader/FBO passes. OpenGL 3.3 does not require a compute shader for this. Use scissored rectangles and ping-pong pages; never read and write the same attached texture in one pass.
3. The mesh samples the newest completed transaction. Navigation changes transforms and clipping, without recompiling coefficient pages.
4. An independent CPU compiler replays the same commands against immutable source data, creates a canonical CPU snapshot and reconstructs audio asynchronously.
5. Once a CPU revision acknowledges a command prefix, retire that prefix. Retain later visual transactions. Acknowledgment must not replace a newer GPU target with an older CPU raster or create a visible pop.

GPU float evaluation may differ from the current CPU double integration. Require declared tolerances and event-subdivision tests. The first prototype should use CPU-generated exact exposure patches uploaded to the GPU, then compare shader-generated exposure before adopting it. This distinguishes scheduling gains from numerical changes.

### Picking, ownership and bounds

UI owns the bounded command journal and transaction metadata. Render owns GL objects, GPU page mutation and completed visual sequence numbers. Compiler owns its CPU replay state. Workers receive immutable captures. Audio reads only the existing published playback snapshot; it never reads commands, textures, performs GL readback, waits or locks.

Picking needs a CPU mirror of the visible result. Options are a CPU replay of the same dirty pages, or evaluating original magnitude plus the visible command suffix for cells visited by the picking traversal. The latter must have a hard suffix limit and cannot secretly scan every command for every ray cell. Associate the CPU mirror with the last successful GPU transaction, including its transform and source epoch. If GL publication fails, invalidate that mirror.

The current height texture is about 4 MiB. Two copies add about 8 MiB of GPU storage. Coefficient pages are potentially much larger: one float plane is `4 * coefficientCount` bytes, and ping-pong target/source planes multiply that cost. Use a bounded resident page atlas rather than assuming all coefficient pages fit. GPU memory is separate from host admission; reserve CPU replay, journal, picking mirror, retained Undo tiles and cancelled compiler work explicitly.

Do not drop unacknowledged commands when the compiler falls behind. Compact completed transactions into an immutable canonical checkpoint, retiring only the acknowledged prefix. Permit compaction only when its provenance preserves silence and transaction order. At the hard budget limit, stop accepting additional authored input with visible feedback; neither audio nor visual thread may block waiting for the compiler. Source replacement invalidates all old epochs. Cancel restores the stroke checkpoint; Undo selects a checkpoint and replays its retained suffix.

### Prototype phases and validation

1. Implement one GPU target page and scissored CPU exposure upload. Measure render/upload latency and GPU capability failures with the current mesh.
2. Implement bounded transactions, asynchronous CPU replay and acknowledgment. Artificially stall reconstruction and verify that rendering continues.
3. Add CPU picking mirrors, seam duplication, cancellation, Undo and source replacement. Compare every visible checkpoint with CPU reference projection.
4. Prototype coefficient-domain broadband bands and selective recovery. Listen to transition smearing before committing to the compatibility change.
5. Only then prototype shader exposure integration, atlas residency and checkpoint compaction.

Test exact authored zero recovery, original zero retention, Alt silence followed by local ordinary amplification, unchanged frequencies outside that footprint, exposure subdivision, stale acknowledgments, journal exhaustion, dropped render frames, context loss, source epochs and teardown. Report audio/visual discrepancy separately from feedback latency.

### Tradeoff against Plan A

B1 can move brush work onto the GPU and avoid transferring a complete changed field. It also adds transaction reconciliation, GPU/CPU numerical agreement, page residency and a significant Alt-envelope compatibility decision. Plan A should establish whether these costs are justified before B1 proceeds.

## B2: an adaptive canonical sculpt field with an audio compiler

### Representation

Make a field in source time and logarithmic frequency the authoritative edit, independent of FFT bins and mesh vertices. Store an immutable original target-magnitude anchor, authored dB values and explicit silence/provenance bits in copy-on-write tiles. Use finer tiles around active strokes and coarser tiles elsewhere, with a fixed refinement limit and deterministic interpolation.

The mesh samples this field directly. Changing an authored value immediately changes displacement. The audio compiler samples or fits the field onto the selected STFT lattice later. Visual geometry resolution, authoring tile resolution and FFT resolution become separate choices.

Both ordinary brushes and broadband bands write this same field. A band writes silence or a relative target across its frequency span. A later ordinary brush raises the selected target from the floor, clearing only the selected silence coverage. There is no independent final PCM mask that can suppress this recovery. The original anchor permits Undo and recovery; a truly empty original region remains empty unless a future, explicitly separate synthesis feature supplies phase/content.

For low original magnitudes, derive bounded coefficients from the target magnitude and retained original phase. Avoid calculating an infinite gain when the original magnitude is zero. Frequency-local recovery is a first-class field operation, not an exception in the playback path.

### Interactive path and compilation

1. UI or a dedicated fast edit lane applies an integrated brush path to intersecting field tiles. Publish an immutable tile-root revision and dirty regions.
2. Renderer uploads changed tiles and samples the latest root. A small displacement atlas with coarse fallback can serve partially resident tiles.
3. CPU picking samples that exact published root with the same interpolation, including current navigation transforms.
4. A separate audio compiler maps the newest root into coefficient targets, preserving original phase, reconstructs PCM and publishes through the existing crossfade snapshot path.

Start with a uniform grid matching the current display and a single interpolation rule. Adaptive refinement is a second prototype, not a prerequisite. Exact representation preservation requires a compatible nested interpolation basis; arbitrary resampling or coarsening can introduce error and must have declared height/audio tolerances. Brush strength is measured in source seconds/octaves and integrated dB exposure, not number of tile samples.

Direct coefficient sampling is the simple compiler. An optional later compiler minimizes projected magnitude error under bounded temporal/frequency smoothness constraints. That fit can improve agreement, but makes compiler cost and audible behavior harder to predict. Neither compiler can recover transients beyond the chosen analysis window merely because the visual field is finer.

### Ownership, bounds and history

The fast edit lane owns mutable tile builders and stroke checkpoints. Renderer and picking receive const tile roots; the audio compiler receives const roots and source captures. Audio sees only immutable PCM playback snapshots. Last-owner tile and compiler destruction must remain outside callbacks.

A uniform 4096 by 257 field with one float target plane uses about 4 MiB; adding anchor/provenance planes and retained revisions increases that. A full frame/bin inverse lookup is optional and must not be allocated without admission. Adaptive storage has an explicit tile count, maximum refinement depth, atlas capacity and history cap. Resource estimation includes the currently drawn root, active stroke, queued/running compiler roots, cancellation, Undo roots and resident GPU pages.

At the refinement cap, continue authoring at the finest admitted resolution; never allocate without a bound or change strength. At a retained-history/storage cap, compact unreferenced tiles or refuse additional input with a clear status. Queued compiler requests may replace older queued roots because roots already contain every accepted edit. A running compiler may finish, but its result is rejected if its source epoch is obsolete. Cancel restores the before-root; Undo/Redo selects immutable roots and queues compilation without waiting for audio.

### Prototype phases and validation

1. Implement a uniform authoritative field and direct mesh projection behind an experimental path. Benchmark bounded tile edits independently of audio.
2. Implement direct STFT sampling and existing playback publication. Test ordinary brush and band silence/recovery using the same field.
3. Establish visible-target versus reconstructed-spectrum tolerances with tones, impulses, quiet noise, seam-crossing edits and short/long loops. Add listening checks for phase, leakage and window smear.
4. Add deterministic refinement/coarsening and CPU/GPU atlas sampling parity. Nested refinement must preserve sampled values; other basis changes and coarsening must stay within declared tolerances.
5. Explore constrained fitting only if measured mismatch warrants it.

Include stalled compiler, repeated source replacement, bounded backlog, checkpoint retention, Alt then local recovery, negative/positive clamps, subdivision invariance, seam/DC handling, exact picking, and Undo before audio catches up. Audit callback diffs and test memory admission under simultaneous roots and compiler scratch.

### Tradeoff against Plan A

B2 offers the cleanest sculpting model: the user's target surface is authoritative and audio compilation follows it. It changes editing semantics, saved-state representation and the meaning of visual/audio agreement. A field that looks exact can still compile into a spectrogram that differs because of the STFT lattice and phase constraints. It is a larger product decision than making current feedback faster.

## Decision gate

Implement and measure Plan A first. Consider B1 if GPU brush throughput remains the dominant interaction limit and coefficient-domain band behavior is acceptable. Consider B2 if making the sculpted target the authoring contract is more valuable than retaining current coefficient/envelope semantics. Neither alternative should replace the current path without measured latency, bounded-memory evidence, visual/picking parity and listening validation.
