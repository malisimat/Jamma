#pragma once

namespace base
{
	// Trigger history borrows punch targets, so calls must be real-time safe.
	class TriggerPunchTarget
	{
	public:
		virtual ~TriggerPunchTarget() = default;
		virtual void SetTriggerSourceMutedAudio(bool muted) noexcept = 0;
		virtual void TriggerPunchInAudio() noexcept = 0;
		virtual void TriggerPunchOutAudio() noexcept = 0;
	};
}
