# Mute and selection groups implementation plan

Status: ratified specification, checked against the codebase.
Branch: `feature/mute-group`
Worktree: `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-mute-group`

## Feature specification

### Scope and stored state

Both group types apply to local stations, takes, and their loops only. Remote stations and their contents are excluded from capture and recall. Save and load both group types with JAM files.

Mute groups capture the mute configuration at the finest level at which it is defined, including individual loops, plus full VST state on local takes and stations using the same state representation as JAM persistence. Capture both muted and unmuted states: material inaudible because of muting when a group is created or overwritten must remain muted when that group is recalled. Silence caused by absent material or zero gain does not itself define mute.

Keep the hierarchy's mute flags and effective muted result distinct. Record the mute state needed to reproduce the captured configuration, including effective suppression from parent or temporary source muting. Recall must reproduce that suppression using explicit persistent mute state without acquiring or releasing recording-owned source-mute controls. A transient mute that was audible as suppression at capture therefore becomes a stored mute choice; recording lifecycle state itself is never restored.

Selection groups capture the complete local selection configuration, including selected and deselected targets at station, take, and individual audio/MIDI-loop level. Restore the captured selection depth and preserve individual loop choices and aggregate parent flags without inadvertently selecting siblings. Mute recall does not change selection; selection recall does not change mute or VST state.

Groups reference stable target and plugin identities, never mutable GUI positions. Recall skips deleted targets/plugins and leaves new targets outside the snapshot unchanged. Do not keep removed entities alive through group references or apply a deleted plugin's state to a replacement occupying its chain position.

### Slots and interaction

Each row supports slots `1` through `9`, with independent active-group indication. A slot is either empty or holds a complete snapshot; an empty selection is a valid stored snapshot.

- An empty holder initially contains one subdued `+` button.
- `+` captures current state into the lowest available empty slot. It fills interior holes before appending. Disable `+` when all nine slots are filled; retain it in the layout as a subdued disabled action.
- Clicking an occupied numbered slot recalls it. Clicking an empty numbered slot captures current state there.
- Shift-click creates or overwrites a numbered slot. Alt-click clears it. These operations apply to both rows.
- Mute shortcuts `1` through `9` recall an occupied slot or capture into an empty slot. Shift+number creates or overwrites; Alt+number clears. `0` does not address a group. Selection groups have no number-key activation.
- Storing directly into a higher slot creates visible empty slots below it. Clearing an interior slot leaves its number visible and unfilled. Remove all trailing empty slots above the highest occupied slot. Numbers never move between stored snapshots.
- Recall replaces the snapshot's target states; pressing an active button again recalls it again.
- The last stored/recalled slot stays highlighted until another slot is stored/recalled or that slot is cleared. This indicates the last applied group, rather than continuous equality with live state; manual edits and automation do not overwrite the stored snapshot.

Popup, text-entry and engaged editor input retain priority. Outside these contexts, supported group shortcuts and Backspace take priority over configured keyboard triggers and consume both key transitions. Use one command per physical press, suppress repeats, and support Alt system-key delivery. Ignore unsupported modifier combinations; Alt+Shift does not ambiguously clear or overwrite.

### Recall and Backspace

Recall is immediate, with no beat/bar quantisation. Publish mute changes at the next available audio boundary and preserve smooth gain transitions. Reuse existing smoothed mixer paths where verified, and add smoothing where a recalled mute would otherwise cause a hard discontinuity. Loop-boundary crossfades alone do not establish click-free mute switching.

Backspace resolves selected local stations and takes into a deduplicated set of takes and inverts each take's mute independently. Selected stations expand to their takes; a take selected both directly and through its station is toggled once. Individual loop selection contributes its aggregate selected take under the existing selection semantics. With no local selection, Backspace makes no mute change and does not fall back to hover. Preserve recording-owned temporary source-mute controls.

Full VST state capture/restore uses the existing JAM-compatible plugin blobs, including VST2 chunk/parameter fallback and VST3 component/controller state. Automation keeps running and overrides restored values through its normal subsequent playback. Group recall must not disable, clear, replace or record over automation lanes.

“Immediate” means dispatch without musical delay; plugin state restoration still requires a safe non-audio execution path. No plugin state serialization/deserialization may run on the audio callback. Smooth audible mute transitions; do not promise arbitrary third-party plugin preset changes are internally smoothed.

### Appearance and layout

Extend the existing top panel downwards for two group rows beneath its input-source controls. Mute groups are blue; selection groups are bright yellow. Both use rounded holder borders with slight fill and numbered rectangular buttons of matching geometry. The `+` action stays subdued and neutral.

