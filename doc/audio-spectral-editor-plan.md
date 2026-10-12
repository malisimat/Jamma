# Audio spectral editor: feature and implementation plan

Status: live spectral/broadband editing, accurate displayed picking, committed playback orchestration, bounded session admission, waveform refresh and Undo/Redo are implemented. Native focused/regression validation passes; GPU/input/listening and optimized responsiveness verification remain pending. Persistence is deferred. See [current handover](audio-spectral-editor-handover.md) for exact evidence and remaining work.
Date: 2026-10-12.
Repository checked: Jamma audio-spectral-editor worktree.
Paths and symbols below were checked against this worktree; recheck line-level details before implementation because the plan is expected to outlive the current source revision.

## Agreed feature

Press E over an eligible, selected audio Loop to enter the existing editor lifecycle. The waveform unravels into a rectangular 3D spectral surface. Its axes are time horizontally, logarithmic frequency into depth, and compressed STFT magnitude vertically.

A fixed grid mesh receives heights from a texture sampled in its vertex shader. Colour follows height using the existing waveform level palette as the reference, with low values black. Silence produces a flat, black surface with the MIDI editor's overall framing, without pitch bands, guide lines, or alternating columns. A blue/cyan playhead crosses the surface at the Loop's own playback position. Dragging the background uses precisely the MIDI editor camera behaviour.

Without Alt, hold left mouse on the visible surface to increase nearby spectral magnitudes; hold right mouse to decrease them. Longer holds produce greater changes. This mode uses the fixed soft oval brush with fixed strength. Time wraps at the loop seam; frequency is clamped.

Hold Alt when beginning a left-mouse stroke to use a full-frequency bar brush. Horizontal movement paints time; vertical movement sets an absolute broadband level relative to the stroke's initial mouse Y. The initial level is 0 dB (unity), downward movement reaches exact zero at a finite drag distance, and upward movement boosts above unity up to a bounded limit. The bar spans all frequencies irrespective of mouse Y. Holding still does not accumulate more gain in this mode. Each press-to-release stroke is one Undo step.

Only the individual Loop under the mouse is edited. Stereo/multichannel siblings are unaffected. Edits are non-destructive. Spectral painting preserves original analysis phase and boosts existing sound only: originally zero spectral coefficients remain zero. Broadband time modulation is a separate amplitude envelope; modulation sidebands are expected and do not replace the retained source phase. Ordinary spectral-brush attenuation reaches authored exact zero, while retained original coefficients allow subsequent boosting to restore those bins. The broadband envelope can reach exact zero without discarding source samples, and subsequent absolute bar painting can restore unity immediately.

Playback updates continuously while painting, with feasible, measured delay. Spectral edits use background reconstruction; broadband envelope edits use a bounded immutable envelope handoff and do not require a new STFT reconstruction for audible response. Loop playback timing remains unchanged. The original recording and editable changes survive session save/reload. WAV output, bounce and overdub use the edited sound. Undo/Redo history is session-local.

## V1 boundaries and implementation defaults

These are implementation choices to validate during prototyping, rather than additional user-approved performance guarantees.

User steering: punch-in must never be delayed by editing. Source acquisition uses a bounded callback-side copy into storage prepared off audio; source changes cancel/retry acquisition. Older previews are acceptable while acquiring the precisely completed current source. Normal punch-in may destructively bake the source output after spectral and broadband gain into the existing bounce destination. The replaced portion becomes newly recorded audio; unaffected portions retain their audible edited result in the new original, with unity edit layers. No separate source stream or edit-state transfer is required. The source take keeps its own edits. Tests/builds and adding tests remain deferred until the user explicitly asks.

- Edit completed, stable audio only. A playing or muted Loop can be edited, but recording, capture-tail completion, punch and destructive writing exclude editing. Reuse the existing editor eligibility checks and strengthen them for audio source stability.
- Keep E entry and existing Esc/close behaviour, selection/hover targeting, camera restoration, scene dimming, and editor exclusivity.
- No phase editing, noise generation, frequency shifting, additional drawing/selection tools, adjustable brush controls, channel linking, or changes to transport authority. The fixed oval brush and Alt-held broadband bar are both in v1.
- Initial analysis configuration: real float FFT, 4096-sample periodic Hann window and 1024-sample hop. Store FFT normalization explicitly. Validate alternatives against transient detail, bass resolution and response cost before freezing the sidecar version.
- Initial brush: about 50 ms time radius and one-quarter octave frequency radius, with a smooth compact oval falloff. Initial tuning was 12 dB per second at its centre; live testing on 2026-10-12 confirmed flow works but requested a significantly stronger effect in both directions. A further live request asked for roughly ten times the stronger rate and an exposure-throttling audit. Current rate remains 480 dB per second. On 2026-10-12 the user identified per-original gain plateaus as incorrect and specified absolute limits: full red level and exact black/silence. Ordinary painting now bounds normalized spectral magnitude at 0 dB and authors exact silence at the -90 dB visual floor, with retained originals allowing recovery. The old -60/+24 dB relative brush bounds are superseded; Alt bar gain remains separate. Constants belong in a named configuration type. Tune them by ear and usability testing.
- Initial bar configuration: 160 logical screen pixels downward reaches zero; 160 logical pixels upward reaches the +24 dB boost ceiling. These are named, DPI-aware tuning constants. Use upward-positive drag displacement irrespective of the operating system's mouse-Y convention. The transfer curve and anchoring contract are specified below.
- Initial display: fixed compressed magnitude mapping, with a stable reference for the editing session, rather than renormalizing after every stroke. Establish a finite display floor and ceiling; exact zero is always zero height/black.
- Proposed surface topology: at most 2048 time segments by 256 frequency segments, fixed while open. Visual resolution is separate from DSP resolution. Verify triangle count and texture upload cost; changing heights must never rebuild the vertex/index buffers.
- Display frequency runs from an explicit positive lower bound to Nyquist, with an explicit DC edge/handling rule. Keep DC and Nyquist processing valid even when they are not individually distinguishable on the logarithmic visual mesh.
- Never silently drop edits on large loops. Estimate working memory before analysis, use bounded job/history budgets, and show a useful unavailable reason if a configured limit cannot be met.
- No global playback delay is introduced. Measure edit-to-audible publication latency separately from the wait until playback reaches a painted time region.

