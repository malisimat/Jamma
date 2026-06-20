# MIDI Automation - Review of Implementation

This doc notes a number of issues in the current implementation of VST automation recording/playback, primarily focussed on reacting to parameter changes from the VST editor window, and also playing back already-recorded automation values to the VST.

## What is good

MidiRouter::IsParameterSuppressed preventing live per-param updates being sent back to VST until expiry has elapsed.

## Problems:

### Incorrect VST2 implementation
The host MUST call setParameterAutomated (on the VST instance) in response to changes received from the plugin calling audioMasterAutomate callback to host.  This is expected host behaviour, and is not happening.

### Calling setParameter incorrectly
We must not call setParameter too frequently in the audio callback - typically just once per block is sufficient (with the latest extrapolated value of each parameter in that block).  So if linearly interpolating between two cointrol points, and we start at P1+delta and at end of block we would be at P1+delta+block then send the value at P1+delta+block.

### AutomationEpsilon
The current epsilon approach (to not permit parameter changes too close to each other) may result in inaccuracies.  Either remove epsilon or make it so small (just a few milliseconds) that it will not degrade timing accuracy.

### Recordsing and overwriting control points
When a parameter value is received by the host via audioMasterAutomate call, we must immediately set automation record mode on (if not already), set a control point at current position, and also another control point 800ms in the future (wiping everything in between).  This will also ensure to wipe previous 'end of 800ms' control points written previously by this same logic which are no longer required - for example if we get param values at 0ms, 700ms, 1400ms then we should end up with control points at 0ms, 700ms, 1400ms and (1400+800)=3200ms.  The point written at 800ms (when the param received at 0ms) is wiped by the subsequent param value at 700ms, since it will wipe from 700ms to (700+800=1500ms) and leave a point at both 700ms and 1500ms.  Then the param at 1400ms will wipe the one at 1500ms, and leave a point at 1400ms and 3200ms.

### Too low a limit on control points
The current limit on number of points is too small - ideally the number of control points should be unlimited (but could merge any within a few milliseconds of each other).  The logic to drop control points is not desirable.

### Incorrect writing of control points
We seem to be continuously writing control points during PumpMidi even with no change - this is not correct. No points should be written here - we do want to keep track of whether 800ms has elapsed (to exit automation recording) but that can be done elsewhere.

### Incorrect header comment
Header comment in MidiRouter.h still says fresh drag wipes lane points, but implementation in MidiRouter.cpp explicitly says "Do not clear the lane" and does not call clear.  Ref: JammaLib/src/midi/MidiRouter.h#L193

### EditorTouchState
Comments indicate that VST params are written for every call to PumpMidi even if no change (confirmed in MidiRouter::ConsumeEditorAutomation implementation).  That is incorrect - we should not write points if no change. By wiping all stored control points into the future 800ms after every actual change in param coming from CC or VST editor, and writing a control point value at both the start and end of this period, we avoid the need to continuously repeat the same parm value in the automation lane.  Subsequent pumps do not affect this - we only write points/wipe points in response to changes, not regularly. 

### RefreshAutomationSuppression calls
We must call RefreshAutomationSuppression after every change in VST parameter received from the editor (and ideally from external CC, although that is out of scope for now).  However, RefreshAutomationSuppression(...) is currently called in two places inside MidiRouter::_ConsumeEditorAutomation(...):

1. On a new VST editor touch (newTouch == true)
  - It refreshes the suppression entry for that (plugin, paramIdx).
  - This happens when vst::_lastTouchedParam.Sequence changes.
  - On every pump for every active editor touch state

2. In the for (auto& state : _editorTouchStates) loop.
  - As long as the touch state is active and not expired, suppression is refreshed again each cycle.
  - So playback stays suppressed continuously near the live editor write-head.

This is wrong.  Yes it should be called on first touch, but also EVERY touch.  Also it should not be called inside PumpMidi, there is no reason to keep refreshing that there, the expirySamp should represent the true sample pos to deactivate, which does not change based on PumpMidi calls - only in response to an actual received change in VST parameter.