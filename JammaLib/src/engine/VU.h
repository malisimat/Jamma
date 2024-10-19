#pragma once

#include <vector>
#include <memory>
#include "../audio/FallingValue.h"
#include "../gui/GuiModel.h"

namespace engine
{
	class VuParams :
		public gui::GuiModelParams
	{
	public:
		VuParams() :
			gui::GuiModelParams(),
			LedHeight(10.0f),
			FallRate(0.00004),
			HoldFallRate(0.00002),
			HoldSamps(12000u)
		{
		}

		VuParams(gui::GuiModelParams params) :
			gui::GuiModelParams(params),
			LedHeight(10.0f),
			FallRate(0.00004),
			HoldFallRate(0.00002),
			HoldSamps(12000u)
		{
		}

	public:
		float LedHeight;
		double FallRate;
		double HoldFallRate;
		unsigned int HoldSamps;
	};

	class VU :
		public virtual gui::GuiModel
	{
	public:
		VU(VuParams params);
		~VU();

		// Copy
		VU(const VU&) = delete;
		VU& operator=(const VU&) = delete;

	public:
		void Draw3d(base::DrawContext& ctx, unsigned int numInstances) override;

		double Value() const;
		void SetValue(double value, unsigned int numUpdates);
		double FallRate() const;
		double HoldValue() const;
		double HoldFallRate() const;
		void UpdateModel(float radius);

	protected:
		static unsigned int TotalNumLeds(unsigned int vuHeight, double ledHeight);
		static unsigned int CurrentNumLeds(double value, unsigned int maxLeds);
		static std::tuple<std::vector<float>, std::vector<float>>
			CalcLedGeometry(float radius,
				unsigned int height,
				float ledHeight);

	protected:
		static const float _LedDy;
		static const double _MaxValue;

		audio::FallingValue _value;
		VuParams _vuParams;
	};
}