With only slot 1 visible, `1` and `+` each occupy half the usable holder width, with even padding. Divide usable width equally among all visible numbered slots and the trailing `+`. Interior empty slots retain an unfilled numbered presentation. Provide distinct stored, active, empty, hover, pressed and disabled states.

Use 32px button height, 4px holder padding and 8px spacing between rows as layout starting values, consistent with existing style constants and visual verification. Render labels as text separate from the textures. Use dedicated nine-patch textures generated with the tga-icon-gen skill, retaining editable SVG sources and registered slice metadata.

Ease button positions and widths over approximately 120ms when slots are added or removed. Retarget from current presented geometry during rapid changes. Keep hit testing, clipping and pointer capture consistent with animated geometry.

Adjust the settings panel to fit below the taller top panel. Derive the occupied top-panel bounds from the actual source-row and group-row layout, including source scrollbars, rather than duplicating a fixed height. Preserve camera/performance-area layout. Verify small windows, DPI scaling, routing overlays and Ctrl overlay transitions.

## Codebase findings and implementation mapping

| Area | Verified source | Required implementation |
| --- | --- | --- |
| Top panel | `JammaLib/src/gui/GuiHud.cpp:791`, `_LayoutPanels`; `GuiHud.h:82`, `SetSize` | Extend `_topStrip` and border. Final height is recomputed from source-row height at lines 859-862; include both group rows in that calculation and retain source controls at the top. |
| Settings panel | `JammaLib/src/gui/GuiMainPanel.cpp`, `_Layout`, `_UpdatePresentation` | Pass actual occupied top-strip bounds into settings layout so the taller strip constrains settings height/placement, including small viewports. |
| UI animation | `JammaLib/src/engine/Scene.cpp:927`; `GuiMainPanel::AdvanceAnimation` | Reuse the elapsed-time UI pump and eased presentation pattern; animate off the audio callback. |
| Radio controls | `JammaLib/src/gui/GuiRadio.h`, `GuiRadio.cpp` | Existing radio uses fixed toggle children. Add a dedicated group-row widget supporting sparse slots, no active choice, dynamic geometry and a separate action-only `+`. |
| Input routing | `JammaLib/src/engine/Scene.cpp:1355`, `OnAction(KeyAction)` | Preserve contextual consumers, then route to the dedicated group controller before keyboard triggers. Keep Scene wiring minimal. |
| Other keyboard input | `JammaLib/src/midi/MidiRouter.cpp:186` | Channel override uses PageUp/PageDown. Configured trigger overlap with digits/Backspace follows the shortcut precedence specified above. |
| Station mute | `JammaLib/src/engine/Scene.cpp:2882-2911` | Existing station selection mute fans out to takes and status uses `AllTakesMuted()`. Reuse owner operations and deduplicate children for Backspace. |
| Loop and take mute | `JammaLib/src/engine/Loop.h:217-219`, `Loop.cpp:Mute/UnMute`; `base/Tweakable.h`; `engine/LoopTake.h:171` | Preserve loop-level configuration and effective take suppression. Audit mixer transitions rather than assuming loop crossfades smooth mute. |
| Selection | `JammaLib/src/engine/Scene.cpp:2967`, `_UpdateSelection`; `GuiElement.h:179` | Preserve station propagation, individual loop choices and aggregate take flags. Extract reusable capture/restore behavior into focused classes instead of expanding Scene. |
| Identity | `JammaLib/src/engine/LoopTake.h:149`, `Id`; `GuiElement.h:164`, `GlobalId` | Take IDs exist; GUI paths are positional. Add/extend persistent station, loop and plugin identity as needed, with compatible migration for old JAM files. |
| VST ownership | `LoopTake.h:200-217`; `Station.h:230-245`; `vst/VstChain.h` | Validate owning entity and plugin instance before restore. A changed chain position must not change snapshot association. |
| VST state | `JammaLib/src/vst/IVstPlugin.h:125-137`; `LoopTake.cpp:3869`, `Station.cpp:3044`, `VstEntries`; `io/JamFile.h:88-105` | Reuse JAM-compatible full state blobs and encoding. Capture/restore is non-RT; existing load-time restoration does not establish safe live restoration. |
| VST automation | `JammaLib/src/vst/IVstPlugin.h:75-79`; `Vst3Plugin.cpp:1721-1760` | Parameter writes interact with audio-block queues and controller reads. Retain normal automation precedence after safe full-state restore; parameter enumeration is not required just to persist full state. |
| Session storage | `JammaLib/src/io/JamFile.h:172-211`; `io/IoSessionExporter.*` | Add optional group records, stable identity mapping and import/export wiring. Old files without groups load as two empty rows. |

## Implementation design

### Dedicated model and controller

