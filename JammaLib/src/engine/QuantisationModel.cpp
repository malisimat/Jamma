#include "QuantisationModel.h"

#include <algorithm>
#include <cmath>
#include "glm/ext.hpp"
#include "../graphics/GlDrawContext.h"

using namespace engine;

namespace
{
	constexpr float GateInnerRadius = 132.0f;
	constexpr float GateOuterRadius = 312.0f;
	constexpr float GateHalfHeight = 92.0f;
	constexpr unsigned int MaxVisibleGates = 128u;
	constexpr float GateWidthFraction = 0.18f;

	std::vector<float> BuildDummyUvs(size_t vertexCoordCount)
	{
		return std::vector<float>((vertexCoordCount / 3u) * 2u, 0.0f);
	}

	void AppendQuad(std::vector<float>& verts,
		const glm::vec3& a,
		const glm::vec3& b,
		const glm::vec3& c,
		const glm::vec3& d)
	{
		verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
		verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
		verts.push_back(c.x); verts.push_back(c.y); verts.push_back(c.z);

		verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
		verts.push_back(c.x); verts.push_back(c.y); verts.push_back(c.z);
		verts.push_back(d.x); verts.push_back(d.y); verts.push_back(d.z);
	}

	glm::vec3 GatePoint(float radius, float angle, float y)
	{
		return glm::vec3(std::sin(angle) * radius, y, std::cos(angle) * radius);
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

	verts.reserve(static_cast<size_t>(gateCount) * 108u);
	const auto step = static_cast<float>(constants::TWOPI) / static_cast<float>(gateCount);
	const auto halfSpan = step * GateWidthFraction;
	for (auto gate = 0u; gate < gateCount; ++gate)
	{
		const auto angle = step * static_cast<float>(gate);
		const auto left = angle - halfSpan;
		const auto right = angle + halfSpan;

		const auto innerTopLeft = GatePoint(innerRadius, left, halfHeight);
		const auto innerTopRight = GatePoint(innerRadius, right, halfHeight);
		const auto outerTopLeft = GatePoint(outerRadius, left, halfHeight);
		const auto outerTopRight = GatePoint(outerRadius, right, halfHeight);
		const auto innerBottomLeft = GatePoint(innerRadius, left, -halfHeight);
		const auto innerBottomRight = GatePoint(innerRadius, right, -halfHeight);
		const auto outerBottomLeft = GatePoint(outerRadius, left, -halfHeight);
		const auto outerBottomRight = GatePoint(outerRadius, right, -halfHeight);

		AppendQuad(verts, innerTopLeft, outerTopLeft, outerTopRight, innerTopRight);
		AppendQuad(verts, innerBottomRight, outerBottomRight, outerBottomLeft, innerBottomLeft);
		AppendQuad(verts, innerBottomLeft, innerBottomRight, innerTopRight, innerTopLeft);
		AppendQuad(verts, outerBottomRight, outerBottomLeft, outerTopLeft, outerTopRight);
		AppendQuad(verts, innerBottomLeft, outerBottomLeft, outerTopLeft, innerTopLeft);
		AppendQuad(verts, innerBottomRight, innerTopRight, outerTopRight, outerBottomRight);
	}

	return verts;
}