#pragma once

#include <vector>
#include <memory>
#include "../include/Constants.h"
#include "../utils/ArrayUtils.h"
#include "../utils/VecUtils.h"
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
		enum LoopModelState
		{
			STATE_RECORDING = 0,
			STATE_PLAYING = 1,
			STATE_MUTED = 2,
			STATE_PICKING = 3
		};

	public:
		LoopModel(LoopModelParams params);
		~LoopModel();

		// Copy
		LoopModel(const LoopModel&) = delete;
		LoopModel& operator=(const LoopModel&) = delete;

	public:
		void Draw3d(base::DrawContext& ctx, unsigned int numInstances) override;
		double LoopIndexFrac() const;
		void SetLoopIndexFrac(double frac);
		void SetLoopState(LoopModelState state);
		void UpdateModel(const audio::BufferBank& buffer,
			unsigned long loopLength,
			unsigned long offset,
			float radius);

	protected:
		static unsigned int TotalNumLeds(unsigned int vuHeight, unsigned int ledHeight);
		static unsigned int CurrentNumLeds(unsigned int vuHeight, unsigned int ledHeight, double value);

		std::weak_ptr<resources::ShaderResource> GetShader() override;

		std::tuple<std::vector<float>, std::vector<float>, float, float>
			CalcGrainGeometry(const audio::BufferBank& buffer,
				unsigned int grain,
				unsigned int numGrains,
				unsigned long offset,
				float lastYMin,
				float lastYMax,
				float radius);

	protected:
		static const utils::Size2d _LedGap;
		static const float _MinHeight;
		static const float _RadialThicknessFrac;
		static const float _HeightScale;

		double _loopIndexFrac;
		LoopModelState _modelState;
	};
}
