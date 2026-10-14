#pragma once

#include <algorithm>
#include <chrono>
#include "MidiDevice.h"

namespace midi
{
	// Job-owned cadence and pure refresh decisions. No device or Scene access.
	class MidiRefreshPolicy
	{
	public:
		bool ShouldPoll(std::chrono::steady_clock::time_point now, bool requested) noexcept
		{
			if (!requested && now - _lastCheck < std::chrono::seconds(1)) return false;
			_lastCheck = now; // Failed enumeration still observes the existing poll interval.
			return true;
		}

		static bool ShouldRefresh(const MidiConnectionResult& previous,
			const MidiInputInventory& inventory, bool requested)
		{
			if (!inventory.Error.empty()) return false;
			const auto& oldPorts = previous.Inventory.Devices;
			const bool same = oldPorts.size() == inventory.Devices.size()
				&& std::equal(oldPorts.begin(), oldPorts.end(), inventory.Devices.begin(),
					[](const auto& a, const auto& b) { return a.DeviceId == b.DeviceId && a.PortName == b.PortName; });
			if (!same) return true;
			return requested && std::any_of(previous.Attempts.begin(), previous.Attempts.end(),
				[](const auto& attempt) { return attempt.Status == MidiConnectionStatus::Missing
					|| attempt.Status == MidiConnectionStatus::Failed; });
		}

	private:
		std::chrono::steady_clock::time_point _lastCheck{};
	};
}
