#include "QuantisationModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include "glm/ext.hpp"
#include "GlDrawContext.h"
#include "../../include/Constants.h"

using namespace engine;

namespace
{
	constexpr float GateInnerRadius = 0.0f;
	constexpr float GateOuterRadius = 180.0f;
	constexpr float GateHalfHeight = 138.0f;
	constexpr unsigned int MaxVisibleGates = 128u;
	constexpr float FrameWidthFraction = 0.008f;
	constexpr float FrameDepthFraction = 0.15f;

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

	glm::vec3 GatePoint(float x, float y, float z)
	{
		return glm::vec3(x, y, z);
	}
}

QuantisationModel::QuantisationModel() :
	GuiModel(gui::GuiModelParams()),
	_seedSamps(0u),
	_masterLoopSamps(0u),
	_gateCount(0u),
	_loopIndexFrac(0.0),
	_overlayVisible(false),
	_confirmedAt(Timer::GetZero())
{
	_modelParams.ModelShaders = { "quantisation" };
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

	auto highlight = 0.25f;  // lower resting alpha
    if (!Timer::IsZero(_confirmedAt))
    {
        const auto elapsed = Timer::GetElapsedSeconds(_confirmedAt, Timer::GetTime());
        if (elapsed < 1.0)
            highlight = 1.0f - static_cast<float>(elapsed * 0.75);  // 1.0 -> 0.25 over 1.0s
        else
            _confirmedAt = Timer::GetZero();
    }

	const auto phaseOffset = std::fmod(
		static_cast<float>(constants::TWOPI * _loopIndexFrac),
		static_cast<float>(constants::TWOPI));

	const auto angleStep = (_gateCount > 1u) ?
		static_cast<float>(constants::TWOPI) / static_cast<float>(_gateCount) :
		0.0f;

	glCtx.SetUniform("AngleStep", angleStep);
	glCtx.SetUniform("PhaseOffset", phaseOffset);
	glCtx.SetUniform("Highlight", highlight);
	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);
	if (_gateCount > 1u)
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, _gateCount);
	else
		glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);

	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void QuantisationModel::SetTiming(unsigned int seedSamps, unsigned int masterLoopSamps, float sampleRate)
{
	(void)sampleRate;

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

void QuantisationModel::SetLoopIndexFrac(double loopIndexFrac) noexcept
{
	if (loopIndexFrac < 0.0)
		_loopIndexFrac = 0.0;
	else if (loopIndexFrac > 1.0)
		_loopIndexFrac = 1.0;
	else
		_loopIndexFrac = loopIndexFrac;
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

	const auto radialSpan = std::max(outerRadius - innerRadius, 1.0f);
	const auto frameWidth = std::clamp(radialSpan * FrameWidthFraction, 8.0f, halfHeight * 0.8f);
	const auto frameDepthHalf = std::max(frameWidth * FrameDepthFraction, 4.0f) * 0.5f;

	const auto yMin = -halfHeight;
	const auto yInnerMin = yMin + frameWidth;
	const auto yInnerMax = halfHeight - frameWidth;
	const auto yMax = halfHeight;
	const auto zMin = innerRadius;
	const auto zInnerMax = outerRadius - frameWidth;
	const auto zMax = outerRadius;
	const auto xFront = frameDepthHalf;
	const auto xBack = -frameDepthHalf;

	verts.reserve(15u * 6u * 3u);

	// Front faces for the top, right, and bottom beams of the half-frame.
	AppendQuad(verts,
		GatePoint(xFront, yInnerMax, zMin),
		GatePoint(xFront, yMax, zMin),
		GatePoint(xFront, yMax, zMax),
		GatePoint(xFront, yInnerMax, zMax));
	AppendQuad(verts,
		GatePoint(xFront, yInnerMin, zInnerMax),
		GatePoint(xFront, yInnerMax, zInnerMax),
		GatePoint(xFront, yInnerMax, zMax),
		GatePoint(xFront, yInnerMin, zMax));
	AppendQuad(verts,
		GatePoint(xFront, yMin, zMin),
		GatePoint(xFront, yInnerMin, zMin),
		GatePoint(xFront, yInnerMin, zMax),
		GatePoint(xFront, yMin, zMax));

	// Matching back faces.
	AppendQuad(verts,
		GatePoint(xBack, yInnerMax, zMax),
		GatePoint(xBack, yMax, zMax),
		GatePoint(xBack, yMax, zMin),
		GatePoint(xBack, yInnerMax, zMin));
	AppendQuad(verts,
		GatePoint(xBack, yInnerMin, zMax),
		GatePoint(xBack, yInnerMax, zMax),
		GatePoint(xBack, yInnerMax, zInnerMax),
		GatePoint(xBack, yInnerMin, zInnerMax));
	AppendQuad(verts,
		GatePoint(xBack, yMin, zMax),
		GatePoint(xBack, yInnerMin, zMax),
		GatePoint(xBack, yInnerMin, zMin),
		GatePoint(xBack, yMin, zMin));

	const std::array<std::pair<glm::vec2, glm::vec2>, 8u> boundary = {{
		{ { yMin, zMin }, { yMin, zMax } },
		{ { yMin, zMax }, { yMax, zMax } },
		{ { yMax, zMax }, { yMax, zMin } },
		{ { yMax, zMin }, { yInnerMax, zMin } },
		{ { yInnerMax, zMin }, { yInnerMax, zInnerMax } },
		{ { yInnerMax, zInnerMax }, { yInnerMin, zInnerMax } },
		{ { yInnerMin, zInnerMax }, { yInnerMin, zMin } },
		{ { yInnerMin, zMin }, { yMin, zMin } }
	}};

	for (const auto& [from, to] : boundary)
	{
		AppendQuad(verts,
			GatePoint(xFront, from.x, from.y),
			GatePoint(xFront, to.x, to.y),
			GatePoint(xBack, to.x, to.y),
			GatePoint(xBack, from.x, from.y));
	}

	return verts;
}
