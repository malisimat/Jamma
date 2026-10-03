#pragma once

#include <chrono>

namespace console
{
	inline bool StatusUpdateDue(std::chrono::steady_clock::time_point last,
		std::chrono::steady_clock::time_point now) noexcept
	{
		return now - last >= std::chrono::seconds(1);
	}
}
