#include "VU.h"

using namespace engine;
using base::DrawContext;
using base::Drawable;
using base::DrawableParams;
using graphics::GlDrawContext;
using gui::GuiModel;
using utils::Size2d;

const float VU::_LedDy = 1.5f;
const double VU::_MaxValue = 1.0;

VU::VU(VuParams params) :
	GuiModel(params),
	_value(audio::FallingValue({
		params.FallRate,
		params.HoldFallRate,
		params.HoldSamps })),
	_vuParams(params)
{
}

VU::~VU()
{
}

void VU::Draw3d(DrawContext& ctx,
	unsigned int numInstances)
{
	auto val = _value.Current();
	auto hold = _value.HoldValue();
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto totalNumLeds = TotalNumLeds(_sizeParams.Size.Height,
		_vuParams.LedHeight);
	auto numLeds = CurrentNumLeds(val, totalNumLeds);
	auto holdLed = CurrentNumLeds(hold, totalNumLeds) - 1;

	glCtx.SetUniform("DX", 0.0f);
	glCtx.SetUniform("DY", _LedDy);
	glCtx.SetUniform("NumInstances", totalNumLeds);
	glCtx.SetUniform("InstanceOffset", 0u);

	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(1.0f, 4.0f, 1.0f)));

	GuiModel::Draw3d(glCtx, numLeds);

	glCtx.SetUniform("InstanceOffset", holdLed);

	GuiModel::Draw3d(glCtx, 1);

	glCtx.PopMvp();
}

double VU::Value() const
{
	return _value.Current();
}

void VU::SetValue(double value, unsigned int numUpdates)
{
	_value.SetTarget(value);

	for (auto i = 0u; i < numUpdates; i++)
		_value.Next();
}

double VU::FallRate() const
{
	return _vuParams.FallRate;
}

double VU::HoldValue() const
{
	return _value.HoldValue();
}

double VU::HoldFallRate() const
{
	return _vuParams.HoldFallRate;
}

void VU::UpdateModel(float radius)
{
	auto [ledVerts, ledUvs] =
		CalcLedGeometry(radius,
			_sizeParams.Size.Height,
			_vuParams.LedHeight);

	SetGeometry(ledVerts, ledUvs);
}

unsigned int VU::TotalNumLeds(unsigned int vuHeight,
	double ledHeight)
{
	return (unsigned int)std::ceil(((double)vuHeight) / ledHeight);
}

unsigned int VU::CurrentNumLeds(double value, unsigned int totalNumLeds)
{
	auto frac = value / _MaxValue;
	if (frac < 0)
		frac = 0.0;
	else if (frac > 1)
		frac = 1.0;

	return (unsigned int)std::ceil(frac * (double)totalNumLeds);
}

std::tuple<std::vector<float>, std::vector<float>>
VU::CalcLedGeometry(float radius,
	unsigned int height,
	float ledHeight)
{
	const float radialThickness = radius / 15.0f;

	auto yToUv = [&height](float y) {
		if (height > 0)
			return y / (float)height;

		return 0.0f;
	};

	auto angle1 = -0.01f;
	auto angle2 = -angle1;
	auto xInner1 = sin(angle1) * (radius - radialThickness);
	auto xInner2 = sin(angle2) * (radius - radialThickness);
	auto xOuter1 = sin(angle1) * (radius + radialThickness);
	auto xOuter2 = sin(angle2) * (radius + radialThickness);
	auto yMin = (ledHeight * 0.5f);
	auto yMax = -(ledHeight * 0.5f);
	auto yMid = (yMin + yMax) * 0.5f;
	auto zInner1 = cos(angle1) * (radius - radialThickness);
	auto zInner2 = cos(angle2) * (radius - radialThickness);
	auto zOuter1 = cos(angle1) * (radius + radialThickness);
	auto zOuter2 = cos(angle2) * (radius + radialThickness);

	std::vector<float> verts;
	std::vector<float> uvs;

	// Front
	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zOuter1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	// Top
	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner1); verts.push_back(yMax); verts.push_back(zInner1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	// Back
	verts.push_back(xInner1); verts.push_back(yMin); verts.push_back(zInner1);
	verts.push_back(xInner1); verts.push_back(yMax); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xInner1); verts.push_back(yMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	// Bottom
	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zOuter1);
	verts.push_back(xInner1); verts.push_back(yMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	// Left Side
	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zInner1);
	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zOuter1);
	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zInner1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zInner1);
	verts.push_back(xOuter1); verts.push_back(yMin); verts.push_back(zOuter1);
	verts.push_back(xOuter1); verts.push_back(yMax); verts.push_back(zOuter1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	// Right Side
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMid));

	return std::make_tuple(verts, uvs);
}