## Checked codebase foundations

| Existing area | Files and symbols | Implication |
| --- | --- | --- |
| Domain ownership | doc/glossary.md | Loop owns samples, length and local phase; LoopTake coordinates take state; Scene wires UI/jobs. |
| Existing editor | JammaLib/src/midi/LoopGridEditor.h and .cpp: Open, TryOpenFromKey, FindCandidate, Close, ApplyToModels | Already accepts either an audio Loop or a MIDI Loop. Audio currently opens an audio view, not an audio editing engine. |
| Editor interaction | LoopGridEditor: _BeginOrbit, _UpdateOrbit, _EndOrbit, _PointAt and OnAction overloads | MIDI background drag is custom editor orbit behaviour. Generic Camera background drag alone would not match it. |
| Alt and mouse coordinates | JammaLib/src/base/Action.h: MODIFIER_ALT; JammaLib/src/graphics/Window.cpp: key/touch action handling; LoopGridEditor::_UpdateOrbit | Alt is already exposed on pointer actions. App mouse pixels use upward-positive Y; latch modifier mode at down and normalize DPI rather than introducing a different input path. |
| Projection and framing | JammaLib/src/graphics/LoopGridProjection.h; JammaLib/src/graphics/Camera.h and .cpp | Reuse framing and ray construction concepts. Existing flat-plane intersection is insufficient for raised-surface hits. |
| Audio waveform renderer | JammaLib/src/graphics/LoopModel.h and .cpp: UpdateModel, BuildFixedGeometry, UploadWaveformTexture, Draw3d | Fixed geometry already receives heights from an RG min/max envelope texture. It is a 1D envelope, not a spectrogram. |
| Existing morph and palette | Jamma/resources/shaders/waveform.vert and .frag; Loop constructor in JammaLib/src/engine/Loop.cpp | Ring morphs into a time/amplitude strip. Loop uses the levels texture, with height mapped to its vertical gradient. Ambient, hover and material lighting currently lift black. |
| MIDI appearance | Jamma/resources/shaders/midi_note.frag, midi_grid.vert and .frag | Reuse playhead/reveal styling, without note/pitch bands or grid guides. |
| Rendering integration | Jamma/resources/ResourceList.txt; JammaLib/src/resources/ShaderResource.*; JammaLib/src/graphics/GlDeleteQueue.* | Register new shaders/uniforms and release GL resources through the existing context-safe lifecycle. |
| Picking limitation | Jamma/resources/shaders/picker.vert; LoopModel::Draw3d | Current picker uses fixed geometry and lacks waveform displacement. It cannot supply accurate spectral-surface hits. |
| Audio storage and playback | JammaLib/src/engine/Loop.h and .cpp: ReadBlock, WriteBlock, EndMultiPlay, ExportSamples; JammaLib/src/audio/BufferBank.h and .cpp | Playback directly reads mutable banked PCM. There is no immutable audio-content publication mechanism to reuse wholesale. |
| Completion | JammaLib/src/engine/LoopTake.cpp: EndRecording and JOB_ENDRECORDING handling; Loop::EndRecording | Analysis must wait for the final stable source, including capture tail, rather than merely observing a nonzero logical length. |
| Edit/undo precedent | JammaLib/src/midi/MidiLoop.h and .cpp: EditState, SnapshotForEdit, PublishEdit; JammaLib/src/midi/MidiLoopEditUndo.h; JammaLib/src/actions/ActionUndoHistory.* | Reuse revision-checked publication and the application's Undo history, with audio-specific data and lifetime handling. |
| Snapshot retention precedent | JammaLib/src/audio/AudioHost.h and .cpp: PublishPendingRigSnapshot, ApplyPendingRigSnapshotAtAudioBoundary, ReleaseRigSnapshotsBefore | Off-audio retention prevents callback references becoming the last owner. A similar lifetime guarantee is required for reconstructed PCM. |
| Export/import | JammaLib/src/io/IoSessionExporter.*; JammaLib/src/io/JamFile.*; Loop::FromFile and Load; LoopTake::FromFile | Session export writes WAV sidecars and metadata. Spectral edit state needs new optional metadata and sidecars. |
| WAV precision | JammaLib/src/io/WavReadWriter.cpp: _Write, FloatToChar | Existing WAV output is 16-bit PCM. Preserve original floats in a lossless native asset rather than round-tripping originals through this writer. |
| Bounce/overdub | JammaLib/src/engine/LoopTake.cpp: WriteBlock and ProcessActiveBounce; JammaLib/src/engine/Station.cpp: OnBounce | The edited source must enter the common Loop read path, so these operations hear the edited signal. |
| FFT dependency | vcpkg.json and JammaLib source/library search | No current application FFT/STFT implementation was found. Add a deliberate FFT dependency. |
| Timing | doc/loop-alignment-and-ninjam-sync.md; Loop body-phase APIs | Spectral frame coordinates are Loop source coordinates, not master transport coordinates. Publication must preserve local phase, offsets and anchors. |

