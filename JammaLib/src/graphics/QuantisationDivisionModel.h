#pragma once

#include <vector>
#include <glm/vec3.hpp>
#include "../engine/Quantiser.h"
#include "../gui/GuiModel.h"
#include "Timer.h"

namespace engine
{
	class QuantisationDivisionModel :
		public gui::GuiModel
	{
	public:
		QuantisationDivisionModel();

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetLoopTakeVisuals(const std::vector<engine::QuantisationLoopTakeVisual>& visuals);
		void SetOverlayVisible(bool visible, bool confirm);
		void SetOverlayAlpha(float alpha) noexcept;
		bool OverlayVisible() const noexcept;

	private:
		static constexpr float StripOuterRadius = 180.0f;
		static constexpr float StripHalfHeight = 138.0f;
		static constexpr float MinVisualHalfHeight = 8.0f;
		static constexpr float MinVisualRadius = 24.0f;
		static constexpr unsigned int MaxVisibleDivisions = 256u;
		static void AppendPartUvs(std::vector<float>* uvs, float partKind);
		static void AppendQuad(std::vector<float>& verts,
			std::vector<float>* uvs,
			float partKind,
			const glm::vec3& a,
			const glm::vec3& b,
			const glm::vec3& c,
			const glm::vec3& d);
		static void BuildDivisionMesh(std::vector<float>& verts, std::vector<float>& uvs);
		bool _overlayVisible;
		float _overlayAlpha;
		Time _confirmedAt;
	};
}
