#pragma once

#include <vector>
#include <memory>
#include "../include/Constants.h"
#include "../utils/ArrayUtils.h"
#include "../audio/BufferBank.h"
#include "../gui/GuiModel.h"

namespace engine
{
	class LoopModelParams :
		public gui::GuiModelParams
	{
	public:
		LoopModelParams() :
			gui::GuiModelParams(),
			LedHeight(10.0)
		{
		}

		LoopModelParams(gui::GuiModelParams params) :
			gui::GuiModelParams(params),
			LedHeight(10.0)
		{
		}

	public:
		double LedHeight;
	};

	class LoopModel :
		public virtual gui::GuiModel
	{
	public:
		LoopModel(LoopModelParams params);
		~LoopModel();

		// Copy
		LoopModel(const LoopModel&) = delete;
		LoopModel& operator=(const LoopModel&) = delete;

	public:
		void Draw3d(base::DrawContext& ctx) override;
		double LoopIndexFrac() const;
		void SetLoopIndexFrac(double frac);
		void UpdateModel(const audio::BufferBank& buffer,
			unsigned long loopLength,
			float radius);

	protected:
		static unsigned int TotalNumLeds(unsigned int vuHeight, unsigned int ledHeight);
		static unsigned int CurrentNumLeds(unsigned int vuHeight, unsigned int ledHeight, double value);

		std::tuple<std::vector<float>, std::vector<float>, float, float>
			CalcGrainGeometry(const audio::BufferBank& buffer,
				unsigned int grain,
				unsigned int numGrains,
				float lastYMin,
				float lastYMax,
				float radius);

	protected:
		static const utils::Size2d _LedGap;

		double _loopIndexFrac;
	};
}
