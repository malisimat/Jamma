#pragma once

#include <optional>

#include "glm/glm.hpp"
#include "../utils/CommonTypes.h"

namespace graphics
{
	// Pure world-to-pixel mapping shared by editor rulers and future pointer math.
	struct LoopGridProjection
	{
		static std::optional<utils::Position2d> Project(const glm::mat4& viewProjection,
			const glm::mat4& model, glm::vec3 local, int width, int height) noexcept
		{
			if (width <= 0 || height <= 0)
				return std::nullopt;
			const auto clip = viewProjection * model * glm::vec4(local, 1.0f);
			if (!(clip.w > 0.0f))
				return std::nullopt;
			return utils::Position2d{
				static_cast<int>((clip.x / clip.w + 1.0f) * 0.5f * width),
				static_cast<int>((clip.y / clip.w + 1.0f) * 0.5f * height)
			};
		}
	};
}
