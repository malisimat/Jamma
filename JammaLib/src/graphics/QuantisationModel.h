#pragma once

#include <vector>
#include "../engine/Quantisation.h"
#include "../gui/GuiModel.h"
#include "../engine/Timer.h"

namespace engine
{
	class QuantisationModel :
		public gui::GuiModel
	{
	public:
		QuantisationModel();

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetTiming(unsigned int seedSamps, unsigned int masterLoopSamps, float sampleRate = 0.0f);
		void SetLoopTakeVisuals(unsigned int seedSamps, const std::vector<QuantisationLoopTakeVisual>& visuals);
		void SetLoopIndexFrac(double loopIndexFrac) noexcept;
		void SetOverlayVisible(bool visible, bool confirm);
		void SetOverlayAlpha(float alpha) noexcept;
		bool OverlayVisible() const noexcept;

		static std::vector<float> BuildGateGeometry(unsigned int gateCount,
			float innerRadius,
			float outerRadius,
			float halfHeight);

	private:
		unsigned int _seedSamps;
		bool _overlayVisible;
		float _overlayAlpha;
		Time _confirmedAt;
	};
}
