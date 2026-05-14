Things to fix in overdub timing, based on review in docs/overdub-timing.md:

* When overdubbing, the source loop offset should be -1 * (MaxLoopFadeSamps + PreDelay + OutLatency). OutLatency is required so the trigger aligns with the exact moment in the music when it was pressed in the real world (the loop playback is heard outLatency samps later, and the user triggering uses their ears to judge timing).

* Punch-in and punch-out actions have immediate effect on the target loop, but their actions should be delayed by MaxLoopFadeSamps + PreDelay (like the mixer). We don't need to account for the output latency is needed so 

* Like with punch-in compensation of audio output latency for trigger alignment with the real world, the same should apply to overdub start and end triggers so that these line up too.

* On finishing an overdubbed loop recording, the _playIndex of this loop should be offset by MaxLoopFadeSamps + PreDelay.  We don't need to include OutLatency here because it is already applied in the source loop offset during recording.

* The muting of the original loop whilst punched-in should still be immediate (in terms of being audible through the DAC).  However, it should not be muted when being bounced at all - muting should only apply to the DAC output, not the internal routing.

* The STATE_OVERDUBBINGRECORDING state is applied at the end of an overdub session, but this wipes over any delayed punching in that may be active.  We may be still recording live ADC input into the target loop after end of overdubbing due to delayed punch-in/out, where this delay is accounting for the MaxLoopFadeSamps lead-in.  The STATE_OVERDUBBINGRECORDING should not stop that.  We may need a separate flag for the delayed internal punchin status.

* The target loop should still be playable in the case that the overdub end trigger action has executed, but the loop is still recording live ADC input (i.e. still in delayed punchin state). So STATE_PUNCHEDIN is not exclusive with respect to playback state - playback state could be obtained from ORing the new delayed punchin flag (or ANDing) it with the existing state.

