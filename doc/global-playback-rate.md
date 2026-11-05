# Global playback rate

Each independent take keeps the applied global rate at its effective recording
start. Its source speed is the current applied rate divided by that recording
rate; overdubs and punches inherit the source take's reference. Independent
recordings store raw device samples, including their MIDI timing. Changing rate
during an independent recording does not reshape that capture to keep it in
time; its reference remains the rate at capture start. Audio changes
pitch with speed, while MIDI note pitch stays unchanged. Device sample rate,
physical timestamps, latency compensation, and network cadence do not change.

Rate and phase edits converge smoothly. Multiple output readers use the same
block trajectory, with progress committed once. Child loops keep independent
lengths and phases, and the local master retains its recording-rate reference
and full BPI.

Playback uses two-sample linear interpolation. Overdub and punch input is
linearly mapped onto the inherited source grid with the existing latency
alignment. There is no anti-alias FIR: speeding material up or mapping input
onto a slower source grid can alias high frequencies. This is the deliberate
quality/performance tradeoff for predictable callback cost. Settled unity
playback retains exact integer advancement; rate and phase ramps retain their
shared fractional trajectory.

Editing the rate leaves server follow; returning to 1.0 does not rejoin it.
Sync to server is explicit. While following, server geometry determines source
speed, and the retained local rate resumes when leaving follow. Accepted server
authority can be Aligning until audible phase converges. Requesting server
BPM/BPI is separate from following: a fresh server observation must match both
values, and a newer rate edit supersedes the request.

The prepared reader supports base source rates from 0.25 to 4.0, with positive
phase correction bounded to 25 percent. Remote geometry is checked against all
local takes before authority changes; unsupported follow stays local and
reports the limit.

JAM metadata stores the global target and recording-rate references. Missing,
malformed, or out-of-range rates default to 1.0; invalid values also produce a
load diagnostic. Saved WAV material is not resampled, and transient ramps and
server request identities are not saved.