Introduce a focused JammaLib performance-group model/service and command controller. Scene supplies only necessary wiring, current hierarchy access and existing pumps. Keep slot rules, snapshots, identity resolution, selection behavior and persistence outside Scene to avoid further bloat. Take/station/loop/plugin owners retain their own state application behavior, following `doc/glossary.md`.

Use separate mute and selection snapshot types. Model each row as nine optional slots plus a separate active slot. Persist explicit slot numbers and holes; derive visible count from the highest occupied slot. Share store, recall, overwrite, clear and add commands between pointer and keyboard routing. Store and overwrite atomically replace a complete snapshot; capture failure must preserve an existing slot and report the failure rather than silently replacing it with incomplete state.

Capture local hierarchy configuration into immutable values. Preserve the finest mute/selection detail and negative states; record effective suppression without coupling saved groups to recording lifecycle handles. Use stable IDs for persistence and weak runtime references or re-resolution for live application. Refresh selection visuals and mute status using existing update paths after restore.

### Safe recall and plugin lifecycle

Document owner thread, readers, writers, synchronization and teardown for every new shared variable. Follow `doc/realtime-audio.md` and the threading-review skill. Keep slot mutation, state-blob work and geometry on non-audio owners. Prepare and publish immutable mute/application values through the existing compatible snapshot patterns. Callback work stays bounded, allocation-free, lock-free and free of waits, logging and state-blob calls.

Trace existing plugin lifetime, automation and editor execution before adding live full-state restore. Serialize capture/restore with the plugin's UI/controller requirements. Safely coordinate the active processor so non-RT `SetState` never races `ProcessBlock`; use owner-managed staging/replacement with audio-boundary publication where needed. Preserve routing, editor ownership, automation bindings and persistent plugin identity. Do not suspend the callback or hold a mutex across plugin state work.

Version recall requests so superseded preparation cannot publish after a newer recall. Cancel stale operations when a target/plugin is removed or a session closes. Retire replaced processor state off the callback through existing lifetime management. Retain smooth audible mute transitions during application. Full-state recall may require non-RT preparation; publish as soon as ready rather than waiting for a musical boundary. Resume normal automation application over the recalled plugin state without generating automation-record events.

### JAM persistence

Extend optional JAM records for both nine-slot rows, selection depth, target IDs, mute hierarchy/effective suppression and owner/plugin state blobs. Preserve sparse numbering and valid empty-selection groups. Integrate export/import, validation limits and stable identity restoration. Exclude remote targets. Validate slot ranges, duplicates, unresolved identities and malformed blobs using established loader behavior.

Old JAM files remain loadable with empty group rows. Older entity records receive stable IDs during import as necessary; export writes those IDs for subsequent round trips. Preserve plugin association across load and chain reorder; deleted or unmatched entities must never be matched merely by current array position.

### Widgets and assets

Compose each group row from a holder, numbered toggles and a separate neutral add action. Connect modifier-click operations and tooltips to the same command controller as keyboard input. Keep active indication independent for each row; clearing the active slot clears its indication.

Generate rounded nine-patch holder/button art and aligned interactive states using `.agents/skills/tga-icon-gen/SKILL.md`. Keep staged intermediates separate from committed assets. Verify straight-alpha edges, exact slice coordinates and texture dimensions; register assets in `Jamma/resources/ResourceList.txt`. Reuse one geometry with renderer tint where appropriate.

Expose occupied top-strip bounds to settings layout. Update both layout and animated presentation calculations to honor those bounds. Reuse the Scene UI animation pump for group easing; do not add callback-owned GUI work.

## Implementation sequence

1. Implement/test the nine-slot model and commands: lowest-empty `+`, sparse direct creation, overwrite, interior clear, trailing removal, independent active indication and stable numbering.
2. Add persistent entity/plugin identity and owner-level capture/restore for complete local mute and selection configuration at the finest defined level. Preserve effective suppression, negative states, selection depth and stale/new-target behavior.
3. Add the dedicated input/controller integration with minimal Scene wiring. Implement modifier clicks, digit shortcuts, context priority, repeat suppression and deduplicated independent Backspace inversion.
4. Implement JAM-compatible full VST capture and safe live restore through plugin-owner lifecycle coordination. Preserve editor/routing identity, smooth mute transitions, superseding-request order and ongoing automation precedence.
5. Add JAM group records and identity import/export with old-file compatibility, sparse slot validation and state-blob round-trip tests.
6. Implement both group-row widgets and eased geometry. Extend the top strip and constrain settings layout using actual occupied bounds.
7. Generate, validate and register nine-patch textures with the tga-icon-gen skill; visually verify both tints, neutral `+`, all button states and animated hit testing.
8. Read the copied local `.vscode/tasks.json` before each build/test run and follow `doc/build.md`. Incrementally build affected JammaLib/app targets and run relevant native tests. Run the threading-review audit and manually inspect affected callback paths. Do not guess MSBuild paths or use Clean/Rebuild without necessity.

