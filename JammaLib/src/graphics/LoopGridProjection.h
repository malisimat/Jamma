#pragma once

#include <optional>
#include <cmath>

#include "glm/glm.hpp"
#include "../utils/CommonTypes.h"

namespace graphics
{
	// Pure world-to-pixel mapping shared by editor rulers and future pointer math.
	struct LoopGridProjection
	{
		static float CameraDistance(float radius, float worldScale,
			float viewportAspect) noexcept
		{
			if (!(radius > 0.0f) || !(worldScale > 0.0f)
				|| !(viewportAspect > 0.0f)) return 340.0f;
			constexpr float tanHalfFov = 0.383864f; // 42 degree editor perspective.
			const auto halfWidth = radius * worldScale;
			const auto halfHeight = radius * worldScale * 0.78f;
			return 1.40f * std::max(halfWidth / (tanHalfFov * viewportAspect),
				halfHeight / tanHalfFov) + 40.0f;
		}

		// Screen coordinates use the app's bottom-left pixel origin.
		static std::optional<glm::vec3> UnprojectToLocalPlane(
			const glm::mat4& viewProjection, const glm::mat4& model,
			utils::Position2d pixel, int width, int height, float planeY) noexcept
		{
			if (width <= 0 || height <= 0) return std::nullopt;
			const auto inverse = glm::inverse(viewProjection * model);
			const auto x = 2.0f * static_cast<float>(pixel.X) / width - 1.0f;
			const auto y = 2.0f * static_cast<float>(pixel.Y) / height - 1.0f;
			auto nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
			auto farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);
			if (std::abs(nearPoint.w) < 1e-7f || std::abs(farPoint.w) < 1e-7f)
				return std::nullopt;
			const auto nearLocal = glm::vec3(nearPoint) / nearPoint.w;
			const auto farLocal = glm::vec3(farPoint) / farPoint.w;
			const auto delta = farLocal - nearLocal;
			if (std::abs(delta.y) < 1e-7f) return std::nullopt;
			const auto distance = (planeY - nearLocal.y) / delta.y;
			if (distance < 0.0f || distance > 1.0f) return std::nullopt;
			const auto local = nearLocal + delta * distance;
			if (!std::isfinite(local.x) || !std::isfinite(local.z)) return std::nullopt;
			return local;
		}

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
