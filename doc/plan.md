# Mirror-ball graphics: remaining validation

The original feature implementation is committed on `feature/mirrorball-graphics` in `f515010`, `2019005`, `023f135`, `78517d5`, and `17a8d96`. Master's loop editor has since been integrated using the [merge plan](mirrorball-merge.md). That document is a historical integration record; later commits changed waveform interaction shading, MIDI source handling, glow and console startup. Current review findings and the subsequent mute interaction changes are below. Full interactive appearance and recording-load checks remain open.

## Adversarial PR #134 review (2026-10-04)

Reviewed feature head `9c0c91a` against master `868a209`, then applied these fixes:

- **Selection correctness:** the latest waveform saturation code cancelled its selection gain on neutral material. GL readback reproduced identical idle/selected and hovered/selected-hovered muted waveforms. Reduce neutral-floor removal and reserve peak brightness for hover on selected waveforms. Selected waveforms retain their level hue and orange pressed treatment. Controlled GL readbacks now distinguish the five states for representative playing, recording and muted colours, including black and white input textures; minimum measured channel separation was 11/255. This is representative coverage, not a guarantee for every camera/light/editor combination.
- **Render cost:** skip the highlight draw and both full-screen blur passes with an empty selection. Pair adjacent blur taps through linear filtering and precompute weights: 21 texture samples per axis instead of 41, with the same radius-20 kernel. Picker textures still use nearest filtering. Random RGBA texture readback, including image edges, differed from the previous kernel by at most 1/255 per channel on each axis. MIDI scene draws now submit the notes and backing disc in their respective passes rather than drawing both meshes twice and discarding the unwanted fragments. Instanced note identities and wrap copies are preserved.
- **Stale test:** the MIDI master press-timing test omitted a recording source after `c0a0b12` made source-less triggers audio-only. Supply `Keys` in both the start action and recorded event; retain every timing assertion. The audio-only recording/overdub regression remains in place. Added a selection-query regression covering MIDI-only model selection and clearing, take selection and station selection.
- **Stale comments/docs:** correct the skybox update-method comment and station bottom-face UV description; mark the earlier merge plan and colour measurements as historical.

### Thread and audio review

| State | Owner / readers / writers | Synchronisation and teardown |
| --- | --- | --- |
| Blur framebuffer, fullscreen texture/direction and GL uniforms | Render thread reads/writes; audio and job threads do not use them | Render-thread resize/recreation and `ReleaseGlResources`; framebuffer/texture destruction uses the existing GL delete queue |
| MIDI geometry draw mode | Written by render-thread `Draw3d`, read by its `DrawMesh` call | No cross-thread access; reset at each draw and destroyed with the model |
| Console `ActivateOnLaunch` | Initialized by the UI before launch, read by broker worker | Immutable `const bool` in shared broker state; existing stop-event/join lifecycle retains the state until the worker finishes |
| Selection and station/take/MIDI membership | UI/render presentation reads; job structural edits write | Existing `_sceneMutex` serializes render traversal with structural trigger processing; the public selection query takes it once and its private helper runs under that lock |
| Audio hierarchy / MIDI visual publication | Existing audio readers and job writers | Existing immutable snapshots remain unchanged; no new synchronization in callback-owned bodies |

Manually inspected the affected callback paths and their publication boundaries: no new audio callback locks, waits, allocations, logging or hierarchy scans. The threading audit script flags the new `HasSelection` scoped lock because it scans all of Scene.cpp; this is a false positive for a UI/render query, which is not called from an audio path. Render/job contention on the existing scene mutex and the extra GPU work still require recording-load measurement. Do not infer dropout-free performance from callback inspection.

### Completed validation

