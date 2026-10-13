# Spectral editor refinement plan

## Goals and constraints

Make brush height evolve smoothly at a rate similar to today's medium-low levels; increase mesh detail, halve its physical height, and add subtle specular lighting. Add wheel brush sizing and MIDI-style navigation, with Alt dragging a time band anchored where the gesture began.

All analysis, editing, synthesis and display preparation remain on the UI/dedicated worker side. Preserve immutable publications, generation/revision rejection, undo/cancel and source lifetime handling. Audio callbacks must gain no allocations, waits or locks. Keep DSP independent of display vertex density and mouse event count. Respect existing transform metadata for already edited sources.

## Phase 1: consistent painting response

- Inspect gain exposure, magnitude normalization and display interpolation together. Current constants use 480 dB/s over a -90 to 0 dB authoring range; mesh height uses -90 to +24 dB.
- Use 60 dB/s centre exposure (2 dB per nominal 30 Hz publication), calibrated to the slower perceived medium-low shoulder response. The current 480 dB/s can jump 16 dB per publication. Keep -90 to 0 dB authoring bounds and exact silence semantics. Interpolate normalized dB heights, rather than linear amplitudes followed by log, so a painted corner does not suddenly dominate its neighbours.
- Retain integrated per-coefficient exposure in seconds, independent of event subdivision and mesh topology. Test quiet/medium/loud response, ceiling/floor and split versus combined exposure.
- Review, build relevant native tests, run them and commit this phase.

## Phase 2: denser and lower surface

- Increase fixed mesh defaults from 512 time by 128 frequency segments to about 1024 by 256. Time segment count stays independent of loop length/sample rate.
- Double FFT size from 4096 to 8192 for new analyses, retaining 1024 hop. Use existing reusable KissFFT plans/scratch, retain memory-budget validation and stored transform compatibility. Coefficients/tiles/input-map costs approximately double; retain admission caps. Test explicit legacy 4096 and default 8192 round trips and run the pipeline profile with measured worker timings.
- Preserve admitted long loops: source preflight/reservation tries 8192/1024, then explicit 4096/1024 if the per-source estimate or session reservation fails. Capture the selected transform immutably with the source request; the UI controller uses this exact transform in retained-layout preflight and analysis. Existing edits retain stored transforms. Display mesh stays 1024x256 in either case; expose fallback in feedback. Test short/preferred, 30s fallback, session-pressure fallback/accounting, both rejected, and reopened legacy edits.
- Halve HeightRadiusScale from 0.50 to 0.25; CPU picking and GPU positions must remain identical. Add a restrained specular cue using surface geometry while retaining level colours and brush/playhead visibility.
- Validate geometry/seam/picking, budget and FFT reconstruction regressions; review and commit.

## Phase 3: sizing, navigation and anchored Alt drag

- Bind plain wheel to brush size, Alt+wheel to band size, Shift+wheel to frequency zoom, Ctrl+wheel to frequency pan, and Ctrl+right drag to MIDI-style vertical frequency pan/horizontal zoom. Retain ordinary right attenuation and off-surface orbit. Alt has wheel precedence over Shift/Ctrl; reject wheel/navigation during captured edits.
- Add a logarithmic frequency viewport captured in immutable display metadata. Propagate it through worker sampling, CPU Hz picking, footprint and shaders; never map hits using newer controls against older displayed geometry.
- Add bounded UI-owned brush size, feeding the same physical time/octave radii to brush application and outline. Alt wheel sizes the broadband time band.
- Capture Alt band's initial time, radius and drag Y; ignore subsequent X. Submit only changed Y targets, coalescing latest targets while worker is busy via immutable captures/receipt handoff. Add an anchored relative envelope operation without changing existing absolute PaintSegment semantics. Multiply the before envelope by a relative factor with smooth falloff and gain bounds; neutral Y restores exact baseline, including prior edits/silence. Preserve positive-Y boost, one undo action per gesture, cancellation, release/outside routing, and stale-source rejection.
- Test radius limits, fixed-anchor/reversible drag, non-unity/silent baseline, seam wrap, stationary idempotence, latest target under worker backlog, navigation routing and worker publication behavior. Review, build native tests/app and commit.

## Review and completion

First send this plan to a dedicated review subagent, incorporating concrete findings before implementation. Spawn a fresh implementation subagent for each phase sequentially to avoid overlapping editor changes. The primary agent reviews each diff and validation before committing. Run the threading audit and manually inspect changed callback/publication paths. Record exact checks and outstanding manual listening/visual/performance concerns below.

## Execution record

- Initial plan created. Review found 16 dB publication jumps and linear-before-log interpolation; phase 1 adopts 60 dB/s and height-domain interpolation. App pointer Y rises upward: preserve existing positive-Y boost direction.
- Baseline Debug native build and all 29 spectral tests pass.
- Full independent review completed: incorporated fixed resolution/FFT choices, navigation precedence and immutable viewport metadata, anchored relative envelope semantics, and worker ownership tests. Each new shared field must document owner/readers/writers/handoff/teardown. Final listening and GPU appearance remain manual acceptance checks.
- Baseline worker profile (Debug, 48kHz): 2s analysis38/reconstruction49ms, 10s172/235ms, 30s511/706ms. The 30s estimate is 508.24MiB; follow-up review adds explicit 4096 fallback rather than raising caps or rejecting previously admitted loops.
- Phase 1 complete: 60 dB/s and per-corner compressed height interpolation, with three new equal-rate/ceiling/unequal-corner regressions. Incremental Debug x64 native tests build passed using local task MSBuild and documented process PATH preflight. AudioSpectralTest.* passed all 32 enabled tests (3540 ms); the two profiling tests remain intentionally disabled. The existing blocked-worker saturation fixture now waits for a rate-derived 12 dB exposure instead of assuming the old 40 ms saturation time. Threading audit passed; changed code runs only in display preparation/configuration, with no shared-state or callback-body changes. Live listening must confirm medium-low calibration; GPU appearance and display preparation cost are pending later phases.
