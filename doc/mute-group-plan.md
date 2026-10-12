# Mute and selection groups implementation plan

Status: implemented and validated by native tests and Debug x64 builds; live device/plugin visual checks remain manual.
Current recall policy: mute-group switching applies mute flags only. The live-performance follow-up below supersedes the original full VST recall requirements in this plan.
Branch: `feature/mute-group`
Worktree: `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-mute-group`

## Feature specification

### Scope and stored state

Both group types apply to local stations, takes, and their loops only. Remote stations and their contents are excluded from capture and recall. Save and load both group types with JAM files.

Mute groups capture the mute configuration at the finest level at which it is defined, including individual loops, plus full VST state on local takes and stations using the same state representation as JAM persistence. Capture both muted and unmuted states: material inaudible because of muting when a group is created or overwritten must remain muted when that group is recalled. Silence caused by absent material or zero gain does not itself define mute.

Keep the hierarchy's mute flags and effective muted result distinct. Record the mute state needed to reproduce the captured configuration, including effective suppression from parent or temporary source muting. Recall must reproduce that suppression using explicit persistent mute state without acquiring or releasing recording-owned source-mute controls. A transient mute that was audible as suppression at capture therefore becomes a stored mute choice; recording lifecycle state itself is never restored.

Selection groups capture the complete local selection configuration, including selected and deselected targets at station, take, and individual audio/MIDI-loop level. Restore the captured selection depth and preserve individual loop choices and aggregate parent flags without inadvertently selecting siblings. Mute recall does not change selection; selection recall does not change mute or VST state.

Groups reference stable target and plugin identities, never mutable GUI positions. Recall skips deleted targets/plugins. Mute recall defaults new local takes and audio loops outside the snapshot to muted, reproducing the material heard when the group was stored; selection recall leaves new targets unchanged. Do not keep removed entities alive through group references or apply a deleted plugin's state to a replacement occupying its chain position.

### Slots and interaction

Each row supports slots `1` through `9`, with independent active-group indication. A slot is either empty or holds a complete snapshot; an empty selection is a valid stored snapshot.

- An empty holder initially contains one subdued `+` button.
- `+` captures current state into the lowest available empty slot. It fills interior holes before appending. Disable `+` when all nine slots are filled; retain it in the layout as a subdued disabled action.
- Clicking an occupied numbered slot recalls it. Clicking an empty numbered slot captures current state there.
- Shift-click creates or overwrites a numbered slot. Alt-click clears it. These operations apply to both rows.
- Mute shortcuts `F1` through `F9` recall an occupied slot or capture into an addressable empty slot. Shift+F-key creates or overwrites; Alt+F-key clears an existing slot. Selection groups remain mouse-operated. Digits `0` through `9` continue through normal input routing to configured triggers.
- Keyboard creation is limited to the row's visible slots plus its next add position. On an empty row, only `F1` may store; `F2` through `F9`, including Shift combinations, do nothing. After slot 1 exists, `F2` may store slot 2. A rejected hidden-slot press queues no capture, changes no mute/plugin state or active indication, and does not cancel another pending operation. Existing sparse slots and visible interior holes remain individually addressable; earlier slots are never auto-filled.
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

The existing collapsible top selection panel contains the select-depth radios, then the mute-group row, then the selection-group row. Both complete group rows share the radios' actual laid-out left edge and width, including narrow windows. Mute groups are blue; selection groups are bright yellow. Both use rounded holder borders with slight fill and rectangular buttons of matching geometry. Mute buttons show `F1` through `F9`; selection buttons retain numeric slot labels. The `+` action stays subdued and neutral.

With only slot 1 visible, its button and `+` each occupy half the usable holder width, with even padding. Divide usable width equally among all visible numbered slots and the trailing `+`. Interior empty slots retain an unfilled numbered presentation. Provide distinct stored, active, empty, hover, pressed and disabled states.

Use 32px button height, 4px holder padding and 8px spacing between rows as layout starting values, consistent with existing style constants and visual verification. Render labels as text separate from the textures. Use dedicated nine-patch textures generated with the tga-icon-gen skill, retaining editable SVG sources and registered slice metadata.

