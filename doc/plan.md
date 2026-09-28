# Mirror-ball graphics spruce-up plan

Status as of 28 September 2026. This document describes the work in `feature/mirrorball-graphics` at `Jamma.worktrees/Jamma-mirrorball-graphics`. The implementation is in progress and uncommitted. The application has not yet been visually verified with these changes.

## Intention

Give selected 3D objects a material similar to a ZBrush matcap. A fragment samples a circular TGA light probe using the surface normal transformed into camera space. The lookup coordinate is the normal projected onto the camera backplane: `uv = 0.5 + 0.49 * normalCamera.xy`. The small margin keeps samples within the texture border. Because the lookup follows the camera, highlights should move across the object as the view or object changes.

Apply distinct probes to MIDI note events, MIDI selection discs, audio waveform loops, and stations (including their state rings). Retain MIDI velocity colour, loop recording/mute/hover cues, station state colour, picking IDs, highlight passes, and disc transparency. Keep waveform geometry and upload cost stable: its bevel should be a shader effect. A shared instanced MIDI mesh may gain actual bevel geometry because it is uploaded once rather than once per note.

## Intended rendering design

| Object | Material | Bevel approach | Existing information to preserve |
| --- | --- | --- | --- |
| MIDI notes | `probe_chrome.tga`, tinted by the existing velocity colour ramp | Chamfered shared arc mesh plus normal adjustment in the vertex shader | Velocity, hover, picker and highlight passes |
| MIDI selection discs | `probe_pearl.tga` | Same instanced arc mesh | Alpha, hover, end-cap seam suppression, picker and highlight passes |
| Audio waveform loops | `probe_pearl.tga` alongside the existing `levels.tga` colour texture | Pseudo bevel from interpolated height and radial edge coordinates in the fragment shader; no extra waveform vertices | Recording/mute state, waveform colour, hover, picker and highlight paths |
| Stations and state rings | `probe_amber.tga` | Existing station/ring geometry; probe-driven shading | Station level, state colour, selection and picker passes |

The probe generator in `Jamma/resources/generate_mirrorball_probes.py` evaluates softbox lighting against a reflected view vector on a virtual sphere, then uses ImageMagick to write uncompressed 512 × 512 RGBA TGAs. This is a synthetic studio probe rather than a capture of a real environment. The three generated files are present in the worktree but are not yet committed; regeneration requires Python with NumPy and Pillow plus ImageMagick on `PATH`.

## Completed in the worktree

1. Created the worktree from the main checkout with the `jamma-tree` skill on branch `feature/mirrorball-graphics`; copied local `.vscode` and `.agents` contents into it without replacing files.
2. Added `probe_chrome.tga`, `probe_pearl.tga`, and `probe_amber.tga` in `Jamma/resources/textures/`, registered them in `Jamma/resources/ResourceList.txt`, and added the reproducible generator. ImageMagick identifies all three as 512 × 512 uncompressed TGA files. A PNG preview of the chrome probe was inspected and then removed.
3. Split the scene's projection and view entries in the MVP stack and added a `ModelView` uniform derived from the remaining stack entries in `GlDrawContext`. The product used for `MVP` remains projection × view × model. Updated the material vertex shaders to transform normals with `ModelView` before the circular lookup.
4. Updated MIDI shaders to sample separate note and disc probes. Notes multiply the probe response by the pre-existing velocity colour ramp. The shared `graphics::MidiModel` arc mesh now uses an eight-edge chamfered cross-section and triangulated end caps, with UV end-cap markers retained for full-circle disc suppression. Instance data size was not changed.
5. Updated waveform shaders to blend a pseudo bevel normal near vertical and radial edges and sample the pearl probe. The existing fixed waveform mesh and 1D amplitude texture upload scheme were left at their previous size. `LoopModel` binds the colour texture, waveform data texture, and probe on texture units 0, 1, and 2 respectively.
6. Updated station body and state-ring shaders to sample the amber probe; `StationModel` binds that texture for its draw calls. The station's existing mesh is unchanged.
7. Updated the audio loop model defaults and the restored MIDI-loop texture override to request the relevant probes. Added `GraphicsMidiModel.SharedArcMeshHasChamferedCrossSectionAndMatchingUvs` to the native test project.

## Verification completed

