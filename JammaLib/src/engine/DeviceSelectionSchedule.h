#pragma once
#include <chrono>
#include <cstdint>

namespace engine
{
	// UI-only latest-value scheduling. Hardware work has a separate admitted revision;
	// completion never consumes edits made after that revision.
	struct DeviceSelectionSchedule
	{
		using Clock = std::chrono::steady_clock;
		enum class Edit { Device, Channels, Format, Retry };
		std::uint64_t Revision = 0u;
		std::uint64_t AttemptedRevision = 0u;
		Clock::time_point EditDeadline{}, NextChange{};
		void Changed(Edit edit, Clock::time_point now = Clock::now()) noexcept
		{
			++Revision;
			if (edit == Edit::Device || edit == Edit::Retry) EditDeadline = {};
			else EditDeadline = now + std::chrono::seconds(3);
		}
		bool Eligible(Clock::time_point now) const noexcept
		{
			return Revision != AttemptedRevision && now >= EditDeadline && now >= NextChange;
		}
		void Attempted(std::uint64_t admittedRevision, Clock::time_point now) noexcept
		{
			AttemptedRevision = admittedRevision;
			NextChange = now + std::chrono::milliseconds(500);
		}
	};
}
