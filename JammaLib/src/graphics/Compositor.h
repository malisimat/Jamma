#pragma once

#include <memory>
#include <optional>
#include "Camera.h"
#include "Skybox.h"
#include "../base/Drawable.h"
#include "../gui/GuiLabel.h"
#include "../utils/Timer.h"

namespace graphics
{
	// Window-thread render composition. The caller retains session locking,
	// hierarchy/UI ownership and the cached projection used for picking.
	class Compositor
	{
	public:
		struct Tempo { float Bpm; unsigned int Bpi; };
		struct WorldPresentation
		{
			bool SelectionActive;
			float SurroundingDim;
			bool UseProbe;
			glm::vec3 ProbeEye;
		};
		Compositor();
		void InitResources(resources::ResourceLib& resources, bool forceInit);
		void ReleaseResources();
		void Resize(utils::Size2d viewport);
		void BeginOverlay(GlDrawContext& context) const;
		static void EndOverlay(GlDrawContext& context);
		void DrawStatus(base::DrawContext& context, utils::Size2d viewport, std::optional<Tempo> tempo);
		void DrawBackground(GlDrawContext& context, const Camera& camera, utils::Size2d viewport);
		static glm::mat4 BeginWorld(GlDrawContext& context, const Camera& camera,
			utils::Size2d viewport, utils::Position3d centre, base::DrawPass pass);
		bool SetWorldPresentation(GlDrawContext& context, const WorldPresentation& presentation) const;
		static void EndProjection(GlDrawContext& context);
		static void EndWorld(GlDrawContext& context, bool probeBound);

	private:
		glm::mat4 _overlayViewProjection{};
		Skybox _skybox;
		bool _skyboxStarted = false;
		Time _skyboxStartTime = utils::Timer::GetZero();
		std::unique_ptr<gui::GuiLabel> _tempoLabel, _versionLabel;
	};
}
