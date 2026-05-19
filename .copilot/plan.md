# Feature: waveform Vertex shaders

## Summary
Render LoopModel waveform envelope using vertex shaders, rather than regenerating geometry during recording. Fixed number of segments, but vertex shader used to transform vertical positions instead of vertex positions in the mesh. Also vertex shader controls diameter and thickness dynamically. This should be much more performant compared to the current way, since currently we rebuild the full mesh according to the audio buffer contents, periodically in a background thread.

## Goals & Acceptance Criteria
When waveform is perfectly represented on screen (for an audio buffer with non-zero samples) despite a fixed cylindrical mesh being provided as geometry.

## Scope

### In scope
LoopBuffer rendering, possibly also some scaling or shader parameter adjustment from within Loop class. Anything OpenGL context setup that requires enabling vertex shader support, and related plumbing.

### Out of scope
Must not change audio content, or the public API of Loop class. No change to the overall graphics or audio tech stack.

## Proposed Approach (Refined via Adversarial Review)
Using a 1D Texture paired with Pixel Buffer Objects (PBOs) for async upload is the optimal approach for performance while guaranteeing linear interpolation of the waveform across segments.

1. **Fixed Single Mesh**: Generate a fixed cylindrical mesh once, with vertex U-coordinates representing position along the cylinder. No instancing.
2. **1D Texture for Envelope Data**: Use a dynamically updated 16-bit float (`GL_RG16F`) 1D texture for min/max values. Linear filtering automatically interpolates envelope values between segments for free.
3. **Decimation C++ Routine**: `LoopModel::DecimateWaveform` function to reduce the `BufferBank` to min/max per segment.
4. **Asynchronous Transfer**: Map a PBO, run the decimation routine writing into the mapped memory, unmap, and use `glTexSubImage1D`.

## TDD / Staged Implementation Plan (Red/Green/Refactor)

### Stage 1: Fast Audio Data Decimation & State (TDD)
- **Test**: Write Google Tests verifying `LoopModel::DecimateWaveform` takes `audio::BufferBank`, desired length/offset, and segment count (target 2048), converting chunks of samples into an array of min/max `vec2` values. Test texture conversion and PBO logic safely isolated from window creation.
- **Fail (`Red`)**: Outline function, test fails. *(CURRENT STATE)*
- **Pass (`Green`)**: Implement performant decimation.
- **Refactor**: Remove allocations from loop paths; use a pre-allocated internal buffer or direct PBO mapping.

### Stage 2: Shaders & Plumb-through
- Add `waveform.vert` and `waveform.frag` to `Jamma/resources/shaders/`.
- Ensure new shaders support a uniform 1D texture sampler, along with existing params.
- Add shader to `ResourceList.txt` and ShaderBank.

### Stage 3: Initializing the Fixed Mesh & GL State
- Modify `LoopModel` constructor/setup to pre-allocate a fixed cylindrical mesh based on a static max-resolution segment count (2048).
- Generate standard UV mapping where the U-coordinate defines the normalized position.
- Initialize `GLuint` handles for the 1D Texture and PBOs.

### Stage 4: Execution Pipeline Integration
- In `LoopModel::UpdateModel`, perform the data decimation directly into mapped PBO memory instead of rebuilding vertex arrays.
- In `LoopModel::Draw3d`, bind the new shader, bind the 1D texture, and render the static mesh.
- Introduce the throttle / configurable update rate logic.

## Risks & Open Questions
- **Data Transfer**: Confirmed to use PBOs for asynchronous texture sub-image uploading to ensure robust real-time performance.
- **Segment Count**: Confirmed target of 2048 or more segments to ensure high visual fidelity.

## TODOs
- [x] Stage 0: Plan and Architecture Review
- [x] Stage 0.5: TDD setup and Failing basic state
- [ ] Stage 1: Implement `LoopModel::DecimateWaveform` and PBO structure using TDD
- [ ] Stage 2: Author `waveform.vert` and `waveform.frag`
- [ ] Stage 3: Wire Fixed Mesh and Resource Initialization in `LoopModel`
- [ ] Stage 4: Texture Uploading and Draw logic in `LoopModel`
- [ ] Stage 5: Final Polish, Throttle Rate adjustments, and Profiling test.
