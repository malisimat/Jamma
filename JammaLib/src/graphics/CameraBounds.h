#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include "../utils/CommonTypes.h"

namespace graphics
{
	// Presentation-only axis-aligned bounds. Empty bounds are invalid until included.
	struct CameraBounds
	{
		utils::Position3d Min{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
		utils::Position3d Max{ -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };

		bool IsValid() const noexcept
		{
			return std::isfinite(Min.X) && std::isfinite(Min.Y) && std::isfinite(Min.Z) &&
				std::isfinite(Max.X) && std::isfinite(Max.Y) && std::isfinite(Max.Z) &&
				Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
		}

		utils::Position3d Centre() const noexcept
		{
			return IsValid() ? utils::Position3d{ Min.X * 0.5f + Max.X * 0.5f,
				Min.Y * 0.5f + Max.Y * 0.5f, Min.Z * 0.5f + Max.Z * 0.5f } : utils::Position3d{};
		}

		void Include(utils::Position3d point) noexcept
		{
			if (!std::isfinite(point.X) || !std::isfinite(point.Y) || !std::isfinite(point.Z))
				return;
			Min = { (std::min)(Min.X, point.X), (std::min)(Min.Y, point.Y), (std::min)(Min.Z, point.Z) };
			Max = { (std::max)(Max.X, point.X), (std::max)(Max.Y, point.Y), (std::max)(Max.Z, point.Z) };
		}

		void Include(const CameraBounds& bounds) noexcept
		{
			if (!bounds.IsValid())
				return;
			Include(bounds.Min);
			Include(bounds.Max);
		}

		CameraBounds Transformed(utils::Position3d position, float uniformScale) const noexcept
		{
			if (!IsValid() || !std::isfinite(uniformScale) ||
				!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(position.Z))
				return {};
			const auto first = position + Min * utils::Position3d{ uniformScale, uniformScale, uniformScale };
			const auto second = position + Max * utils::Position3d{ uniformScale, uniformScale, uniformScale };
			CameraBounds result;
			result.Include(first);
			result.Include(second);
			return result;
		}
	};
}
