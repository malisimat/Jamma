# Seven-feature integration handoff — 14 October 2026

Prepared branch: `codex/integrate-ready-features` in `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-integrate-ready-features`.

**Tested code SHA: `25ffa1fb70d8fb9d3f050f0c65bb07f20ad691c5`**, tree `f1883a60a3af7e30605b444e52115a3157e619e1`. The later handoff-only commit changes no code. Local and HTTPS remote master remain `cff00825781eadb18fefa9504e0893f5d3b8b9ea`. No push, source archive or master promotion occurred. All source worktrees remain available. All four rig-model-cleanup-only commits are excluded.

## Sources, tree shaking and serial checkpoints

Heads were checked initially and again after validation. HTTPS remote heads still match the plan; MIDI's local cleanup is deliberately unpushed. Each source was reviewed using the linked session C++ tree-shaking skill. Uncertain public/dynamic/persistence entry points were retained.

| Feature | Final source SHA | Tested merge SHA | Debug checkpoint |
| --- | --- | --- | --- |
| Scene | `12d59aef62865f946911df37a5abdb87d18659b0` | `0ec7f86c3282cf2b6f0b6a674a097ebf36a47eb4` | 250 focused passed |
| MIDI | `b5221d8c53f399788ac971ac326cc2db1302cf6b` | `b3a3dbae2984bb8cad40e229f1f0f6be1d516db6` | 1499 full passed |
| Device | `2a1022e46748809aa663fcac5e2e8d76d4d5db0c` | `0dc26853577b342ca98521af56c624fb4d17a74a` | 170 focused + 15 routing passed |
| Station | `cc35524e297d8390bac8ef38d3524aa665060721` | `6f5c62bd3c1a9e1c27f6a40a8b4fca13cb638998` | 1550 full passed |
| Mute | `a75587b2ea44d166973a4d22ac3ad86aa98f6ea1` | `8eedc459c452197f0f25f52f1bcdd2a58d934710` | 1646 full + 119 focused passed |
| Camera | `48d16fcced3d38e35e5c692dfd55def63d6f8166` | `ca0bb42d3ef9995e965e5067523adb37930b258a` | 261 focused passed |
| Spectral | `75637849f1aee7eaf538800265dd28105658ddc5` | `25ffa1fb70d8fb9d3f050f0c65bb07f20ad691c5` | Final results below |

MIDI cleanup removed only obsolete nonvirtual Station/LoopTake readiness wrappers (20 lines/four files), superseded by topology claims. No callers, exports, registrations, layout or persistence dependency remained. Before evidence reused exact production-equivalent `9b9b88bd` validation (1482 full); after cleanup Debug engine/tests/app built and 171 focused passed. No other source removals were justified: extracted owners/compatibility API; device scheduling/load seams; station inventory/retirement; group identities/guards/assets; camera observation/framing; spectral owners/FFT overlay/licenses/shader registrations all remain live. Temporary or uncertain API candidates were retained.

## Resolutions preserving the seven contracts

- Scene/NINJAM: extracted settings, hierarchy, loader, prompt and diagnostics owners retained; exact shared Timer and leased membership advance once; immutable HUD publication and timing tokens/remote MIDI grid inheritance retained.
- MIDI: preserved first-seed/phase/versioned prepare–claim–commit and cancellation; original-instrument ledger/retries, signed capture age and plugin-path compensation retained. Closed-editor plugin notifications still pump beside spectral publication.
- Device: prepared exact startup, callback quiescence/drain/quarantine and controlled owned-copy Save/Load retained. Hardware preferences update only saved hardware fields through an immutable default descriptor; they cannot autosave live topology/routing.
- Station: shared IDs8400–8402 isolate real GUI dispatch from device7200-series; revision-bearing inventory, master detach/promotion cancellation, remote grid/camera setup for runtime additions and off-callback editor/resource retirement retained. Explicit default save remains separate.
- Mute: truthful guarded bool admission/append/delivery,64 normal release slots and16 independent panic slots coexist. Group publications carry topology revision, reject obsolete membership without success receipt, skip cancelled owners, and retire off callback. Sparse stable identities, gestures and controlled session persistence retained; recalls do no plugin reload/state capture.
- Camera: publisher wiring covers current/back takes; removed/cancelled targets excluded; framing follows animated presentation. Deferred native input loss survives plugin reentry; camera/station motion dirties picking once. Modal/control/editor arbitration and original idle/record/stop/ditch policy retained.
- Spectral: final namespace/FFT/resource setup preserved; bounded callback capture, immutable worker/publication leases, edited overview/read/bounce and source retirement retained. Both editor types suppress station/HUD controls. Validation immediately rejects removing stations/cancelled takes. CPU topology/static GPU/model ownership, force-reset and current-context teardown preserved. **Save/Load exports original PCM; spectral persistence remains intentionally deferred.**

## Final validation and review

Incremental x64 Debug and Release engine, native-test and app builds passed. Exact tested tree: Debug **1721/1721** (61.728s), Release **1715/1715** (14.245s), zero failures/skips. Each configuration passed all four WGL tests. Six callback/meter/quiescence probe tests are intentionally `_DEBUG`-only. Baseline master passed1401 full tests.

One integration writer; independent read-only thread/lifetime and behavior/persistence reviewers rechecked fixes at every checkpoint. A fresh third reviewer passed the complete cumulative result, including clean auto-merges, GL/picking and final editor guards. Hot-path audits ran on uncommitted deltas; reported locks were UI/job ownership, never callback additions. Vendor internals are not certified lock-free. Whitespace checks pass except three required single-space context lines in upstream KissFFT patches; patch syntax was preserved and those files separately checked with blank-at-eol disabled.

Investigated first failures: reconciled private settings IDs/shared editor handle types/test delimiters; repaired shutdown latch, valid saved-history fixtures, old tab assumptions and simulated-vs-real job consumer races. Final full runs pass after the fixes. New test declaration audit against all seven source trees confirms **one new integration test** (actual device/station GUI dispatch), below the per-worktree limit2; existing release/panic/editor/membership tests were extended or reused.

Commands and all logs/XML are preserved in ignored `bin/integration-validation/`; `run-checkpoint.ps1` rereads local `.vscode/tasks.json` before every build/test, uses its installed MSBuild, `/t:Build`, absolute trailing-backslash SolutionDir and isolated outputs/vcpkg. MIDI cleanup logs remain in that source's same evidence subdirectory.

## Manual acceptance — NOT RUN / awaiting user results

Requested short checks on `Jamma\bin\x64\Release\Jamma.exe` from this checkout: audible NINJAM alignment/held-note ditch; Focusrite meters/switch/rollback and Save/Load; active station removal/Cancel with actual VST editor; group keys/previews/topology persistence; idle camera recording/stop/ditch; spectral read/bounce/punch/reopen/Undo/DPI/context behavior. Automated success does not substitute for these listening/hardware/input results. No manual PASS is claimed.

Release executable SHA256: `530D043B60B3C2C23AC7A382D5BDB0F00EA8F34422D0B57A2DC897A5508F34F9`.

After final acceptance, recheck master freshness, promote with ff-only and build in the main checkout; then consider source-worktree cleanup separately. Neither action was performed here.