## Architecture and state

Keep audio analysis/edit data and processed playback ownership in JammaLib. Add focused audio DSP types under JammaLib/src/audio, audio editor interaction under a neutral editor location, and graphics resources under JammaLib/src/graphics and Jamma/resources.

Extend LoopGridEditor's existing audio branch, delegating spectral gestures and analysis orchestration to an audio-specific controller. Reuse the shared lifecycle and orbit implementation; do not duplicate it or thread audio branches through MIDI note algorithms. Extract small shared helpers only where both modes need them.

Suggested new types, names to align with repository conventions before coding:

- AudioSpectralConfig: window/hop, oval brush, spectral gain bounds, bar time width and drag ranges/boost ceiling, visual compression and resource limits.
- AudioSpectralSource: immutable original PCM/context, source identity, logical/physical lengths, sample rate and transform configuration.
- AudioSpectralAnalysis: original complex coefficients or original magnitudes plus unit phase; immutable analysis identity.
- AudioSpectralEditState: source identity, monotonically increasing combined edit revision, tiled bounded spectral log-gain field, and a separate absolute broadband gain envelope in Loop source-time coordinates. Track spectral/envelope component revisions so one pipeline cannot overwrite the other.
- AudioSpectralPlaybackSnapshot: immutable spectral-processed PCM in the Loop's required physical layout plus a coherent immutable broadband envelope, source/component identities and layout metadata. Gate-only changes can reuse the PCM handle.
- AudioSpectralEditor / AudioSpectralGesture: UI state, captured pointer/button and modifier mode, frozen initial mouse-Y anchor/time mapping, current absolute bar level, stroke-before envelope and transient strongest-influence/winning-target data, brush path, stroke history and job scheduling.
- AudioSpectralModel: fixed mesh, height texture, CPU height mirror for picking, palette and shader resources.
- AudioSpectralEditUndo: revision-aware spectral tile or broadband envelope deltas integrated with ActionUndoHistory.
- NativeAudioSpectralSidecar: versioned lossless original PCM, spectral gains, absolute broadband envelope and configuration.

Take/Loop publication checks must confirm membership, eligible state and matching source identity. Scene forwards complete values and schedules work; it does not own spectral DSP or loop timing.

### Thread ownership and handoff

| State | Owner/writer | Readers | Handoff and teardown |
| --- | --- | --- | --- |
| Recording buffers and live play cursor | Audio callback | Existing engine; stable-source capture via validated off-audio operation | Never copy concurrently with writes. Pin eligibility/source generation for the copy; invalidate on resumed writing or reset. |
| Immutable original source | Loop/job-side lifecycle | Analysis worker, serializer | Retained while jobs/snapshots refer to it; freed off audio after acknowledgement/quiescence. |
| Editable spectral gains, broadband envelope and stroke history | Editor/job owner | Worker, callback envelope reader and serializer through immutable revisions | Copy-on-write dirty tiles/segments; coherent bounded publications. No shared mutable gain arrays. The callback uses precomputed envelope data rather than searching/editing a control-point container. |
| Analysis and reconstruction scratch | Dedicated worker | Worker only | Preplanned FFT and reusable scratch; cancellation at bounded work units; joins occur off audio. |
| Processed playback snapshot | Worker creates; job owner validates/publishes | Callback and exporter | Existing immutable snapshot/retention pattern adapted for PCM; retire after callback acknowledgement, off audio. |
| Current/previous playback handles and transition progress | Audio callback | Job-side receipts | Scalar/handle publication must be bounded and demonstrably callback-safe; no final ownership destruction on audio. |
| Height texture and GL handles | Render/context owner | GPU | Immutable CPU display revisions handed to render; upload/delete on valid GL context. |
| Picking height mirror and display identity | Render/editor coordination | UI picking | Pick the exact displayed mesh revision; retain it until input processing completes. |

Audit whether existing atomic shared_ptr operations satisfy the callback requirement on this toolchain. Do not assume atomic shared_ptr is lock-free. If necessary, retain immutable snapshots off audio and exchange bounded preallocated handles/slot indices with scalar atomics, preserving the established snapshot model. Reserve enough slots for active, transitioning and pending content; if none are free, coalesce worker updates rather than wait in the callback.

Before teardown, invalidate source/editor generations, stop new publications, cancel jobs, detach at an audio boundary, wait for off-audio acknowledgement, then retire data and GL resources. Ditch/reset/remove/load/sample-rate changes must invalidate stale jobs as well as history.

## DSP and reconstruction

1. Add a thin FFT wrapper. Proposed backend: KissFFT real float transforms through the repository's pinned vcpkg manifest. Verify its available port, build targets and API against the chosen revision; record licensing. Allocate plans and scratch off callback.
2. Copy a coherent stable source on first editor entry, and optionally prewarm after recording completion. Avoid eagerly allocating spectra for every Loop.
3. Analyze windowed PCM into positive-frequency complex coefficients. Define exact frame centres, sample origin and retained context. Native FFT frequency bins remain linear even though display/brush frequency coordinates are logarithmic.
4. Represent edits as bounded gain in dB relative to the original spectrum:
   edited magnitude = original magnitude * 10^(gainDb / 20).
   During a hold, gainDb changes by sign * strengthDbPerSecond * elapsedSeconds * brushWeight, clamped to its limits.
