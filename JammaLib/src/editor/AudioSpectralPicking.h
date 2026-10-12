#pragma once

#include <optional>
#include "../graphics/AudioSpectralModel.h"

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
