# Mirror-ball graphics: remaining validation

The feature implementation is committed on `feature/mirrorball-graphics` in `f515010`, `2019005`, `023f135`, `78517d5`, and `17a8d96`. The original implementation plan and reviewed corrections remain in those commits. No further code change is identified by the final static review.

## Live appearance and shader loading

1. Launch the newly built Debug x64 `Jamma.exe` from this worktree in an interactive desktop session. Check shader compile and link diagnostics during startup. The C++ build copies GLSL files but does not compile them, and no standalone GLSL validator was installed in the review environment.
2. Rotate the camera around MIDI notes, selection discs, a waveform, and a station. Confirm the supplied BMP probe is visible on waveform top and bottom faces, the neutral station caps and bevels have smooth highlights, and the dark jagged ring surrounding the green state shape now shares that shading. Confirm the green surface and level cylinder retain their original cues. Check short MIDI notes and full-circle disc seams.
3. Check scene, picker, and highlight passes, and recording, muted, and hover states. Pay particular attention to waveforms at zero or near-zero visual height, where the corrected probe basis should remain stable.

## Audio and render load

1. Repeat a recording session using the same audio device, sample rate, buffer size, plug-ins, and visual density as the run that showed dropouts. Record whether dropouts occur during recording and after it ends. Compare with the parent branch under the same conditions if they recur.
2. If dropouts are repeatable, capture audio underrun counts and render frame time/CPU/GPU load. The code review found no new audio-callback work, locks, or allocations; the relevant added load is the MIDI mesh (272 versus 132 triangles per shared arc instance, drawn twice in the scene) and one extra fragment probe read on visible material surfaces. Reduce measured render cost first, without changing audio-thread synchronization or waveform upload size.

The final review could launch the executable, but the available computer-use window inventory did not expose its window. Therefore the live visual and recording checks above remain open.
