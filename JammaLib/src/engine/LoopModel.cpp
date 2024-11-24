#include "LoopModel.h"

using namespace engine;
using base::DrawContext;
using base::Drawable;
using base::DrawableParams;
using graphics::GlDrawContext;
using audio::BufferBank;
using gui::GuiModel;
using utils::Size2d;

const Size2d LoopModel::_LedGap = { 6, 6 };
const float LoopModel::_MinHeight = 1.0f;
const float LoopModel::_RadialThicknessFrac = 1.0f / 20.0f;
const float LoopModel::_HeightScale = 100.0f;

LoopModel::LoopModel(LoopModelParams params) :
	GuiModel(params),
	_loopIndexFrac(0),
	_modelState(STATE_RECORDING)
{
}

LoopModel::~LoopModel()
{
}

void LoopModel::Draw3d(DrawContext& ctx,
	unsigned int numInstances)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	glCtx.PushMvp(glm::rotate(glm::mat4(1.0), (float)(constants::TWOPI * (_loopIndexFrac + 0.0)), glm::vec3(0.0f, 1.0f, 0.0f)));

	glCtx.SetUniform("LoopState", (unsigned int)_modelState);

	GuiModel::Draw3d(glCtx, 1);

	glCtx.PopMvp();
}

double LoopModel::LoopIndexFrac() const
{
	return _loopIndexFrac;
}

void LoopModel::SetLoopIndexFrac(double frac)
{
	_loopIndexFrac = frac;
}

void LoopModel::SetLoopState(LoopModelState state)
{
	_modelState = state;
}

unsigned int LoopModel::TotalNumLeds(unsigned int vuHeight,
	unsigned int ledHeight)
{
	if ((vuHeight % ledHeight) == 0)
		return vuHeight / ledHeight;

	return (vuHeight / ledHeight) + 1;
}

unsigned int LoopModel::CurrentNumLeds(unsigned int vuHeight,
	unsigned int ledHeight,
	double value)
{
	auto numLeds = TotalNumLeds(vuHeight, ledHeight);

	return (unsigned int)std::ceil(value * numLeds);
}

void LoopModel::UpdateModel(const BufferBank& buffer,
	unsigned long loopLength,
	unsigned long offset,
	float radius)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	auto lastYMin = -_MinHeight;
	auto lastYMax = _MinHeight;
	auto numGrains = (unsigned int)ceil((double)loopLength / (double)constants::GrainSamps);

	for (auto grain = 1u; grain < numGrains; grain++)
	{
		auto [grainVerts, grainUvs, nextYMin, nextYMax] =
			CalcGrainGeometry(buffer,
				grain,
				numGrains,
				offset,
				lastYMin,
				lastYMax,
				radius);

		for (auto vert : grainVerts)
			verts.push_back(vert);

		for (auto uv : grainUvs)
			uvs.push_back(uv);

		lastYMin = nextYMin;
		lastYMax = nextYMax;
	}

	auto [grainVerts, grainUvs, nextYMin, nextYMax] =
		CalcGrainGeometry(buffer,
			numGrains,
			numGrains,
			offset,
			lastYMin,
			lastYMax,
			radius);

	for (auto vert : grainVerts)
		verts.push_back(vert);

	for (auto uv : grainUvs)
		uvs.push_back(uv);

	SetGeometry(verts, uvs);
}

std::tuple<std::vector<float>, std::vector<float>, float, float>
LoopModel::CalcGrainGeometry(const BufferBank& buffer,
	unsigned int grain,
	unsigned int numGrains,
	unsigned long offset,
	float lastYMin,
	float lastYMax,
	float radius)
{
	const float radialThickness = radius * _RadialThicknessFrac;

	auto yToUv = [](float y) {
		auto scale = 1.0f / ((_MinHeight * 2) + (_HeightScale * 2));
		auto offset = 0.5f;
		return scale * y + offset;
	};

	auto angle1 = ((float)constants::TWOPI) * ((float)(grain - 1) / (float)numGrains);
	auto angle2 = ((float)constants::TWOPI) * ((float)grain / (float)numGrains);
	auto i1 = offset + (grain - 1) * (unsigned long)constants::GrainSamps;
	auto i2 = offset + grain * (unsigned long)constants::GrainSamps;
	auto gMin = buffer.SubMin(i1, i2);
	auto gMax = buffer.SubMax(i1, i2);

	auto xInner1 = sin(angle1) * (radius - radialThickness);
	auto xInner2 = sin(angle2) * (radius - radialThickness);
	auto xOuter1 = sin(angle1) * (radius + radialThickness);
	auto xOuter2 = sin(angle2) * (radius + radialThickness);
	auto yMin = (_HeightScale * gMin) - _MinHeight;
	auto yMax = (_HeightScale * gMax) + _MinHeight;
	auto zInner1 = cos(angle1) * (radius - radialThickness);
	auto zInner2 = cos(angle2) * (radius - radialThickness);
	auto zOuter1 = cos(angle1) * (radius + radialThickness);
	auto zOuter2 = cos(angle2) * (radius + radialThickness);

	std::vector<float> verts;
	std::vector<float> uvs;

	// Front
	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));

	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	// Top
	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));

	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	// Back
	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	// Bottom
	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	return std::make_tuple(verts, uvs, yMin, yMax);
}
