#pragma once

#include <array>
#include <memory>
#include <gl/glew.h>
#include "glm/glm.hpp"
#include "../utils/CommonTypes.h"
#include "../resources/ResourceLib.h"
#include "../base/DrawContext.h"

namespace graphics
{
	class GlDrawContext;

	// Lightweight 2D screen-space overlay that shows a small row of coloured
	// handle buttons near the cursor while Ctrl is held.  Each button has a
	// distinct hue so the user can tell them apart at a glance.
	class CtrlHandleOverlay
	{
	public:
		CtrlHandleOverlay();
		~CtrlHandleOverlay();

		CtrlHandleOverlay(const CtrlHandleOverlay&) = delete;
		CtrlHandleOverlay& operator=(const CtrlHandleOverlay&) = delete;

		// Update the anchor pixel position and the scene viewport size.
		// Call this when Ctrl is pressed to pin the panel location.
		void SetAnchor(utils::Position2d screenPos, utils::Size2d sceneSize) noexcept;
		int HitTestButton(utils::Position2d pos) const noexcept;

		// Set the blended alpha (0–1).  The overlay is invisible at 0.
		void SetAlpha(float alpha) noexcept;
		float Alpha() const noexcept { return _alpha; }

		void InitResources(resources::ResourceLib& resourceLib, bool forceInit);
		void ReleaseResources();

		void Draw(base::DrawContext& ctx);

	private:
		struct ButtonSpec
		{
			float hue;
		};

		static constexpr int NumButtons = 2;
		static constexpr float ButtonW = 72.0f;
		static constexpr float ButtonH = 28.0f;
		static constexpr float ButtonGap = 6.0f;
		static constexpr float AnchorOffsetY = 14.0f;

		// Button 0: Phase drag (cyan-blue). Button 1: Fraction drag (orange).
		static constexpr std::array<ButtonSpec, NumButtons> ButtonSpecs = {{
			{ 0.58f },
			{ 0.11f }
		}};

		static glm::vec3 HsvToRgb(float h, float s, float v) noexcept;

		GLuint _vertexArray = 0;
		GLuint _vertexBuffer = 0;
		float _alpha = 0.0f;
		utils::Position2d _anchorPos{};
		utils::Size2d _sceneSize{};
		float _panelX = 0.0f;
		float _panelY = 0.0f;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
