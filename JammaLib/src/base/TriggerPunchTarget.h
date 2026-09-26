#pragma once

namespace base
{
	// Narrow, non-owning audio-boundary interface retained by Trigger history.
	// Implementations must remain allocation-free, lock-free, and noexcept.
	class TriggerPunchTarget
	{
	public:
		virtual ~TriggerPunchTarget() = default;
		virtual void SetTriggerSourceMutedAudio(bool muted) noexcept = 0;
		virtual void TriggerPunchInAudio() noexcept = 0;
		virtual void TriggerPunchOutAudio() noexcept = 0;
	};
}
