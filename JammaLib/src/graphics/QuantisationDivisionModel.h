#pragma once

#include <vector>
#include "../engine/Quantisation.h"
#include "../gui/GuiModel.h"
#include "../engine/Timer.h"

namespace engine
{
	class QuantisationDivisionModel :
		public gui::GuiModel
	{
	public:
		QuantisationDivisionModel();

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetLoopTakeVisuals(const std::vector<QuantisationLoopTakeVisual>& visuals);
		void SetOverlayVisible(bool visible, bool confirm);
		void SetOverlayAlpha(float alpha) noexcept;
		bool OverlayVisible() const noexcept;

	private:
		bool _overlayVisible;
		float _overlayAlpha;
		Time _confirmedAt;
	};
}
