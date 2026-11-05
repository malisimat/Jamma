# Global playback rate

The Timing panel's purple Playback rate control ranges from 0.5 to 2.0,
logarithmically around 1.0. Recorded audio changes pitch with speed. MIDI notes
keep their pitch; their timing and recorded parameter automation follow the
same source trajectory as audio. Live instruments still process device blocks.

Each independent take remembers the applied rate at its effective recording
start. Its local source speed is applied global rate divided by that recording
rate. Overdub and punch inherit the source take's ruler. For example, material
recorded at 0.5 plays at its original speed at global 0.5 and twice that speed
at global 1.0. The audio device sample rate, physical timestamps, latency
compensation, and network cadence do not change.

Rate and audible phase edits converge smoothly. Multiple output readers replay
one block descriptor and commit progress once. Child loops retain independent
lengths and phases. The local master retains its own recording-rate reference
and full BPI; odd BPI is not reconstructed by rounding the grain.

## Server timing

Editing the rate leaves server follow. Returning to 1.0 stays local. Sync to
server is a separate explicit action. While following, server geometry owns
source speed; the retained local rate is used when leaving follow. Aligning
means the audible phase is still converging.

Request server BPM/BPI asks the server to adopt the displayed local tempo and
full BPI. It is available only with a settled local rate and an applied master.
The displayed BPM uses the protocol's three-decimal precision. Sending is not
acceptance: a fresh observation must match both BPM and BPI. A newer rate edit
supersedes the request.

The prepared reader admits base source rates from 0.25 to 4.0, with positive
phase correction bounded to 25 percent. Remote geometries can exceed this
range (the protocol domain can reach 640); admission checks all local takes
before changing authority. Unsupported follow remains local and reports the
reader limit. It never silently clamps a take and reports synchronization.

## Audio quality and saved sessions

Exact unity uses the existing direct/interpolated reader. Downsampling uses a
prepared 128-tap Blackman-windowed FIR with fractional phase and rate tables.
Its conservative transition band reduces high-frequency bandwidth at large
ratios. Filter activation during playback is smoothed. This is pitch-changing
resampling, not pitch-preserving time stretch.

The reader uses contiguous source spans and a fixed, stack-local material cache
for seams and spectral content. Each reader reconstructs that cache from its
own block head; it adds no delay and does not advance shared playback state.

JAM metadata stores the global target, master recording rate, and each take's
recording rate. Missing fields mean exactly 1.0. Malformed or out-of-range
rates also default to 1.0 with a load diagnostic. Saving does not resample
stored WAV material. A restored session starts directly at its saved target;
transient ramps and request identities are not saved.

Session export writes source media; this codebase has no offline mixed-audio
renderer with a separate rate-dependent duration. Live output uses the rendered
trajectory once, including outgoing network audio.

## Manual checks

Use a disposable session and test both Debug and Release builds.

1. Record a loop at 1.0. Sweep to 0.5 and 2.0 and reverse direction mid-ramp.
   Check pitch, smooth transitions, consistent playhead/click, and no clicks.
2. Record a second independent take at 0.5. At global 0.5 it should sound at
   its recorded speed; at 1.0 it should play twice as fast. Test stereo audio,
   MIDI notes/pedals and automation, then overdub and punch during a ramp.
3. Use different child lengths and offsets. Change global phase and child
   SHFT, including across wrap, while changing rate. Check their independent
   relationships and eventual settling.
4. Save during convergence with nonzero SHFT, reload, and compare phase,
   lengths, rates, MIDI and automation. Load an older JAM without rate fields.
5. With odd BPI, select a master, Reclock, tap, and restart the device at a
   non-unity rate. Check tempo/BPI, completed takes and first new recording.
6. Connect to a test NINJAM server. Follow, edit rate to detach, return to 1.0,
   and confirm it stays local. Request the displayed BPM/BPI, then exercise
   partial acceptance, a newer rate edit, disconnect/reconnect and explicit
   Sync. Check the remote click and incoming audio remain on server timing.
7. Stress short loops and many channels at the largest relative rate, with
   active VST racks and capture. Check underruns and held notes. Listen to
   sweeps, bass, vocals, percussion and stereo-correlated material.

Automated evidence and remaining manual checks are recorded separately in
[the implementation review](reviews/global-playback-rate-status.md).

From the repository root, run the native regression suite with:

```powershell
Get-Content -Raw .vscode/tasks.json | Out-Null
& .\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe
```

The opt-in renderer benchmark should use Release, with other heavy work idle:

```powershell
Get-Content -Raw .vscode/tasks.json | Out-Null
& .\test\JammaLib_Tests\bin\x64\Release\JammaLib_Tests.exe '--gtest_filter=LoopPlaybackRateTest.DISABLED_FixedFilterCpuBenchmark' --gtest_also_run_disabled_tests
```

Its printed durations must be compared with the printed callback deadline;
Google Test success alone does not establish adequate performance. The benchmark
measures rendering, not a complete callback with capture, plugins and networking.