Ease button positions and widths over approximately 120ms when slots are added or removed. Retarget from current presented geometry during rapid changes. Keep hit testing, clipping and pointer capture consistent with animated geometry.

Collapse/expand, clipping, animation and pointer cleanup apply to all three selection-panel rows together. Collapsed group controls cannot draw, retain hover/capture or receive pointer actions; function-key commands still work. Derive selection-panel height and settings avoidance from the combined layout. HUD occupied bounds describe its source controls and scrollbars. Preserve camera/performance-area layout. Verify small windows, DPI scaling, routing overlays and Ctrl overlay transitions.

Hovering an occupied group previews its members through rendering only: members are highlighted and other local items are greyed out. Mute-group members are targets captured as audibly unmuted (including effective parent suppression); selection-group members are captured selected targets. Use a blue cue for mute groups and a yellow cue for selection groups. Hover does not recall the group or alter live mute/selection state; leaving the button or hiding the panel clears the preview.

## Codebase findings and implementation mapping

| Area | Verified source | Required implementation |
| --- | --- | --- |
| Top selection panel | `JammaLib/src/gui/GuiMainPanel.cpp`, `_Layout`, `_UpdatePresentation`; `JammaLib/src/engine/Scene.cpp` | Own both group rows in the SelectionOnly panel below its select-depth radios; share its collapse lifecycle, clipping and animation. |
| Settings panel | `JammaLib/src/gui/GuiMainPanel.cpp`, `_Layout`, `_UpdatePresentation` | Account for selection-panel occupied bounds in settings avoidance; retain actual source-control bounds in the HUD, including small viewports. |
| UI animation | `JammaLib/src/engine/Scene.cpp:927`; `GuiMainPanel::AdvanceAnimation` | Reuse the elapsed-time UI pump and eased presentation pattern; animate off the audio callback. |
| Radio controls | `JammaLib/src/gui/GuiRadio.h`, `GuiRadio.cpp` | Existing radio uses fixed toggle children. Add a dedicated group-row widget supporting sparse slots, no active choice, dynamic geometry and a separate action-only `+`. |
| Input routing | `JammaLib/src/engine/Scene.cpp:1355`, `OnAction(KeyAction)` | Preserve contextual consumers, then route to the dedicated group controller before keyboard triggers. Keep Scene wiring minimal. |
| Other keyboard input | `JammaLib/src/midi/MidiRouter.cpp:186` | Channel override uses PageUp/PageDown. Digits remain available to configured triggers; supported F-key group commands and Backspace follow the shortcut precedence above. |
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

Old JAM files remain loadable with empty group rows. Legacy take records retain the identity previously stored in Name; other missing entity IDs are assigned during import as necessary; export writes those IDs for subsequent round trips. Preserve plugin association across load and chain reorder; deleted or unmatched entities must never be matched merely by current array position.

### Widgets and assets

Compose each group row from a holder, numbered toggles and a separate neutral add action. Connect modifier-click operations and tooltips to the same command controller as keyboard input. Keep active indication independent for each row; clearing the active slot clears its indication.

Generate rounded nine-patch holder/button art and aligned interactive states using `.agents/skills/tga-icon-gen/SKILL.md`. Keep staged intermediates separate from committed assets. Verify straight-alpha edges, exact slice coordinates and texture dimensions; register assets in `Jamma/resources/ResourceList.txt`. Reuse one geometry with renderer tint where appropriate.

Expose occupied selection-panel and source-strip bounds to settings layout. Update both layout and animated presentation calculations to honor those bounds. Reuse the Scene UI animation pump for group easing; do not add callback-owned GUI work.

## Implementation sequence

