# Verification matrix

Phase 1 added obligations only; no build, native test, or runtime scenario was executed. Before future builds/tests, read local `.vscode/tasks.json` and `doc/build.md`, use the repository environment-normalizing wrapper, and run affected incremental targets.

| Finding | Build/static evidence | Native test evidence | Runtime/manual evidence | Later owner/sign-off |
| --- | --- | --- | --- | --- |
| F-001 | Regenerate diff/project/resource inventory if HUD moves | Retained HUD/graphics tests | Trigger/station/HUD rendering | Human scope gate; Stages 12/14/16/20/21 |
| F-002 | Regenerate diff/interface inventory if VST moves | VST3 mapping/state plus affected engine tests | VST2/VST3 load, editor, MIDI, state | Human scope gate; Stages 12/14/16/20/21 |
| F-003 | Affected app build if retained | Persistence tests | Multi-monitor window save/restore | Human scope gate; Stage 16 |
| F-004 | Validate task commands and wrapper if retained | Applicable native target invocation | None | Human scope gate; Stage 21 |
| F-005 | Include/dependency audit; JammaLib build | Continuous/Block/NoSync timing tests | Reconnect and `NoSync` invalidation | Stages 7/9/20 |
| F-006 | JammaLib build; one common-map owner audit | Unequal lengths, offsets, rebase/wrap | Join, reconnect, free-run after `NoSync` | Stages 7/8/9/11/20 |
| F-007 | Include-graph and JammaLib build | Coordinator/local timing tests | None | Stages 13/20 |
| F-008 | JammaLib build; include audit | Latest/zero mailbox and local-offset tests | Disconnected and `NoSync` offset control | Stages 7/8/9/20 |
| F-009 | JammaLib build | Coordinator command + integration forwarding tests | Join/leave command trace | Stages 9/10/20 |
| F-010 | JammaLib/tests compile; identifier audit | Affected timing tests | None | Stage 15/20 |
| F-011 | JammaLib/tests compile; retired-name audit | Export/metronome helper tests | None | Stages 14/15/20 |
| F-012 | Incremental JammaLib link/build | Timing/command/mailbox tests | None | Stages 7/8/20 |
| F-013 | Symbol and UI-string audit | Coordinator/quantisation tests | Remote-tempo prompt wording | Stages 9/15/20 |
| F-014 | JammaLib/tests compile; coordinate-name audit | Command and integration simulations | Trace replace + discipline coordinates | Stages 9/11/15/20 |
| F-015 | JammaLib/tests compile | Distinct Timer/device sentinel tests; MIDI timestamp tests | Late-observation scenario | Stages 9/11/15/20 |
| F-016 | JammaLib/tests compile; usage audit | MIDI automation/timing tests | Automation playback across correction | Stages 9/14/15/20 |
| F-017 | JammaLib/tests compile | Export-lane pure-helper tests | Only if compensation path is retained/enabled | Stages 8/13/15/20 |
| F-018 | Incremental JammaLib/test build | Sync-map, offsets, reconnect, `NoSync` tests | Focused remote join/local loop | Stages 9/20 |
| F-019 | Incremental JammaLib/test build | Existing timing/logging-adjacent tests | Normal and verbose logging; remote join | Stages 8/17/20 |
| F-020 | App build and copied-resource audit | Relevant HUD/graphics tests | Trigger default/hover/down/out rendering | Dependent on F-001; Stages 12/16/20/21 |

Cross-phase obligations not represented as Phase 1 deletions:

- Stage 8 must assess callback-side cost of retained before/after alignment diagnostics.
- Stage 17 must decide the supported audience and volume for live/dormant alignment snapshots and coordinator counters.
- Phase 2 must prove that the unified command, Timer update, common map, and per-entity restores remain coherent under all three follow policies.
