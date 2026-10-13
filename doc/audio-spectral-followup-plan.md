# Spectral editing follow-up

## Reviewed decisions

The 1024-column mesh exceeds the independent analysis frames of many short loops. Its current 8192-sample window/1024 hop spans 171 ms with 21 ms frame spacing at 48 kHz. Tiny brushes also miss coefficient centres, and the shader hides the bottom 10.8 dB in a dark fade. Raising the global brush rate would mask these problems.

The user chose finer time steps plus a shorter FFT window, accepting less frequency detail. Start new analyses at 4096 samples with 256 hop (85 ms window, 5.3 ms steps at 48 kHz). Keep named window/overlap constants easy to tune; preserve stored transforms. Source/session/retained-layout admission can fall back to 4096/512 then 4096/1024 without raising memory caps. Feedback and measurements must distinguish fallback time detail from requested detail.

Increase the fixed total time geometry about fourfold to 4095 segments, giving a 4096-wide texture; frequency stays 256. This avoids the 4097-wide texture that exactly 4096 segments would require on GPUs limited to 4096. Audit the 64 MiB host display allowance for the approximately 32 MiB grid/indices plus retained 4 MiB heightfields; report GPU buffers separately. Longer loops still spread this fixed count over more time.

Ctrl currently controls frequency only; no time-zoom viewport was added. Remove spectral Ctrl navigation and reserve Ctrl input. Use Shift+right drag for MIDI-style vertical frequency pan/horizontal frequency zoom, keeping Shift+wheel zoom, plain wheel brush size, Alt+wheel band size, ordinary right attenuation and off-surface orbit.

## Phase 1: time detail and small-brush visibility

- Implement the new fixed geometry and tunable transform defaults/admission ladder. Retain immutable selected-transform capture and stored-transform reopen/undo behavior.
- Resolve sub-lattice brush coverage with a full-strength central region and smooth shoulders, using effective support based on hop/bin spacing and chosen physical radius. Expand visited bounds consistently, show the effective footprint, and preserve elapsed-time integration/event subdivision. Do not independently round points on each input event or scale strength by number of affected cells.
- Retain 60 dB/s. Shorten the shader dark fade from 0.12 to about 0.03, preserving authored silence and true-zero black; investigate whether floor changes are necessary rather than changing authoring bounds blindly.
- Test minimum-radius midpoint/bin-gap brushes, centre rate across sizes/levels, subdivision, seam/context, bounds/silence, new/legacy transforms, preferred and fallback resource selection, geometry/picking, and small edits on the dense mesh.
- Build affected native targets, run relevant tests, and profile Release analysis/reconstruction/display/input maps for representative 2/10/30-second loops. Separate mesh-only cost from denser analysis where feasible. Report CPU timings and the unmeasured GPU draw/upload cost. Primary reviews and commits.

## Phase 2: relative Alt sweeping and navigation cleanup

- Extend the envelope with relative swept segments evaluated against E0. Full-strength centre samples receive the drag-derived relative factor; smooth shoulders multiply E0, preserving silence and gain bounds. Strongest influence wins, with later input winning equal influence; retain local competing-winner ramps.
- Pure vertical drag must match the current relative band exactly. Freeze click projection and initial screen Y for horizontal mapping, calculating displacement from the initial projected point and adding the actual picked time. Current mouse Y still controls level. The footprint follows the moving centre; stationary holds do not compound.
- Boost vertically then move horizontally: the swept interval receives the same factor. Diagonal sweeps interpolate logical Y displacement, then convert to the factor, avoiding event-subdivision dependence from nonlinear factor conversion. Returning to neutral restores the current band; distant earlier swept portions retain their authored changes.
- Preserve full path geometry under worker backlog using bounded preallocated winner maps and terminal receipt handoff, rather than latest endpoint or a lossy fixed segment queue. Account for active/frozen/cancelled maps and worker scratch, including transitions between ordinary and broadband strokes.
- Move navigation to Shift+right and remove Ctrl wheel/drag branches/help. Reserve Ctrl without changing brush content or viewport. Keep pointer capture, camera/size/source cancellation and displayed-viewport coherence.
- Test vertical equivalence, plateau sweep, diagonal subdivision, reversals/winners, non-unity/silence/seam, backlog equivalence, undo/cancel/release, physical projection/DPI, and modifier routing. Build native tests/library/app, verify deployed shader resources. Primary reviews and commits.

## Ownership and validation

Two read-only subagents reviewed detail/brush behavior and Alt/navigation separately before implementation. Fresh implementation agents own the phases sequentially to prevent shared-file collisions. Workers receive immutable captures or frozen map handoffs; UI owns mutable input and navigation. Existing audio snapshot publication remains unchanged. Run threading-review audit and manually inspect changes: no callback allocation, lock or wait is permitted.

## Execution record

