# Mirror-ball graphics: remaining work

## Target

Finish the camera-space light-probe treatment introduced in `f5150101bfbd87c2d09a77bc11e8b050e09a8882`. MIDI notes already show the effect in a live run. Keep their velocity colours, the waveform's amplitude texture and vertical colour scaling, all recording/mute/hover/selection/picking cues, and the existing station level cylinder and coloured state rings. Audio reliability takes priority over visual detail.

## Phase 1 — targeted material correction

1. Replace `probe_pearl.tga` with an uncompressed TGA converted by ImageMagick from the supplied `DefMat_Sphere2b.bmp`. Put the BMP in the resource source area so regeneration is reproducible. Replace `probe_amber.tga` with a distinct complementary treatment derived from the BMP using a short, documented ImageMagick command. Keep `probe_chrome.tga` for colour preserving overlays and tinted MIDI notes. Remove or revise the synthetic generator so it cannot silently overwrite the new art. Preserve the app loader's expected TGA orientation, format, and texture registration.
2. Restrict station probe shading to the neutral top and bottom caps and their bevels. Restore the pre-feature shading of the tall level cylinder and coloured state rings exactly. The dark ring occluders may remain neutral but need no probe if doing so adds a full fragment sample to the ring shader. Keep picker and highlight behaviour intact.
3. Smooth the station cap/bevel lookup normals around the circumference, while keeping flat top/bottom normals where appropriate. Prefer a small procedural normal calculation from the existing position/profile in the station vertex shader or a one-time normal-buffer change; do not add per-frame geometry rebuilding. Avoid smoothing across hard top/bevel/side boundaries.
4. Make the waveform probe visible on its top and bottom surfaces as well as the walls. Inspect the current normal direction and BMP latitude, then tune the existing shader normal/lookup or restrained blend so its surface receives visible variation. Keep waveform geometry and uploads unchanged, along with the amplitude colour texture, vertical scaling, highlights, and states. Avoid additional texture reads per fragment if one probe lookup suffices.
5. Keep MIDI changes surgical. Fix any concrete shader, texture-binding, or seam correctness issue found during review, while preserving the observed MIDI effect.

Review and commit phase 1 on `feature/mirrorball-graphics` only when the diff is focused and correct.

## Phase 2 — integration and performance audit

1. Trace every change from `f515010` through the render thread and audio callback paths. Check allocation, locks, resource lifetime, and cross-thread ownership. Verify that probe loading and conversion happen during initialization, never during rendering or an audio callback. Compare waveform buffer upload counts and MIDI instance payloads with the parent commit. Audit the new per-draw uniforms, extra GL binds, fragment samples, mesh triangle count, and large station pixel coverage as possible CPU/GPU contention while recording. Remove unnecessary draw-pass work and prefer lowering cost in the existing shader over caching machinery or new synchronization.
2. Validate resource names, uniforms, TGA headers and loader compatibility, texture units/restoration, and scene/picker/highlight paths. Check projection/view/model ordering and camera-space normal transforms. Review the MIDI end caps and outward winding. Correct any demonstrated issue.
3. Before every build or native-test run, reread local `.vscode/tasks.json`. Use its installed MSBuild path, incremental Build, the affected project, and an absolute `SolutionDir` with exactly one trailing backslash for direct `.vcxproj` builds. Build `JammaLib`, native tests if C++ behavior changes, and `Jamma` for shader/resource packaging. Run relevant native tests after behavior changes. Compile GLSL in an actual OpenGL context or a suitable validator when available.
4. Compare the result in the running app at multiple camera angles: MIDI notes/discs, waveform top/bottom/walls and recording/mute states, station caps, level cylinder, state rings, picker and highlights. Exercise recording while observing audio dropouts and frame pacing. If repeatable measurements are unavailable, report that limit explicitly rather than claiming performance is proven.

Review and commit phase 2 corrections separately. Do not add dependencies, change audio-thread synchronization, or expand mesh/upload size to solve a shader appearance issue.

## Final review and handoff

Review all commits against the original feature and the live-run notes, including thread safety and render state. Resolve required findings before the final commit. Create a concise HTML report in the system temp directory that lists the changes, evidence from builds/tests/live inspection, and any remaining concerns; open it in the browser.
