#pragma once

#include <vector>
#include <glm/vec3.hpp>
#include "../engine/Quantiser.h"
#include "../gui/GuiModel.h"
#include "Timer.h"

namespace engine
{
	class QuantisationModel :
		public gui::GuiModel
	{
	public:
		struct VisualCounts
		{
			unsigned int GrainFrameCount = 0u;
			unsigned int FractionDivisionCount = 0u;
			unsigned int StepSamps = 0u;
		};

		QuantisationModel();

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetTiming(unsigned int seedSamps);
		void SetLoopTakeVisuals(unsigned int seedSamps, const std::vector<engine::QuantisationLoopTakeVisual>& visuals);
		void SetOverlayVisible(bool visible, bool confirm);
		void SetOverlayAlpha(float alpha) noexcept;
		bool OverlayVisible() const noexcept;

		static std::vector<float> BuildGateGeometry(unsigned int gateCount,
			float innerRadius,
			float outerRadius,
			float halfHeight);
		static VisualCounts ResolveVisualCounts(const engine::QuantisationLoopTakeVisual& visual) noexcept;

	private:
		static constexpr float GateInnerRadius = 0.0f;
		static constexpr float GateOuterRadius = 180.0f;
		static constexpr float GateHalfHeight = 138.0f;
		static constexpr unsigned int MaxVisibleGates = 128u;
		static constexpr float FrameWidthFraction = 0.008f;
		static constexpr float FrameDepthFraction = 0.15f;
		static constexpr float MinVisualHalfHeight = 8.0f;
		static constexpr float MinVisualRadius = 24.0f;
		static constexpr float FramePart = 0.0f;
		static constexpr float BackingPart = 1.0f;
		static void AppendPartUvs(std::vector<float>* uvs, float partKind);
		static void AppendQuad(std::vector<float>& verts,
			std::vector<float>* uvs,
			float partKind,
			const glm::vec3& a,
			const glm::vec3& b,
			const glm::vec3& c,
			const glm::vec3& d);
		static glm::vec3 GatePoint(float x, float y, float z);
		static void BuildGateMesh(std::vector<float>& verts,
			std::vector<float>* uvs,
			float innerRadius,
			float outerRadius,
			float halfHeight);
		unsigned int _seedSamps;
		bool _overlayVisible;
		float _overlayAlpha;
		Time _confirmedAt;
	};
}
