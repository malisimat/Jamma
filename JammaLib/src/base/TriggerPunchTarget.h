#pragma once

namespace base
{
	// Trigger history borrows punch targets, so calls must be real-time safe.
	class TriggerPunchTarget
	{
	public:
		virtual ~TriggerPunchTarget() = default;
		// Structural job owner only: freeze prepared capture material before publication.
		virtual bool HasTriggerAudioCapture() const noexcept { return true; }
		virtual bool HasPendingTriggerCapture() const noexcept { return true; }
		virtual bool IsTriggerCaptureInactive() const noexcept { return false; }
		virtual void AcquireTriggerSourceMuteAudio() noexcept { SetTriggerSourceMutedAudio(true); }
		virtual void ReleaseTriggerSourceMuteAudio() noexcept { SetTriggerSourceMutedAudio(false); }
		virtual void SetTriggerSourceMutedAudio(bool muted) noexcept = 0;
		virtual void TriggerPunchInAudio() noexcept = 0;
		virtual void TriggerPunchOutAudio() noexcept = 0;
	};
}