- The incremental Debug x64 `JammaLib.vcxproj` build succeeded after the shared MIDI mesh change.
- The incremental Debug x64 `JammaLib_Tests.vcxproj` build succeeded.
- The focused `GraphicsMidiModel.*` native test passed (one test).
- `git diff --check` reported no whitespace errors. Git printed line-ending conversion warnings only.
- The three TGA files were generated and identified by ImageMagick; one chrome preview was visually inspected.

These checks establish C++ compilation and mesh layout, but they do not establish GLSL compilation, correct resource loading in the running app, or visual quality. A `Jamma.vcxproj` build was started next but the tool call was aborted before it returned a result, so it must be rerun. The presence of an existing `Jamma.exe` in the worktree is not evidence that it contains the current changes.

## Remaining work

### Finish integration and correctness checks

1. Reread `.vscode/tasks.json` before each build or native-test run, as required by the repository policy. Run the configured incremental Debug x64 `Jamma.vcxproj` build with an absolute `SolutionDir` ending in exactly one backslash. Confirm its output contains the new shaders, `ResourceList.txt`, and all three TGAs.
2. Compile the GLSL in an actual OpenGL context or with a suitable GLSL validator. The C++ build does not compile shader files. Investigate and repair any compile/link or missing-uniform errors reported by `ShaderResource` at runtime.
3. Check `ResourceList.txt` uniform registration against every uniform used by the updated shaders. In particular, `station.frag` declares `StationStateColor`, and `StationModel` sets it, but the current `station` resource entry does **not** list that name. This was already absent before this feature; decide whether to include it while validating station colour behavior.
4. Verify every resource path and texture binding in all passes. The MIDI scene pass binds the pearl disc probe on unit 1 and relies on `GuiModel` to bind the chrome note probe on unit 0. Picker and highlight passes should remain deterministic and unaffected by materials. The waveform scene path must restore active texture unit 0 before returning.
5. Inspect the TGA headers against `ImageUtils::LoadTga`, which accepts uncompressed type-2 data, and confirm the texture loads with the expected orientation and alpha. `magick identify` alone does not exercise the application's loader.
6. Review the camera-space normal calculation with rotated MIDI loops, waveform loops, translated/scaled models, station rings, and both camera modes. In particular, confirm the generated MIDI chamfer face normals and cap winding produce outward-facing probe highlights. Check the shader pseudo bevel does not overwhelm the actual MIDI chamfer.

### Visual and performance review

1. Launch the freshly built app and inspect several scenes and camera angles. Confirm chrome notes, pearl discs and waveforms, and amber stations are visibly distinct; verify highlight movement with camera rotation.
2. Check short and long MIDI notes, adjacent pitches, full-circle discs, and end-cap seams. Confirm velocity remains readable after probe tinting and translucent discs do not obscure notes or break selection.
3. Check audio waveform top, inner/outer walls, low-amplitude sections, and recording/muted states. Ensure the pseudo bevel follows the apparent edge without gaps or false bands. Compare waveform vertex count and buffer upload behavior with the base branch.
4. Check station level and visual-state colours, state rings, occluders, and picker outlines. Keep state cues legible after probe modulation.
5. Profile scene rendering with dense MIDI notes and several active waveforms. The probe adds per-fragment sampling; the shared MIDI mesh has more triangles per instance. Adjust probe resolution, mesh profile, or shading cost only if measured frame time warrants it.
6. Run relevant graphics/native tests after any corrections and repeat the app build and visual check. Once stable, review the final diff and commit or open a PR if requested.

## Known scope and risks

- This branch currently modifies shader files in place. It does not yet introduce a separately named generic `mirrorball`/`mirrorball_tinted` shader pair; the MIDI fragment shader contains the tinted variant of the probe lookup. A reusable shader abstraction would need to fit the repo's shader loader, which currently loads one `.vert` and one `.frag` per named resource and has no include mechanism.
- The MIDI mesh gains triangles in one shared static buffer; per-note instance attributes remain unchanged. This still increases GPU work for large note counts and needs measurement.
- The waveform bevel is a lighting illusion. Its geometric silhouette remains the existing square-edged mesh by design.
- Probe textures are synthetic rather than photographed. The generator and palette can be revised after in-app review without changing the lookup contract.
- No live app screenshot, shader compile result, or measured performance result is available yet. Treat the current material settings as a first visual pass.
