#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include "CameraBounds.h"

namespace graphics
{
	class CameraFraming
	{
	public:
		static constexpr float FrontFieldOfViewDegrees = 80.0f;
		static constexpr float NearPlane = 10.0f;
		static constexpr float NearMargin = 20.0f;
		static constexpr float OverviewPadding = 1.15f;
		static constexpr float CloseDistance = 240.0f;

		static std::optional<utils::Position3d> Overview(const CameraBounds& bounds,
			float aspectRatio) noexcept
		{
			if (!bounds.IsValid() || !std::isfinite(aspectRatio) || aspectRatio <= 0.0f)
				return std::nullopt;
			const auto centre = bounds.Centre();
			const auto halfWidth = (bounds.Max.X - bounds.Min.X) * 0.5f;
			const auto halfHeight = (bounds.Max.Y - bounds.Min.Y) * 0.5f;
			const auto halfDepth = (bounds.Max.Z - bounds.Min.Z) * 0.5f;
			const auto tangent = std::tan(FrontFieldOfViewDegrees * 0.5f * 0.01745329252f);
			const auto distance = halfDepth + (std::max)(NearPlane + NearMargin,
				OverviewPadding * (std::max)(halfHeight / tangent, halfWidth / (aspectRatio * tangent)));
			const utils::Position3d eye{ centre.X, centre.Y, centre.Z + distance };
			return _Finite(eye) ? std::optional{ eye } : std::nullopt;
		}

		// Close framing deliberately clips width/height. Only the frontmost depth
		// constrains the dolly distance, so the eye cannot enter the visual mesh.
		static std::optional<utils::Position3d> Close(utils::Position3d centre,
			const CameraBounds& bounds) noexcept
		{
			if (!bounds.IsValid() || !_Finite(centre)) return std::nullopt;
			const utils::Position3d eye{ centre.X, centre.Y,
				(std::max)(centre.Z + CloseDistance, bounds.Max.Z + NearPlane + NearMargin) };
			return _Finite(eye) ? std::optional{ eye } : std::nullopt;
		}

	private:
		static bool _Finite(utils::Position3d value) noexcept
		{
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}
	};
}