5. A compact smooth oval brush is evaluated in seconds and log-frequency/octaves. Time distance uses the shortest wrapped Loop distance. Frequency support is clamped. Motion is resampled so fast drags do not leave gaps; elapsed time, not frame count or mouse-event count, determines total exposure.
6. Never divide by zero to recover phase. Retain original complex direction; multiplying original coefficients by positive gain preserves phase. Truly zero coefficients remain zero. Handle DC/Nyquist real values, non-finite input and bounded numeric range explicitly.
7. Reconstruct off callback with inverse FFT and normalized weighted overlap-add. Compute normalization for the actual analysis/synthesis windows, arbitrary Loop lengths, and wrapped/context boundaries. Do not assume loop length is a multiple of hop or FFT size. Short loops and repeated wrapped window samples need explicit tests.
8. Establish the exact mapping to MaxLoopFadeSamps, physical capture tails and Hanning seam reads before integrating. Include real captured context in analysis where existing fades read it. Apply wrapped gain coordinates to required fade context; preserve unrelated physical tail data and length metadata. Do not treat a fade-prefixed buffer as sample-zero Loop body.
9. A unity spectral field reads the original playback source exactly, avoiding unnecessary numerical changes, before applying any separate broadband envelope. When that envelope is also unity, retain the transparent original-output path. Add a separate no-op STFT round-trip test to verify transform normalization.
10. Preserve intended headroom: bounded gains do not guarantee sample peaks below full scale. Do not silently renormalize the entire Loop or add a limiter to this feature. Keep internal float processing finite, show peak/overload feedback using existing UI facilities, and retain existing downstream/export clipping behaviour.
11. Fixed phase with modified magnitudes can produce time smearing and reconstructed spectra that differ from the painted target. The surface represents requested magnitudes; the inverse transform is the deterministic audible realization. Do not add iterative phase recovery or promise arbitrary perfect spectral reconstruction.

### Absolute broadband modulation / gate brush

This is a separate non-destructive time-domain gain layer, not a repeated spectral gain increment. Its default is unity everywhere. It operates on the immutable original PCM when there are no spectral edits, or on intentionally spectral-processed PCM otherwise. Existing spectral edits remain independent: restoring the broadband envelope to unity removes only broadband modulation.

#### Gesture and transfer curve

- Start with Alt + left mouse down on the eligible surface, including a silent surface. Capture source identity, mode, before-state, pointer anchor (x0, y0), camera transform and time mapping for the whole stroke. Cancel or finish safely if viewport/camera geometry is invalidated; do not silently reanchor. Right-click retains its ordinary spectral attenuation role; it is not a bar restore/silence toggle.
- Interpret initial gain as zero dB, meaning linear gain 1, not zero amplitude. Do not initialize from the previous envelope value under the mouse. Beginning a new stroke on a previously muted region can therefore restore it at the anchor level.
- Let dy be upward-positive mouse displacement from y0, Ddown the finite silence distance, Dup the upward boost distance, and B the boost ceiling in dB. Use logical screen pixels as the initial implementation, normalized for DPI. Do not derive dy from frequency, surface height or brush hit Z.
- For -Ddown <= dy <= 0, targetGain = max(0, 1 + dy / Ddown). At or below -Ddown it is exactly zero. At dy = 0 it is exactly unity.
- For dy > 0, targetGain = 10^((B * min(dy / Dup, 1)) / 20). Clamp above the ceiling. This gives a continuous finite mapping through unity without requiring a logarithm of zero.
- Horizontal movement paints this target into a fixed time-width bar covering the entire frequency extent, including DC/Nyquist. Vertical displacement changes target level only, rather than repositioning the bar in frequency. Freeze camera/orbit during the stroke and map horizontal pointer X to time using a cached time-axis projection at the initial anchor. Do not repick the changing heightfield or use current mouse Y to alter painted time. Handle an edge-on/ill-conditioned projection with a clear unavailable reason, not unstable jumps.
- Once captured, accept vertical control movement outside the finite surface rectangle while its horizontal time mapping remains valid. Moving beyond the finite gain range clamps the level. Moving outside the time extent pauses writes; endpoint brush support wraps across the Loop seam. It must not start camera orbit or cancel merely because the pointer is above/below the visible frequency surface.
- Holding at the same position repeats the same absolute value and has no cumulative effect. Horizontal path interpolation carries the interpolated target gain, so event/frame rate does not change the drawn envelope. A vertical-only drag updates the same time footprint. A new direct pass over a time instant replaces its authored target with the latest value; weaker neighbouring feather contributions must not overwrite a stronger influence already painted during that stroke.
- Set the absolute target exactly at the cursor's time instant, with progressively less effect on either side. Use a fixed smooth time falloff, initially a 50 ms half-width, and the strongest-influence rule below. Give the bar a full-frequency outline and a displayed target level; label exact silence distinctly from a finite dB value.
- Mode and anchor stay captured until release. Releasing/pressing Alt during a stroke does not switch mode or reset y0. Lost pointer capture cancels according to the existing Undo contract. Each new stroke establishes a new unity anchor.

#### Spatial feathering without cumulative erosion

