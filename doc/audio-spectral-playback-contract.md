# Audio spectral playback ownership

Status: implemented and covered by native spectral/controller/publication tests. Live GPU/input/listening verification remains pending; persistence is deferred. See [current validation and handover](audio-spectral-editor-handover.md). Older validation-history paragraphs below describe the preceding session.

## Punch and bounce

Normal punch starts a fresh overdub destination (Station trigger creates a take; LoopTake::Overdub creates Loops). The source take supplies the existing bounce through Loop::WriteBlock and ReadBlock. User confirmed that this may destructively bake the post-spectral, post-broadband output into the new original. Outside the punched region the audible edited result is retained in recorded samples; the replacement contains newly captured audio according to existing mixing. No edit-state transfer or parallel punch-only source stream is introduced. The source take's non-destructive edits remain its own. New destination edit layers begin at unity.

Loop owns edited reads; LoopTake and Station retain routing, mix and punch timing. No punch flag transition acquires the spectral publication mutex or waits for a worker. Actual destructive writes invalidate the Loop source generation before touching its recording buffer. Obsolete processed reads fall back to the actual buffer; completed source reacquisition can proceed independently after writing ends.

## Committed and audible revisions

AudioSpectralEditState retains the original immutable source, transform configuration, tiled spectral gains and separate broadband envelope, with combined/spectral/envelope revisions. Loop retains committed edits across editor close; untouched unity state remains editor-owned and is released on close. Analysis coefficients need not remain resident on close. Reopening analyzes the retained original rather than an edited compatibility WAV or a second mutable recording copy. Source invalidation discards obsolete retained state off audio.

AudioSpectralPlaybackSnapshot is a complete immutable audible content value: spectral PCM plus independent envelope and their component revisions. Envelope-only updates reuse PCM. Create validates layout and composed finite samples off audio; it neither limits nor renormalizes peaks. Publish methods serialize off audio, reject regressing source/component revisions and reject changed payload handles under unchanged component revisions. Later worker results must compose with accepted newer envelope content rather than overwrite it. Live preview may trail committed edits; persistence must render a captured committed revision in its later integration phase.

## Callback and retirement

The callback uses PublishedAudioSnapshot scalar hazard leases, never atomic shared_ptr operations. Loop holds active and previous leases across blocks. EndMultiPlay advances an existing transition once after all bounce/DAC reads and adopts the newest ready content for the following block. Intermediate publications coalesce while a transition is active. There is no cursor/length/offset/anchor change. Initial transition tuning is 7.5 ms, capped at 48000 samples for finite pathological sample rates; these values are unmeasured.

ReadBlock reads both versions at the same storage indices and blends with one block-relative progress. Both seam contributors receive the same transition progress and the same wrapped envelope gain before the existing Hanning mix. Original/unity-to-original/unity uses copied original floats directly, without envelope/transition arithmetic. Source generation is checked independently by callback reads and adoption.

Lease release only clears a scalar hazard. Owning PCM, gain and envelope destruction occurs in the off-audio publication/CommitChanges collection pump, including after cancelled sources and completed transitions. The owner must stop readers before destruction/exclusive moves; declaration order releases leases before publisher teardown. The only added mutex is off audio. Static hot-path script reports these locks at file granularity; call-path inspection confirms they are not reached by ReadBlock, OnBlockWrite, EndMultiPlay or punch.

## Export boundary

ExportOriginalSamplesQuiescent explicitly exposes the existing raw-bank export contract with stopped callbacks. Legacy ExportSamples delegates to it until session persistence integration is complete. ExportAudioSpectralSamples takes one immutable playback value and applies its broadband envelope exactly once, without reading recording banks. Existing session export still needs to capture/render committed edits and use this edited accessor; it has not yet been switched to preview-based output.