1. Implement/test the nine-slot model and commands: lowest-empty `+`, sparse direct creation, overwrite, interior clear, trailing removal, independent active indication and stable numbering.
2. Add persistent entity/plugin identity and owner-level capture/restore for complete local mute and selection configuration at the finest defined level. Preserve effective suppression, negative states, selection depth and stale/new-target behavior.
3. Add the dedicated input/controller integration with minimal Scene wiring. Implement modifier clicks, function-key shortcuts, bounded keyboard creation, context priority, repeat suppression and deduplicated independent Backspace inversion.
4. Implement JAM-compatible full VST capture and safe live restore through plugin-owner lifecycle coordination. Preserve editor/routing identity, smooth mute transitions, superseding-request order and ongoing automation precedence.
5. Add JAM group records and identity import/export with old-file compatibility, sparse slot validation and state-blob round-trip tests.
6. Implement both group-row widgets and eased geometry. Place both rows below the selection panel's depth radios and constrain settings layout using actual occupied bounds.
7. Generate, validate and register nine-patch textures with the tga-icon-gen skill; visually verify both tints, neutral `+`, all button states and animated hit testing.
8. Read the copied local `.vscode/tasks.json` before each build/test run and follow `doc/build.md`. Incrementally build affected JammaLib/app targets and run relevant native tests. Run the threading-review audit and manually inspect affected callback paths. Do not guess MSBuild paths or use Clean/Rebuild without necessity.

## Acceptance and validation

- First capture shows `F1` (mute) or `1` (selection) and `+` at equal widths; filled rows stop at nine groups and disable `+`.
- `+` fills the lowest hole; direct higher-slot creation preserves lower empty numbers; clearing trailing slots resizes survivors without renumbering.
- Both rows support plain, Shift and Alt clicks; only mute groups respond to `F1`-`F9`. Unsupported modifiers and `F10`-`F12` do not address groups. Digits `0`-`9` remain available downstream. Manual edits leave last-applied indication intact.
- Empty-row `F2` and Shift+`F2` do nothing, including after the UI pump. `F1` stores first, then `F2` may store; keys beyond the visible slots plus one do nothing. Sparse `F9` recall and visible interior-hole capture remain valid without filling preceding slots.
- Expanded selection-panel rows follow radios / mute / selection order with shared width and alignment. Collapse hides all rows and clears pointer state; keyboard commands remain active. Hover previews audible mute members or selected selection members without changing live state.
- Mute recall restores muted and unmuted configuration at individual loop, take and station scope, including effective captured suppression. Local-only capture/recall does not affect remote entities or recording-owned controls.
- Selection recall restores selected and deselected configuration and depth, preserving loop-level detail and parent aggregates. Empty selection groups remain valid stored slots.
- Removed/reordered entities/plugins never redirect recall; new local mute targets default to muted, while new selection targets remain untouched; pending operations cancel on unload and teardown.
- Backspace inverts each selected take once, including station expansion and overlapping selections. Mixed states invert independently. No selection causes no mute action; text/editor input and key repeats remain correct.
- Recall has no musical delay and audible mute transitions use verified smoothing. Check existing mixer behavior and add a meaningful discontinuity/ramp test where smoothing changes are needed.
- VST2/VST3 full state round-trips match JAM state semantics, including chunk/component/controller data; live restore has no callback state-blob calls or processor races. Editor values reflect restoration, automation continues overriding and recall does not record automation.
- Rapid recalls apply the newest request; failed capture preserves the previous slot; failed plugin restore follows established error reporting without corrupting group state.
- JAM round trips preserve both rows, sparse numbering, selection depth, identities and plugin blobs. Old files produce empty groups. Malformed records and unmatched targets are handled safely.
- Verify add/remove/resize animations, hit testing during motion, narrow/short windows, DPI scaling, nine-patch corners/alpha and related HUD/settings overlays.

Existing test homes: `test/JammaLib_Tests/src/gui/GuiLayout_Tests.cpp`, `GuiControls_Tests.cpp`, `GuiComponents_Tests.cpp`; `src/engine/SceneRoutingIntegration_Tests.cpp`, `VstStaging_Tests.cpp`; `src/io/JamFile_Tests.cpp`, `IoSessionExporter_Tests.cpp`; `src/vst/`. Add focused group-model, persistence and lifecycle tests rather than tests that mirror rendering implementation.

The implementation and validation record below supersedes the initial planning-only status.

## Implementation review amendments (2026-10-12)