The Alt bar paints a smooth temporal neighbourhood, but it is not a rolling average or an accumulating low-pass operation. A high peak followed by a nearby sharp trough must retain the peak's authored level: the trough's distant tail cannot repeatedly pull that peak toward the trough.

Capture E0(t), the broadband envelope before this stroke. Keep transient bestInfluence(t) and winning target/brush-position data for the touched envelope samples or segments. Initialize bestInfluence to zero; allocate/reuse this scratch off callback and account for it in the editor's memory budget.

For a brush centre tc and absolute level A:

1. Compute shortest wrapped time distance d = abs(wrappedDistance(t, tc)). With fixed half-width R, use a smooth compact falloff such as w(d) = (1 - (d/R)^2)^2 for d < R, and zero otherwise. Thus w(0) = 1, and influence decreases continuously to zero at the support edge. There is no full-strength plateau covering neighbouring times.
2. Form the candidate from the fixed stroke-before state: candidate(t) = E0(t) + w(d) * (A - E0(t)). Never use the previously painted result as the blend's baseline.
3. Accept a candidate only where its w is stronger than bestInfluence(t). Retain the winning result and influence. For an equal-strength tie, the later authored target wins, which permits a vertical-only drag or deliberate retrace at the same time to replace the level. Evaluate ties with a stable documented numerical convention.
4. At every authored cursor-time point, full influence sets the requested absolute level, including exact zero/unity. Later weaker feather tails cannot erode that point. An equal full-strength visit can change it intentionally. This is dominance by proximity/influence, not by gain: do not take the maximum gain, since that would prevent painting a trough.

Interpolate both time and absolute level along each swept path segment; evaluate the nearest point on that segment for each affected envelope location rather than depending on raw mouse-event density. This preserves the drawn peak/trough trajectory, fills fast-drag gaps, and makes results consistent across event/frame rates. A new segment passing directly over an earlier point counts as an intentional repaint; a distant tail does not.

Each stroke starts a fresh dominance record against its own before-state. This permits a later complete modulation pass to replace prior levels without retaining obsolete brush authority. The dominance scratch is transient: store the resulting envelope in persistence/Undo, not an unbounded history of mouse events.

Use the same falloff, dominance resolution and segment interpolation for cursor preview, envelope publication, cancellation and Undo. Changing the cursor's current level must not retroactively re-evaluate all previously painted times using that level. Do not apply a final moving-average/convolution pass over the whole envelope that flattens authored extrema.

#### Replacement and original-sound guarantee

Overwrite broadband envelope values at painted times. Never multiply the newly requested gain into a previous gate, normalize the entire recording, or analyze previously gated PCM as the original. Areas not visited remain unchanged; a full replacement pattern must traverse the entire Loop, explicitly painting unity where old muted regions should reopen. Do not silently reset untouched times when a stroke starts.

For the held-chord workflow: paint a first modulation; sweep the Loop again with a new anchored level trajectory; prior silent regions traversed at dy = 0 reopen at original level, downward regions become quiet/silent, and upward regions boost. Undo/Redo restores the envelope before/after each sweep.

Apply the envelope samplewise in the common `Loop::ReadBlock` path. For ordinary samples, use their wrapped Loop body coordinate. In the existing seam crossfade, apply the envelope independently to the loop-end sample and its loop-start partner at each sample's own wrapped source coordinate, then perform the existing Hanning mix. The actual prefix partner and its body-end partner differ by one logical Loop length, so both use the same wrapped body-time gain. This was confirmed during implementation: silencing the end crossfade band also silences the prefix partner, preventing seam leakage. Per-contributor application states the contract explicitly; the common scalar can equivalently factor through the linear Hanning mix. Build piecewise/samplewise evaluation data off callback; evaluation is bounded, allocation-free and contains no container searches. Avoid applying gain a second time in the mixer/bounce/export path.

Where the spectral field is unity and broadband gain is unity, use original samples directly, bypassing STFT resynthesis and unnecessary arithmetic. Fully reopened plateaus must match the original internal float playback samples exactly once publication transitions have settled, subject to the same existing Loop seam/effects processing. Exact zero plateaus contain zero samples. Intentional non-unity spectral edits, boost headroom, normal WAV quantization and downstream effects are separate from this guarantee.

Use the spatial feathering above to soften nearby levels, plus short, finite, explicit gain ramps only where a residual discontinuity or loop seam needs anti-click treatment. Preserve authored peak/trough levels and exact plateau values; confine ramps to transitions between them and shorten a ramp when necessary to fit neighbouring authored points. Do not repeatedly average the entire envelope or let smoothing progressively flatten a previously painted peak. Boundary regions may still change; do not claim instant hard-edge gating is artifact-free. The initial anti-click ramp length is about 2-5 ms, to be validated on held chords and transients.

Publish coherent immutable envelope revisions at audio boundaries, retain/reclaim them off audio, and smooth live revision transitions with the existing bounded transition mechanism. No FFT is needed solely to hear a broadband edit. A late spectral job must reuse the current envelope or publish a validated complete combined state, never reinstate an older gate.

For the surface, apply the absolute envelope to the target-magnitude preview before height compression; the bar can lower all frequencies to black or restore them without rebuilding mesh geometry. Framewise magnitude scaling is a preview, not an exact STFT of a rapidly time-modulated signal. If actual output spectra are displayed, analyze the composed PCM asynchronously and label/track that display revision; never feed that analysis back into the retained original or reconstruction phase.

### Live response strategy