Incremental Debug x64 JammaLib, Jamma and native test builds succeeded with no warnings or errors. The full native suite passed: **1,179 passed, 1 hardware-dependent MIDI-device test skipped** (1,180 total). All **22 shader programs compiled, linked and validated** in a hidden NVIDIA OpenGL 4.6 / GeForce GTX 960 context. Controlled fragment readbacks covered waveform interaction states both with and without an active selection, MIDI held velocity colours and selection-disc feedback, and the blur-kernel comparison described above. These GL checks used temporary verification harnesses; they are not part of the native test target and do not establish full-app gesture/picker or recording-load behavior.

## Live appearance and shader loading

1. Launch the newly built Debug x64 `Jamma.exe` from this worktree in an interactive desktop session. Check shader compile and link diagnostics during startup. All 22 shader programs have passed compile/link/validation in a real hidden NVIDIA GL context; full-app startup and interactive appearance still need confirmation.
2. Rotate the camera around MIDI notes, selection discs, a waveform, and a station. Confirm the supplied BMP probe is visible on waveform top and bottom faces, the neutral station caps and bevels have smooth highlights, and the dark jagged ring surrounding the green state shape now shares that shading. Confirm the green surface and level cylinder retain their original cues. Check short MIDI notes and full-circle disc seams.
3. Check scene, picker, and highlight passes, and recording, muted, and hover states. Pay particular attention to waveforms at zero or near-zero visual height, where the corrected probe basis should remain stable.

## Audio and render load

1. Repeat a recording session using the same audio device, sample rate, buffer size, plug-ins, and visual density as the run that showed dropouts. Record whether dropouts occur during recording and after it ends. Compare with the parent branch under the same conditions if they recur.
2. If dropouts are repeatable, capture audio underrun counts and render frame time/CPU/GPU load. The code review found no new audio-callback work, locks, or allocations. Added load includes the MIDI mesh (528 versus 260 triangles per shared 32-segment note instance), material probe reads, another full-size RGBA framebuffer, and two full-screen blur passes while a selection is active. Notes are now submitted only in their note pass; each blur axis uses 21 samples. Measure render cost under dense recording and selection before further changes to mesh density or framebuffer resolution.

During the earlier integration review, the executable launched but the available computer-use window inventory did not expose its window. The current review used controlled GL readbacks and native tests. The live visual and recording checks above remain open.

## Mute presentation and paint gestures (2026-10-04)

- Middle-button down previews a blue press. A click commits only its pressed target on release. Moving four pixels starts a stroke with the direction chosen from the starting target: mute or unmute. The starting target and each touched target change immediately; preview and hover are suppressed throughout painting, then hover returns on release. Capture loss cancels a pending click and preserves already painted changes. Untouched takes retain their states.
- Muted waveforms, MIDI note events and their backing/picker rings use a dark blue-grey palette. Hover and selection still have distinct brightness within that palette after release. MIDI streams share their owning take's mute state. Station-depth gestures mute/unmute all takes; a station displays muted when it has takes and all are muted. Picker ID shaders remain unaffected by presentation colours.
- Presentation mute flags and encoded press values (0 released/painting, 1 left press, 2 middle press) are UI/render-owned model state, read and written during the existing scene-locked UI/render traversal and destroyed with the models. Audio reads the existing atomic engine mute flags. UI target lookup and membership traversal use the scene mutex; no callback body, audio publication scheme, texture fetch count or render pass count changed. The threading audit flags three scene locks because it scans the whole file; manual inspection confirms these are UI helpers, not callback calls.
- Incremental Debug x64 JammaLib, Jamma and native test builds succeeded. Full suite: **1,183 passed, 1 hardware-dependent MIDI-device test skipped** (1,184 total). New tests cover click preview, drag threshold/direction, capture loss, immediate MIDI/audio paint, sibling-hover suppression, hover restoration, untouched takes and station aggregate toggling. All 22 shaders compile/link/validate in the real hidden NVIDIA GL context. Controlled readbacks distinguish all six idle/hover/selected/selected-hover/left-press/middle-press states for muted waveforms, MIDI notes across five velocities, MIDI backing discs, station caps/bevels/side walls and both station ring materials. Full-app visual and recording-load checks above remain open.