- Assets: use one state-independent stretchable holder background, with row labels rendered separately as text (Mute / Selection). Generate a neutral add-button base, hover (`_over`) and pressed (`_down`) family. Generate one white/grayscale tintable numbered-button family with six aligned states: unfilled base/hover/down and bright filled base/hover/down. Active indication is a separate visual cue from occupancy, so an occupied inactive slot remains visibly filled. Disabled add appearance may use renderer opacity; it does not require another texture. Keep glyphs outside stretchable textures and render the plus separately if the button stretches.
- Existing audio loops already have IDs; reuse these rather than introducing competing identity. Extend stable identity to stations, takes in the JAM manifest, MIDI streams and plugin instances. Reject duplicate identity ambiguity on import; generate IDs only for absent legacy IDs. Group references never keep deleted targets alive.
- Selection capture must include MIDI streams as well as audio loops. Apply leaf choices and aggregate flags without parent propagation overwriting the captured leaves. New targets remain untouched even beneath captured parents.
- Both GetState and SetState require processor exclusion: non-audio execution alone is insufficient. Audit capture as well as restore, including editor/controller calls and automation delivery. Implement and test a bounded audio acknowledgement/owner handoff or compatible staged processor lifecycle before enabling live blob access. Callback participation must never wait, allocate, lock, destroy processors, or serialize blobs. Failed or superseded work must not partially overwrite a stored slot.
- Bound imported group target counts and total decoded blob bytes before allocating. Validate explicit slots, duplicate slots/IDs, target kinds and blobs; keep legacy fields distinct from the new snapshot records. Define round-trip tests for empty occupied groups and sparse rows.
- Execution: implement in dependency order, with an independent subagent review for each phase before committing its focused diff. Assets may be prepared independently once their contract is fixed. Re-read local tasks before each build/test. Record checks and any unresolved limitations honestly; completion requires engine, plugins, persistence, input, widgets and assets, not only the slot model.

## Implementation and validation record

Original implementation record (before the function-key and selection-panel follow-up): sparse nine-slot model; persistent station, take, audio/MIDI-loop and plugin identities; local finest-level mute/selection snapshots; dedicated controller and UI retry pump; digit/Shift/Alt and Backspace precedence; coherent audio-boundary mute publication; live plugin state access exclusion and MIDI recovery; optional bounded JAM records; two animated group rows; actual HUD bounds constraining settings; all ten registered textures and editable SVG sources.

The asset state sheet is [performance-group-assets.png](images/performance-group-assets.png). This shows the texture families and tints, rather than an application screenshot. Labels and glyphs are rendered separately. Occupancy uses the filled texture family; last-applied indication uses a separate white cue.

Validation completed:

- Incremental Debug x64 native library/test and Jamma application builds succeeded using the copied local tasks.
- All **1,440 native tests across 175 suites passed**, including added slot, sparse persistence/blob validation, identity migration, audio/MIDI leaf selection, reordered/deleted targets, running audio-boundary application/retirement, shortcut/focus/rapid inversion, atomic state-access concurrency, MIDI recovery capacity/order, animated hit bounds and capture cancellation tests.
- Mixer transitions now apply sample-continuous gain with fixed stack scratch during ramps and retain the direct steady-gain path. Bounce uses complementary old/new per-sample coefficients. Both continuity and bounce regression tests pass.
- Asset validation checked type-2 uncompressed 32-bit bottom-left straight-alpha TGAs, tintable grayscale, alpha edges, aligned states and uniform exact nine-patch slices. Native geometry tests cover small viewports and settings collapse; the asset state sheet was visually inspected.
- The threading-review audit flagged new Scene input/frame locks and owner VST path-metadata locks because it scans whole-file additions. Manual function review confirmed these are non-audio methods. Added callback code has no host heap allocation, mutex, wait, state-blob call, logging or ownership reclamation.

### Shared-state ownership