Use one bounded worker pipeline for the active editor. Coalesce requests by complete source/edit revision. A held ordinary spectral stroke keeps advancing UI magnitudes and display, while reconstruction publishes useful intermediate revisions rather than waiting for release. An Alt bar stroke instead publishes absolute envelope changes; elapsed hold duration never changes its gain target.

Start with complete processed PCM snapshots to simplify playback and lifecycle correctness. Benchmark immediately. If whole-loop reconstruction cannot provide useful continuous response for representative loops, make dirty-frame reconstruction and immutable chunk replacement part of v1 rather than leaving the performance problem unresolved. An edited STFT frame affects every sample under its synthesis window, including seam overlap; update all affected samples and normalization contributions.

Do not repeatedly cancel almost-finished work under continuous painting. Complete/publish intermediate accepted revisions monotonically, then process the newest coalesced revision. Reject work from a changed source generation. Cap backlog, retained PCM, scratch, spectrum tiles and Undo memory. Display pending-analysis/reconstruction feedback when it is perceptible.

At the audio-block boundary, adopt a ready snapshot without resetting the cursor. Crossfade old/new content at the same source positions, initially about 5-10 ms, independently of the existing Loop seam fade. Maintain that transition across blocks. Keep only bounded active/transition handles; merge or defer new arrivals during a transition. No FFT, allocation, locks, destruction of large objects or job waits occur in ReadBlock/WriteBlock.

## Rendering and interaction

### Fixed mesh and morph

Create a dedicated spectral mesh/model rather than attempting to stretch the existing two-sided envelope topology into a full time/frequency surface. Build vertex/index buffers once per opening/configuration, with stable UVs and explicit seam endpoints.

Reuse the existing editor morph progression. Supply a ring-compatible collapsed starting position for each grid vertex and interpolate to the rectangular time/frequency location plus texture-derived height. Hide/crossfade the old envelope as the spectral mesh takes over, avoiding double surfaces. The animation must read as the selected waveform unravelling and must reverse cleanly on close.

Populate an R32F 2D display texture with compressed magnitudes resampled from full-resolution analysis/edit data onto display time and logarithmic frequency coordinates. Keep its resolution independent of STFT storage. Use bounded textures, dirty subregion uploads and paging/tiling when required by device texture limits. Do not transfer full source spectra to the GPU or require GPU readback for synthesis.

Reuse texture/PBO lifecycle patterns where useful. Initially direct subimage upload is acceptable if measured; add asynchronous staging only when it solves a demonstrated stall. GPU vertex texture capabilities, GL errors and unavailable resources must fail with a clear editor message.

### Colours, silence and playhead

Use the waveform gradient as the visual reference. The user requested an explicit black-silence, dark-blue, green, yellow, orange, red ramp during live testing. Colour spans -90 to 0 dB while surface height retains -90 to +24 dB headroom. Define one nonnegative magnitude-to-palette mapping; do not alternate positive/negative waveform colours across frequency.

Suppress ambient/material/hover additions at the silence floor so black stays black. Put interaction feedback in brush outlines and overlays. Match MIDI framing/reveal and scene dimming without drawing pitch guides or banded columns.

Use the Loop's body play phase modulo its own logical length. Share the existing cyan core/trail styling, drawing a constant-time stripe across all frequencies on the surface. Keep it visible over peaks through a small bias or overlay pass. Its time position must be independent of analysis-frame origin, master interval and reconstruction latency.

### Accurate surface picking

Construct a local-space screen ray using the same view/model transforms as rendering. Intersect the displayed heightfield triangles and return barycentric UVs for the nearest visible hit. Use bounded grid traversal or a heightfield acceleration structure; avoid scanning the entire mesh on each event.

The CPU picking mirror must use the same height compression, texture filtering/sample positions, triangulation and morph state as the shader. Disable spectral painting during opening/closing until the surface is stable. Keep displayed revision and picking data coherent.

A flat silent surface is still an editable surface hit. Rays outside the rectangle start the shared MIDI background orbit. Distinguish HUD hits before either operation.

### Gesture behaviour and Undo

Surface down captures either the ordinary spectral brush (left/right) or the Alt + left broadband bar; background down captures orbit. That decision, including modifiers and the bar's initial anchor, remains fixed until release. Suppress ordinary loop record/mute actions during editor-owned gestures. Holding without moving accumulates gain only for the ordinary spectral brush. Bar painting is absolute and idempotent. Ordinary brush movement outside pauses painting; the bar retains vertical level control outside the visible surface and pauses only when its horizontal time mapping is outside the valid span. Neither can switch to orbit mid-stroke.

Apply stroke updates at a bounded cadence, using elapsed-time integration for the ordinary brush and displacement-based absolute targets for the bar. Include a visible brush footprint and bar level indicator. Frequency bounds, time seam and numerical gain limits use the same rules in preview and actual edits.

One stroke stores changed spectral gain tiles or broadband envelope segments before/after and contributes one ActionUndoHistory entry. No-op strokes add nothing. Undo/Redo republish complete revisions and reconstruct, preserving original phase. Validate expected source/content generation rather than restoring old transport or geometry.

Esc/normal close ends a valid active stroke before exiting. Pointer capture loss or target invalidation cancels the uncommitted stroke and restores its before-state if the source remains valid; otherwise discard the obsolete editor state. Pending jobs cannot resurrect cancelled or undone edits. Undo is applied between strokes.

## Persistence, export and downstream behaviour

