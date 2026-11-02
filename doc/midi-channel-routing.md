# Station MIDI channel routing

Station channel buttons and cables control both live input and recorded MIDI reaching the station rack. UI and saved channel numbers are 1-16; event channels are 0-15. Take/output indices identify playback sources, not MIDI channels.

- Disabled: admit no new rack events or capture on that channel. Existing recorded material stays stored.
- Enabled with no cables: broadcast once to every MIDI-capable rack plugin.
- Enabled with cables: send only to the selected identities. Unavailable identities remain cables and are silent until recovered; they do not cause broadcast.

Disabling clears the channel's cables. Re-enabling starts in broadcast mode and does not replay suppressed attacks or reconstruct held physical notes. Removing the last cable also returns to broadcast. A cancelled, invalid or stale cable gesture leaves the prior state intact.

New stations enable all 16 channels without cables. Loading restores saved channel choices, including an empty enabled set. The supported JAM representation is `allowedmidichannels` plus `midiChannelRoutes` (16 objects containing `plugins` identity lists). Obsolete output-index/live-plugin maps and the separate explicit-empty mode are no longer supported or migrated. Recorded material on a saved disabled channel is now silent until enabled.

Rack routing is published with the enabled mask in one immutable audio snapshot. Per-destination/channel retirement counters preserve removal history across skipped publications, so disable/re-enable cannot revive a queued recorded attack. Delivery retires only obsolete ownership and retains bounded release retries to the old processor. Recorded MIDI retains destination suffix latency compensation and non-realtime flags; live MIDI stays immediate. Take-local processing, trigger routing and external MIDI output are unchanged.

Ordinary note edits preserve the articulation actually accepted by each playback source. MidiLoop assigns transient loop-local tokens to raw attacks; sort, trim and undo carry those sidecars, while saved MIDI bytes and flags remain unchanged. A covered start/end extension keeps its sounding voice; deletion, changed channel/pitch or lost current coverage releases only that voice. Velocity affects future attacks. A new onset already behind the source cursor waits for its next occurrence. Source/grid replacement and explicit relocation retain full recovery, including skipped publications; canceled grid preparation cannot become an ordinary edit's recovery lineage.

Recorded destination queues carry the same transient articulation provenance through the existing source-to-device mapping. An unrelated edit retains queued short-note attacks/releases even when the source cursor has passed their end. A deleted or moved queued attack is discarded; a queued release follows the accepted articulation's revised endpoint, including circular start extensions, after the source has already accepted its old release. Delivery adjusts endpoint deltas in the already mapped clock and retains destination ownership and partial-release retries. Preparation is off callback; adoption is bounded by 4096 source articulations and 2048 output slots, and each of at most 1024 scheduled entries per destination uses a bounded binary token lookup. No snapshot pointer escapes its reader lease.

Broadcast excludes destinations already supplied MIDI by an upstream plugin when such a connection exists. The current host has no plugin-to-plugin MIDI forwarding: `VstChain` processes rack audio, `IVstPlugin` exposes MIDI input only, and `Vst3Plugin` sets `processData.outputEvents` to null. Consequently all MIDI-capable rack plugins are independent direct broadcast recipients today. Audio order alone must never be used to infer MIDI supply. Adding plugin MIDI output/forwarding in future must prepare that actual connection topology and exclude its supplied destinations from direct broadcast.

## VST rack order

Drag a row's `=` handle onto another row to move it to that final slot position.
Slot buttons still open editors; sockets and cables still edit MIDI routing.
Release outside the visible rack, press Escape, right-click, or lose pointer
capture to cancel. A changed owner or rack revision also cancels the gesture.
Take racks show the take's slots, or the single selected loop's slots at Loop
depth. The order is saved with each owning station, take or loop.

Moving a slot retains its plugin instance, editor, state and automation identity.
Explicit MIDI cables continue to address the same identities, including unavailable
slots. Multiple instruments remain additive; effects process the accumulated bus
in slot order. Intermediate or non-producing arrangements are allowed. A failed
slot remains silent and recoverable; unsupported layouts are never reinterpreted.
