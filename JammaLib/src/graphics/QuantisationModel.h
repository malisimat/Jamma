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
		void SetTiming(unsigned int seedSamps);
		void SetLoopTakeVisuals(unsigned int seedSamps, const std::vector<QuantisationLoopTakeVisual>& visuals);
		void SetOverlayVisible(bool visible, bool confirm);
		void SetOverlayAlpha(float alpha) noexcept;
		void SetHue(float hue) noexcept;
		bool OverlayVisible() const noexcept;

		static std::vector<float> BuildGateGeometry(unsigned int gateCount,
			float innerRadius,
			float outerRadius,
			float halfHeight);

	private:
		unsigned int _seedSamps;
		bool _overlayVisible;
		float _overlayAlpha;
		float _hue;
		Time _confirmedAt;
	};
}