Add optional spectral/broadband metadata to `JamFile::Loop` and advance the existing manifest format version compatibly. `JamFile` currently writes format version 0.3.0 and has legacy/best-effort parsing; define the exact compatibility policy for old readers and unknown optional fields before choosing the version bump. Unedited/old sessions load with default unity spectral gains and unity broadband envelope. A gate-only edit is still an edited Loop and must persist without requiring non-unity spectral gains.

For an edited Loop, save:

- The ordinary edited WAV sidecar, preserving existing filename and legacy playback compatibility.
- A versioned native spectral sidecar containing lossless original float PCM, required fade/context/physical-tail data, source sample rate, logical and physical lengths, transform configuration, complete bounded spectral gain state, absolute broadband envelope including exact zero/unity values and ramp rules, and integrity/size metadata.
- Safe relative paths and identity linking those assets in the manifest.

Original phase can be regenerated deterministically from the lossless original and frozen transform contract. Persist analysis coefficients only if a demonstrated reproducibility/performance requirement warrants the extra storage. Do not analyze the edited compatibility WAV and apply the saved gains again.

The session snapshot must capture original source, spectral and envelope state, combined edit revision and composed edited PCM from one coherent revision. Compose the broadband envelope exactly once when producing WAV output. Finish/render that captured revision off audio before staging assets, even if a live preview is behind. Ongoing later strokes may continue but do not alter the captured save.

Extend IoSessionExporter staging so every referenced asset is finalized before publishing the manifest. Reuse sidecar path validation, bounded parsing and corruption handling from JamFile/NativeMidiSidecar. Reject bad sizes, non-finite gains, unsupported transform versions and path traversal.

On load, restore original plus spectral gains and broadband envelope, and rebuild edited playback off callback. Gate-only playback must retain the direct-original unity path. If old native assets have no envelope field, default it to unity. If the spectral sidecar is missing/corrupt, load the edited WAV with a clear warning and disable original-based editing for that asset; never silently treat the edited WAV as its original. Prefer the compatibility edited WAV until reconstruction is ready. Keep legacy sessions unaffected.

ExportSamples must distinguish original preservation from edited output explicitly. Update ring-envelope refresh to reflect edited audio after revisions are accepted. Bounce/overdub use the common processed Loop read path and therefore bake the edited source into newly recorded material; that new Loop begins as a new original with unity gains. The source Loop retains its own edit state.

A destructive writer must invalidate or explicitly replace spectral source identity before writing. Normal punch uses a fresh destination through the common edited Loop read path, so no partial STFT-gain clearing or source edit-state transfer is needed. Preserve existing punch timing and mix ownership. No job may continue reading a mutable original. Recheck source dependencies and active bounce readers during that transition.

## Implementation sequence and exit criteria

1. **Confirm the source/layout and eligibility contract.**
   Trace `Loop::ReadBlock`, `ExportSamples`, `EndRecording`, body phase, Hanning seam contributors, and bounce reads. Confirm the logical Loop body starts at `constants::MaxLoopFadeSamps`, while `PhysicalLoopLength()` describes recorded samples beyond the logical boundary; specify which samples are original source, fade context, and preserved physical tail in the native sidecar. Define a stable-source acquisition protocol for completed, non-writing Loops, including invalidation if capture/punch/overdub resumes while a source copy or job is pending. Specify whether a currently playing Loop is safe to snapshot and edit without touching callback-owned mutable buffers. Add focused fixtures for body/prefix/tail and unequal logical/physical lengths. Exit: source-coordinate, sidecar-layout, and eligibility contracts are written down and testable; no concurrent read/write of the recording `BufferBank` is assumed safe.

2. **Implement analysis and synthesis in isolation.**
   Verify the candidate FFT library's pinned vcpkg port, license, API, supported platforms, and actual JammaLib link/build integration before adding it to `vcpkg.json` and project/filter files; the current manifest has no FFT dependency. Then add STFT geometry, phase-preserving gain field, weighted overlap-add, absolute broadband envelope evaluation/ramps and limits. Keep the transform contract/version explicit. Exit: focused native tests establish no-op round-trip tolerance, silence, zero-bin boost behavior, short/non-hop-aligned loops, seam/context handling, and finite limits before engine publication depends on the DSP types.

3. **Implement non-destructive engine publication.**
   Add Loop-owned source identity, processed immutable playback snapshots and independently updatable coherent envelope snapshots, bounded publication/acknowledgement, off-audio retirement, source validation and old/new transition. Use the existing `base::PublishedAudioSnapshot` contract where it fits: callback readers borrow immutable values; load/store/exchange/collect and last-owner destruction stay off callback. Integrate the common read path, exact original-unity bypass, per-contributor seam gain coordinates, and an explicit original-versus-edited export accessor. Do not add mutable PCM reads that can race capture or destructive writers. Exit: playback phase/length/offset/anchors do not change; stale work cannot publish; cancellation/teardown retire safely; preview can be heard while playing.

4. **Implement the fixed spectral renderer.**
   Add spectral model, 2D texture, shaders and ResourceList entries. Implement morph, compressed height/palette, exact black silence and local playhead. Exit: no per-edit mesh rebuilding, correct seam orientation, close animation and graceful resource failure.

5. **Wire interaction and Undo/Redo.**
   Extend audio editor mode with precise surface picking, the fixed spectral brush, held-time updates, and Alt + left full-frequency bar with frozen Y anchor, absolute displacement transfer and horizontal time mapping. Add temporal feathering against the captured before-state, per-stroke strongest-influence dominance, swept-segment interpolation, wrapped strokes, pointer ownership and one history entry per stroke. Exit: ordinary stationary holds accumulate, stationary bar holds do not, bar downward/anchor/upward positions give silence/unity/boost, a nearby trough does not progressively erode an authored peak, a new pass restores previously muted times, background orbit matches MIDI, right-click does not mute, and stale jobs cannot overwrite Undo or the latest envelope.