## Acceptance and validation

- First capture shows `1` and `+` at equal widths; filled rows stop at nine groups and disable `+`.
- `+` fills the lowest hole; direct higher-slot creation preserves lower empty numbers; clearing trailing slots resizes survivors without renumbering.
- Both rows support plain, Shift and Alt clicks; only mute groups respond to `1`-`9`. Unsupported modifiers/`0` do not address groups. Manual edits leave last-applied indication intact.
- Mute recall restores muted and unmuted configuration at individual loop, take and station scope, including effective captured suppression. Local-only capture/recall does not affect remote entities or recording-owned controls.
- Selection recall restores selected and deselected configuration and depth, preserving loop-level detail and parent aggregates. Empty selection groups remain valid stored slots.
- Removed/reordered entities/plugins never redirect recall; new targets remain untouched; pending operations cancel on unload and teardown.
- Backspace inverts each selected take once, including station expansion and overlapping selections. Mixed states invert independently. No selection causes no mute action; text/editor input and key repeats remain correct.
- Recall has no musical delay and audible mute transitions use verified smoothing. Check existing mixer behavior and add a meaningful discontinuity/ramp test where smoothing changes are needed.
- VST2/VST3 full state round-trips match JAM state semantics, including chunk/component/controller data; live restore has no callback state-blob calls or processor races. Editor values reflect restoration, automation continues overriding and recall does not record automation.
- Rapid recalls apply the newest request; failed capture preserves the previous slot; failed plugin restore follows established error reporting without corrupting group state.
- JAM round trips preserve both rows, sparse numbering, selection depth, identities and plugin blobs. Old files produce empty groups. Malformed records and unmatched targets are handled safely.
- Verify add/remove/resize animations, hit testing during motion, narrow/short windows, DPI scaling, nine-patch corners/alpha and related HUD/settings overlays.

Existing test homes: `test/JammaLib_Tests/src/gui/GuiLayout_Tests.cpp`, `GuiControls_Tests.cpp`, `GuiComponents_Tests.cpp`; `src/engine/SceneRoutingIntegration_Tests.cpp`, `VstStaging_Tests.cpp`; `src/io/JamFile_Tests.cpp`, `IoSessionExporter_Tests.cpp`; `src/vst/`. Add focused group-model, persistence and lifecycle tests rather than tests that mirror rendering implementation.

This plan incorporates the user's decisions. Implementation and asset generation are subsequent work; no implementation or asset changes have been made as part of this plan update.

## Implementation review amendments (2026-10-12)

- Assets: use one state-independent stretchable holder background, with row labels rendered separately as text (Mute / Selection). Generate a neutral add-button base, hover (`_over`) and pressed (`_down`) family. Generate one white/grayscale tintable numbered-button family with six aligned states: unfilled base/hover/down and bright filled base/hover/down. Active indication is a separate visual cue from occupancy, so an occupied inactive slot remains visibly filled. Disabled add appearance may use renderer opacity; it does not require another texture. Keep glyphs outside stretchable textures and render the plus separately if the button stretches.
- Existing audio loops already have IDs; reuse these rather than introducing competing identity. Extend stable identity to stations, takes in the JAM manifest, MIDI streams and plugin instances. Reject duplicate identity ambiguity on import; generate IDs only for absent legacy IDs. Group references never keep deleted targets alive.
- Selection capture must include MIDI streams as well as audio loops. Apply leaf choices and aggregate flags without parent propagation overwriting the captured leaves. New targets remain untouched even beneath captured parents.
- Both GetState and SetState require processor exclusion: non-audio execution alone is insufficient. Audit capture as well as restore, including editor/controller calls and automation delivery. Implement and test a bounded audio acknowledgement/owner handoff or compatible staged processor lifecycle before enabling live blob access. Callback participation must never wait, allocate, lock, destroy processors, or serialize blobs. Failed or superseded work must not partially overwrite a stored slot.
- Bound imported group target counts and total decoded blob bytes before allocating. Validate explicit slots, duplicate slots/IDs, target kinds and blobs; keep legacy fields distinct from the new snapshot records. Define round-trip tests for empty occupied groups and sparse rows.
- Execution: implement in dependency order, with an independent subagent review for each phase before committing its focused diff. Assets may be prepared independently once their contract is fixed. Re-read local tasks before each build/test. Record checks and any unresolved limitations honestly; completion requires engine, plugins, persistence, input, widgets and assets, not only the slot model.
