#pragma once

#include <optional>
#include "../graphics/AudioSpectralModel.h"
#include "../graphics/LoopGridProjection.h"

namespace editor
{
	class AudioSpectralPicking
	{
	public:
		struct Hit
		{
			double TimeU = 0.0;
			double FrequencyV = 0.0;
			double Distance = 0.0; // Local-space distance along the normalized ray.
		};
		// Surface is the model's already-validated DisplayedSurface, with the exact
		// view/model ray supplied by the caller. Opening/closing surfaces reject hits.
		static std::optional<Hit> Intersect(const graphics::AudioSpectralModel::Surface& surface,
			glm::vec3 localOrigin, glm::vec3 localDirection) noexcept;
		// Freeze screen Y at click height: vertical adjustment cannot move time.
		// The clicked sample remains authoritative despite pixel projection rounding.
		static std::optional<double> BandTime(const glm::mat4& clipFromLocal,
			float planeY, float radius, utils::Position2d anchor, utils::Position2d pointer,
			int width, int height, double clickedSeconds, double duration) noexcept
		{
			if (!(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(duration) || duration <= 0.0
				|| !std::isfinite(clickedSeconds)) return std::nullopt;
			pointer.Y = anchor.Y;
			const auto first = graphics::LoopGridProjection::UnprojectToLocalPlane(clipFromLocal,
				glm::mat4(1.0f), anchor, width, height, planeY);
			const auto current = graphics::LoopGridProjection::UnprojectToLocalPlane(clipFromLocal,
				glm::mat4(1.0f), pointer, width, height, planeY);
			if (!first || !current) return std::nullopt;
			return std::clamp(clickedSeconds + static_cast<double>(current->x - first->x) * duration / (2.0 * radius),
				0.0, duration);
		}

	private:
		static bool _Finite(glm::dvec3 value) noexcept;
		static bool _Slab(double origin, double direction, double minimum, double maximum,
			double& first, double& last) noexcept;
		static std::optional<Hit> _Triangle(glm::dvec3 origin, glm::dvec3 direction,
			glm::dvec3 a, glm::dvec3 b, glm::dvec3 c,
			glm::dvec2 uvA, glm::dvec2 uvB, glm::dvec2 uvC,
			double first, double last, double positionTolerance) noexcept;
	};
}