- Initial read-only reviews complete; user selected a shorter, tunable FFT with finer steps.
- Phase 1 implemented: 4095 fixed time segments, 4096-point preferred FFT/16-fold overlap, doubling hops to the fourfold-overlap baseline. At 48 kHz the window is 85.33 ms, frequency bins 11.72 Hz, and preferred steps 5.33 ms. Existing authored transforms (including 8192/1024) reopen unchanged.
- Higher-detail admission leaves DisplayWorkingBytes + MaxHistoryBytes + 32 MiB short-source headroom (160 MiB total). The baseline may still use the whole 512 MiB cap to preserve long-loop admission. A concurrent 10-second edited loop and short loop now admit together; the 10-second analysis falls back to 512 hop.
- Minimum-radius brushes use a full-strength inner 75% plateau with analytic shoulders, at least one hop of time support and each coefficient's neighbouring log-bin spacing. Minimum brushes at simultaneous half-hop/bin-gap positions receive the full rate, including legacy 1024 hops. Authoring remains 60 dB/s and -90 dB floor; only the shader's lowest colour fade changes from 10.8 dB to 2.7 dB.
- Display scratch audit: host grid approximately 8.03 MiB plus 23.99 MiB indices and up to five distinct 4.016 MiB heights (52.1 MiB total) fit the existing 64 MiB allowance. GPU buffers/texture separately occupy approximately 36 MiB. GPU draw/upload/DPI acceptance remains manual.
- Debug native build succeeded; all 47 spectral tests passed in 16.9 seconds, including dense geometry/picking/seam, coefficient-gap centre rate, integrated event subdivision, authored silence/bounds, cached changing-envelope corner equivalence, legacy transforms and admission/reopen. Threading audit passed; no callback bodies changed and no shared mutable state was introduced. Release native build and all 49 spectral tests (including both disabled profiles) passed in 9.87 seconds.

### Phase 1 Release measurements

Single-run diagnostics at 48 kHz, measured off audio; timings are milliseconds. Same-analysis mesh comparison isolates display column cost, with a changing broadband envelope.

| Loop | Hop | Admission MiB | Analyze | Reconstruct | Display 1024 | Display 4095 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 s | 256 | 205.64 | 16.22 | 24.42 | 16.17 | 37.05 |
| 10 s | 512 | 335.20 | 37.49 | 56.65 | 27.69 | 48.94 |
| 30 s | 1024 | 508.24 | 58.47 | 90.85 | 40.83 | 61.64 |

Fourfold time geometry adds approximately 21 ms worker display cost in this comparison. The denser 2-second analysis/reconstruction costs about twice the prior 8192/1024 measurement (9.11/13.01 ms); 10-second fallback roughly matches prior 38.45/56.37 ms, and 30-second fallback matches prior 63.06/90.75 ms. These diagnostics confirm that visual updates can run below 30 Hz and audio changes wait for worker reconstruction/coalescing. Callback processing and playback latency remain unchanged; GPU drawing/upload timings are unmeasured.

High-frequency input-map rasterization, 1000 moving pointer samples near 18 kHz:

| Loop | Default mean / p95 / max ms | Minimum 5 ms, 1/16 octave mean / p95 / max ms |
| --- | --- | --- |
| 2 s | 2.677 / 3.087 / 7.028 | 0.257 / 0.365 / 0.426 |
| 10 s | 1.323 / 1.457 / 2.393 | 0.254 / 0.340 / 0.503 |
| 30 s | 0.656 / 0.738 / 1.009 | 0.251 / 0.292 / 0.591 |

Minimum-brush input remains below 0.6 ms in these runs despite conservative candidate bin bounds; tighter bounds are deferred. Final threading audit and whitespace diff check passed. Combined app deployment/build validation is recorded in phase 2 below.

### Phase 2 implementation and verification

Relative Alt sweeping is implemented with two preallocated winner maps and immutable receipt handoff. Sweeps interpolate double logical Y before converting to relative gain; continuous collinear run provenance survives input subdivision and frozen-map batches. FIFO application gives later equal influence priority. Vertical-only fractional clicks retain the existing rounded sample through one captured alignment offset. Relative local ramps preserve prior silence, and metadata-only winner changes trigger snapshot evaluation even when raw gains remain identical. Shift+right replaces Ctrl navigation; Ctrl wheel/click has priority and is reserved.

Shared input scratch admission is max(36*C,72*N). Active/frozen/cancelled maps and both mixed cancellation directions remain bounded; standard transforms retain their phase 1 admission estimates. The DSP contract records per-field UI/worker ownership, release/acquire handoff, cancellation and teardown. Callback-owned bodies are unchanged; no callback allocations, waits or locks were added.

Debug native library/tests and app builds pass. All 56 enabled spectral tests and 40 MIDI grid/pitch controls tests pass. The extra disabled Alt input diagnostic also passes when explicitly selected. Coverage includes plateau sweeps, nonlinear diagonal subdivision (including .25-to-.75 sample feather boundaries and split handoffs), vertical equivalence, non-unity/silence, reversals, metadata-only ramp changes, worker backlog and one undo, both cancelled mixed stroke transitions, invalid points, frozen projection and DPI equivalence. Threading audit and whitespace checks pass. Deployed Debug fragment and vertex shader SHA256 values match source (3F41B01A468D0926F4AD23994B331415C17E424BB2AF69EF190A72EE08907FC1 and BA63CD078F2A094D752E76F9E38832D872A238A21A704534B3CD2F6ABC05E509 respectively).

Debug Alt map diagnostics (unoptimized build; not comparable to the phase 1 Release timings):

| Loop | Full-loop pointer jump | 1/100-loop pointer-step mean |
| --- | ---: | ---: |
| 2 s | 10.17 ms | 0.43 ms |
| 10 s | 50.12 ms | 0.87 ms |
| 30 s | 156.53 ms | 1.91 ms |

Large single pointer jumps on long loops can stall Debug UI rasterization. Release Alt input performance remains unmeasured; routine pointer-step means in this diagnostic stay below 2 ms. A body-contained centre fast path avoids periodic-copy arithmetic; wrapped/fractional-edge and multiple-visit cases retain the conservative general path. Whole-loop forward/reverse diagonal subdivision tests protect the later seam-visit rule. These timings were captured without concurrent builds/tests after the fast path; they are CPU diagnostics rather than UI/GPU latency guarantees. Live listening, GPU upload/draw and physical input/visual/DPI acceptance remain outstanding.