| State | Owner / access | Synchronization and teardown |
| --- | --- | --- |
| Rows, pending capture/recall, key state, presented geometry | UI owner; Scene serializes UI/job hierarchy access | Existing non-audio Scene mutex. Queued work cancels on topology/session teardown; focus loss clears key latches. |
| Stable IDs | Assigned on construction/import before publication; non-audio resolution/export | Immutable during publication; no positional matching. Preserve legacy take Name identities and explicit modern IDs. |
| Prepared mute application | Non-audio writer; callback reads immutable resolved values | PublishedAudioSnapshot borrow; bounded at 32,768 targets. Atomic version receipt; writer collection retires references off audio. Callback stops before publisher destruction. |
| Plugin exclusive access / generation / pending MIDI recovery | UI state operations; callback and editor entry guards | Bounded atomic guard/reservation, no callback retries or waits. State operations run only when readers are excluded. Instance lifetime remains with existing owners/snapshots. |
| Reserved MIDI events | Callback owns fixed normal + 16 panic slots | No shared container mutation during exclusive state work; panic request is atomic. Recovery precedes ordinary queued events and survives bypass. |
| Mixer gain and scratch | Callback owns fade progression and stack scratch; UI publishes mute/target scalars | Existing atomic target/mute publication. No allocation or lock in the new ramp path. |

Manual checks still required on the user's live setup: DPI/display appearance, routing/Ctrl overlays and rapid pointer gestures in the running app; representative VST2/VST3 editors and audible third-party preset behavior. A vendor state call runs synchronously on its UI owner once safely acquired and may take an unpredictable time; skipped processing/MIDI is recovered, but arbitrary plugin presets are not promised click-free. The existing void SetState API retains vendor failure logging rather than claiming verified SDK success.

### Follow-up: live-performance mute recall

Recall prepares mute choices for every current local take and audio loop. Saved identities use their captured choices; missing identities default to muted, including later-added stations and takes and later-added loops within an existing take. Remote stations remain excluded. Alternating old and newer groups restores their respective material deterministically. Lookup/traversal stays on the UI/job side and publishes through the existing bounded audio application; no callback synchronization or fade changes are needed.

Mute-group recall no longer restores VST blobs or bypass state, including blobs in previously saved JAM groups. Even restoring state on an existing instance can trigger vendor sample/cache work, block the UI thread and exclude audio processing. Recall only publishes the resolved mute application for the next audio boundary; the UI pump has no pending plugin recall work. Existing instances, editor state and automation continue unchanged.

Explicit store/overwrite (including capture into an empty slot) still captures plugin blobs for persistence compatibility. This is preparation work and can still take time in third-party plugins; it is separate from switching occupied groups. Plugin blob persistence and normal JAM session loading remain unchanged.

A native regression exercises rapid occupied-group switching with both station and take plugins, including busy plugins and a later UI pump. It checks the newest mute publication and verifies no capture, restore, bypass, lifecycle or automation-generation changes.

### Follow-up: plugin-induced focus reentry

The user's Kontakt 5 exception stack showed SetState synchronously sending WM_KILLFOCUS while Scene::AdvanceUiAnimations held the Scene mutex. Immediate focus cleanup entered Scene::_InteractionContext and attempted to lock that same non-recursive mutex. Window now latches focus/capture-loss notifications on its UI owner and drains them before the UI frame and after the group-state pump returns, outside the Scene lock. Capture-only requests apply only to active gestures and are discarded when a newer press starts.

At this stage, group release consumption was changed to survive focus loss so a recalled plugin could not send the consumed digit `1` release through to its configured trigger. The later function-key follow-up retains release ownership for `F1` through `F9` while freeing digits for normal triggering. A fresh contextual or unsupported-modifier press clears stale consumption if the original release went to another window.

Independent review passed. All **1,445 native tests** pass, including synchronous native focus/capture notifications under the held Scene mutex, active pointer cancellation, newer gesture preservation, group/contextual release ownership, and a real published rig with digit `1` assigned to trigger activation. The Debug x64 app rebuild succeeded. The threading audit found no new callback lock/wait additions; all changes are on UI input paths. Live Kontakt confirmation remains user-side.


### Follow-up: VST2/VST3 reentry audit

The broader audit found owner metadata mutexes spanning state capture, native editor activation/close under the editor-manager mutex, and VST3 MIDI-map SDK calls under a rebuild mutex. Owner capture now copies metadata while locked and calls plugins after unlocking. Editor ownership decisions stay locked, while native calls and retired plugin destruction run after unlock. VST2 adapter host resize/display/idle callbacks already post messages; startup PreInit and queued plugin destruction already execute outside host lock scopes.