6. **Persist and integrate all audio consumers.**
   Add bounded lossless sidecar codec, optional manifest metadata, coherent exporter snapshots and importer restore/fallback. Snapshot one validated edit revision for both compatibility WAV and native sidecar; publish the manifest only after every asset is staged. Verify bounce/overdub, waveform overview, effects routing and WAV output, checking the current consumer paths so editing does not bypass LoopTake/Station processing. Exit: save/reload preserves original, spectral edits and broadband envelope (including gate-only edits), every consumer hears the selected composed revision, old/unmodified sessions retain current behavior, and gain is never applied twice.

7. **Profile and complete validation.**
   Measure representative loop sizes and continuous painting. Implement dirty-frame/chunk reconstruction if complete snapshots miss practical responsiveness. Audit hot paths and lifecycle repeatedly during integration. Exit: bounded work/memory, no callback regressions, published measured latency, and all acceptance checks below.

## Validation and acceptance

Native tests should cover behaviour, not mirror implementation details.

- DSP: unmodified round-trip tolerance; exact-zero silence; zero bins remain zero under boosting; magnitude gain and original complex direction; finite bounds; DC/Nyquist; tonal/transient fixtures; short and non-hop-aligned lengths; arbitrary sample rates; seam/context overlap and normalization.
- Gesture: elapsed-time independence from event/frame rate; longer stationary hold gives more gain; falloff and path continuity; time seam wrap; clamped frequency; saturation/no-op history; a stroke followed by Undo/Redo restores gain content and sound.
- Broadband workflow: held-chord fixture; finite downward range reaches exact zero; anchor gives exact unity; upward movement boosts and clamps; repeated stationary application does not compound; vertical-only movement paints the same time; horizontal sweeps cover gaps; modifier/anchor capture; DPI and inverted mouse-Y conventions; vertically off-surface gain control; ill-conditioned time projection; touched-only replacement and a complete second pass reopening previously muted plateaus. Compare reopened internal PCM against original samples exactly after transition/ramp boundaries; verify zero plateaus, seam ramp behaviour and separate spectral layer semantics.
- Bar feathering: verify w = 1 at cursor time, smooth monotonic decline with distance, zero outside support and wrapped seam support. Draw a boosted peak then a nearby sharp trough: assert the peak centre retains its authored target, weaker trough-tail updates cannot overwrite it, and the trough centre reaches its own target. Repeated holds/tail overlaps must not progressively average or change the result. Test vertical-only target changes, deliberate full-strength retracing, equal-influence tie resolution, fresh dominance state on the next stroke, unchanged untouched times, cancellation/Undo, and identical swept results under different pointer event densities. Verify anti-click ramps preserve authored extrema and do not become a global smoothing pass.
- Publication/lifetime: stale source and edit jobs; monotonically applied revisions; continuous holds without starvation; latest-value coalescing; snapshot retirement after transition; ditch/delete/reset; sample-rate changes; source writer invalidation; save while preview is behind; a late spectral reconstruction cannot overwrite a newer broadband envelope.
- Engine: unchanged local phase/length/offset/anchors when adopting edited content; existing seam fade preserved; no double time shift; bounce/overdub and export match the edited source; unaffected sibling channels remain unchanged.
- Persistence: original floats and gains survive a round trip; old manifests; native sidecar corruption/truncation/oversized values/path traversal; coherent combined spectral/envelope revisions; gate-only assets; exact zero/unity preservation; compatibility-WAV fallback; no double application of spectral or broadband gains.
- Graphics/manual: silence is flat black with no guides/bands; palette tracks height; ring unravels smoothly; cyan line crosses peaks at correct Loop phase; raised ridges pick correctly from oblique angles; empty background orbit matches MIDI; full-frequency bar remains anchored independently of mouse Y and surface height; gain labels/preview match current target; resize/context/resource teardown.
- Performance: measure source-copy/analysis startup, brush-to-display time, synthesis time, ready-to-audio adoption, frame time and memory on short musical loops, multi-minute loops, multiple active takes and supported device configurations. Report numbers and limits; do not promise an unmeasured universal latency.
- Real-time review: no allocations, waits, locks, FFT work or final large-object destruction in callback-owned paths. Run the threading-review audit and manually inspect affected callback bodies, including ReadBlock even though it is not individually listed in the policy checklist.

Before every future build/native-test run, reread local .vscode/tasks.json. Use its installed tool and applicable arguments, incremental Build, affected JammaLib and native-test projects, and Jamma when integrating rendering/resources. Direct project builds require the absolute SolutionDir with one trailing backslash. Use doc/build.md environment preflight/troubleshooting; do not edit machine-local tasks or guess tools. Run relevant native suites, then the full native target for integration regressions.

This plan review made no feature-code changes and ran no builds or tests. Implementation validation remains to be performed.

## References for DSP/dependency decisions

- KissFFT upstream API and license: https://github.com/mborgerding/kissfft and https://github.com/mborgerding/kissfft/blob/master/COPYING
- STFT inverse/window normalization reference: https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.ShortTimeFFT.html

These references inform the C++ design; SciPy is not a runtime dependency.
