#pragma once

#include <vector>
#include "../gui/GuiModel.h"
#include "Timer.h"

namespace engine
{
	class QuantisationModel :
		public gui::GuiModel
	{
	public:
		QuantisationModel();

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetTiming(unsigned int seedSamps, unsigned int masterLoopSamps, float sampleRate = 0.0f);
		void SetOverlayVisible(bool visible, bool confirm);
		bool OverlayVisible() const noexcept;

		static std::vector<float> BuildGateGeometry(unsigned int gateCount,
			float innerRadius,
			float outerRadius,
			float halfHeight);

	private:
		unsigned int _seedSamps;
		unsigned int _masterLoopSamps;
		unsigned int _gateCount;
		float _masterLoopSecs;
		Time _rotationStartTime;
		bool _overlayVisible;
		Time _confirmedAt;
	};
}