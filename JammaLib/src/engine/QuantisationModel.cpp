#include "QuantisationModel.h"

#include <algorithm>
#include <cmath>
#include "glm/ext.hpp"
#include "../graphics/GlDrawContext.h"

using namespace engine;

namespace
{
	constexpr float GateInnerRadius = 112.0f;
	constexpr float GateOuterRadius = 154.0f;
	constexpr float GateHalfHeight = 82.0f;
	constexpr unsigned int MaxVisibleGates = 128u;

	std::vector<float> BuildDummyUvs(size_t vertexCoordCount)
	{
		return std::vector<float>((vertexCoordCount / 3u) * 2u, 0.0f);
	}
}

QuantisationModel::QuantisationModel() :
	GuiModel(gui::GuiModelParams()),
	_seedSamps(0u),
	_masterLoopSamps(0u),
	_gateCount(0u),
	_overlayVisible(false),
	_confirmedAt(Timer::GetZero())
{
	_modelParams.ModelShaders = { "white" };
	SetTiming(1u, 1u);
	SetVisible(false);
}

void QuantisationModel::Draw3d(base::DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (!_overlayVisible || (base::PASS_SCENE != pass))
		return;

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	auto pos = ModelPosition();
	auto scale = ModelScale();

	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	auto shaderWeak = GetShader();
	auto shader = shaderWeak.lock();
	if (!shader || (0u == _vertexArray) || (0u == _numTris))
	{
		glCtx.PopMvp();
		glCtx.PopMvp();
		return;
	}

	auto highlight = 0.20f;
	if (!Timer::IsZero(_confirmedAt))
	{
		const auto elapsed = Timer::GetElapsedSeconds(_confirmedAt, Timer::GetTime());
		if (elapsed < 0.75)
			highlight = 0.55f - static_cast<float>(elapsed * 0.30);
		else
			_confirmedAt = Timer::GetZero();
	}

	glCtx.SetUniform("Highlight", highlight);
	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);
	if (numInstances > 1)
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, numInstances);
	else
		glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);

	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void QuantisationModel::SetTiming(unsigned int seedSamps, unsigned int masterLoopSamps)
{
	if (seedSamps == 0u)
		seedSamps = 1u;
	if (masterLoopSamps == 0u)
		masterLoopSamps = seedSamps;

	auto gates = std::max(1u, masterLoopSamps / seedSamps);
	gates = std::min(gates, MaxVisibleGates);

	if ((seedSamps == _seedSamps) && (masterLoopSamps == _masterLoopSamps) && (gates == _gateCount))
		return;

	_seedSamps = seedSamps;
	_masterLoopSamps = masterLoopSamps;
	_gateCount = gates;

	auto verts = BuildGateGeometry(gates, GateInnerRadius, GateOuterRadius, GateHalfHeight);
	auto uvs = BuildDummyUvs(verts.size());
	SetGeometry(std::move(verts), std::move(uvs));
}

void QuantisationModel::SetOverlayVisible(bool visible, bool confirm)
{
	_overlayVisible = visible;
	SetVisible(visible);
	if (confirm)
		_confirmedAt = Timer::GetTime();
}

bool QuantisationModel::OverlayVisible() const noexcept
{
	return _overlayVisible;
}

std::vector<float> QuantisationModel::BuildGateGeometry(unsigned int gateCount,
	float innerRadius,
	float outerRadius,
	float halfHeight)
{
	std::vector<float> verts;
	if (gateCount == 0u)
		return verts;

	verts.reserve(static_cast<size_t>(gateCount) * 18u);
	for (auto gate = 0u; gate < gateCount; ++gate)
	{
		const auto angle = static_cast<float>(constants::TWOPI) * (static_cast<float>(gate) / static_cast<float>(gateCount));
		const auto xInner = std::sin(angle) * innerRadius;
		const auto zInner = std::cos(angle) * innerRadius;
		const auto xOuter = std::sin(angle) * outerRadius;
		const auto zOuter = std::cos(angle) * outerRadius;

		verts.push_back(xInner); verts.push_back(-halfHeight); verts.push_back(zInner);
		verts.push_back(xOuter); verts.push_back(halfHeight); verts.push_back(zOuter);
		verts.push_back(xInner); verts.push_back(halfHeight); verts.push_back(zInner);

		verts.push_back(xInner); verts.push_back(-halfHeight); verts.push_back(zInner);
		verts.push_back(xOuter); verts.push_back(-halfHeight); verts.push_back(zOuter);
		verts.push_back(xOuter); verts.push_back(halfHeight); verts.push_back(zOuter);
	}

	return verts;
}