VST3 controller housekeeping uses bounded exclusive access through the existing plugin gate, excluding callback map readers and nested editor polls. Parameter/MIDI rebuilds use a bounded atomic writer reservation and preserve deferred requests. No callback waits, locks or allocations were added.

Window UI dispatch now retains self-contained native input/resize/move messages in fixed storage until its outer Scene call returns; nested rendering is suppressed. Transient raw-input handles are cleaned up immediately and request pointer cancellation. Overflow discards the entire partial gesture and requests input reset. Native destruction cleanup runs after the Scene call unwinds. Editor idle rejects nested timer/idle dispatch; managed editor callbacks retain ownership through plugin message pumps and pending editor opens suppress recursive creation. Close requests during idle or live/raw state access retain the editor until the existing timer can safely detach it. VST2/VST3 raw blob calls expose bounded atomic activity and reject recursive blob access; their existing audio-quiescence precondition remains mandatory. Pointer resampling preserves a retrieved WM_QUIT notification instead of swallowing it.

New shared state is UI-owned: Window dispatch depth, fixed message storage, overflow/destruction flags, and editor idle/close state have no audio readers. Manager editor/pending-open ownership remains mutex-protected off audio; retired windows release references off lock. VST3 housekeeping/rebuild reservations and shared adapter blob activity use bounded atomics, are released by RAII, and live with the retained plugin instance. Deferred editor self-ownership is UI-only and is released during safe timer-driven close.


Validation: all **1,461 native tests across 176 suites** pass, including 16 added regressions for reentrant owner capture, controller exclusion, nested input/resize/destruction, editor creation/idle/lifetime, and live/raw state close deferral. Debug x64 tests and app incremental builds succeeded. Independent review covered VST2, VST3 and native UI ownership. The whole-file hot-path audit flags four relocated path-metadata lock lines in non-audio capture/export; manual callback inspection confirms no new callback locks, waits, allocations or logging. One earlier full run transiently failed the unchanged trigger-history restoration test; it passed five focused repeats and the final full run. Representative live Kontakt/VST editor behavior remains a manual runtime check.


### Follow-up: function keys, selection panel and hover preview (2026-10-12)

Mute buttons now show F1-F9; selection buttons retain numeric slot labels. Shortcut help includes Shift+F-key store and Alt+F-key clear. Keyboard storage addresses visible slots plus their next add position: empty-row F2/Shift+F2 does nothing, while sparse recalls and visible holes remain valid. Digits continue through normal trigger routing. Both rows live below the depth radios inside the collapsible selection panel, sharing their computed width, clipping, animation and pointer cleanup.

Hover preview highlights audible members of mute snapshots in blue and selected members of selection snapshots in yellow, greying other local items through the existing rendering path. Live identities are checked so deleted or reused GUI targets do not inherit stale membership. This is a visual preview and does not apply group state; leaving the button, collapse and hidden ancestors clear it.

Independent source review passed. Final incremental Debug x64 JammaLib, native test and application builds succeeded with zero warnings/errors using freshly read local tasks. The focused run passed **83 tests across 16 suites**; the full native target passed **1,482 tests across 177 suites**. This includes shortcut/creation boundaries, downstream digits, selection-panel layout and pointer cleanup, hover lifecycle/identity, and the updated function-key trigger-fallback regression. Historical counts above describe earlier stages.

The bundled Inter font metrics were checked against the normal 384px-wide row: F-key labels fit the approximately 26px full-row buttons, and shortcut/add help fits the 376px tooltip text area. Existing per-label scissoring and tooltip ellipsis cover narrower widths. When less than 98px remains for settings below selection, layout retains its existing minimum-height fallback to keep settings usable; the two overlays can overlap in very small windows.

Manual verification remains pending for shader compilation/rendering on the user's graphics driver, DPI/display appearance, expanded/collapsed gestures, source/routing overlays, small windows, text/plugin-editor input contexts, and live device/audio behavior. Native tests and font measurements do not establish those runtime checks.
