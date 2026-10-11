#include "QuantisationModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "glm/ext.hpp"
#include "GlDrawContext.h"
#include "../midi/MidiQuantisation.h"
#include "../../include/Constants.h"

using namespace engine;
using namespace utils;

void QuantisationModel::AppendPartUvs(std::vector<float>* uvs, float partKind)
{
	if (!uvs)
		return;

	for (auto i = 0u; i < 6u; ++i)
	{
		uvs->push_back(partKind);
		uvs->push_back(0.0f);
	}
}

void QuantisationModel::AppendQuad(std::vector<float>& verts,
	std::vector<float>* uvs,
	float partKind,
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

	AppendPartUvs(uvs, partKind);
}

glm::vec3 QuantisationModel::GatePoint(float x, float y, float z)
{
	return glm::vec3(x, y, z);
}

void QuantisationModel::BuildGateMesh(std::vector<float>& verts,
	std::vector<float>* uvs,
	float innerRadius,
	float outerRadius,
	float halfHeight)
{
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

	verts.reserve(16u * 6u * 3u);
	if (uvs)
		uvs->reserve(16u * 6u * 2u);

	AppendQuad(verts, uvs, BackingPart,
		GatePoint(xFront * 0.35f, yInnerMin, zMin),
		GatePoint(xFront * 0.35f, yInnerMax, zMin),
		GatePoint(xFront * 0.35f, yInnerMax, zInnerMax),
		GatePoint(xFront * 0.35f, yInnerMin, zInnerMax));
	AppendQuad(verts, uvs, BackingPart,
		GatePoint(xBack * 0.35f, yInnerMax, zMin),
		GatePoint(xBack * 0.35f, yInnerMin, zMin),
		GatePoint(xBack * 0.35f, yInnerMin, zInnerMax),
		GatePoint(xBack * 0.35f, yInnerMax, zInnerMax));

	// Front faces for the top, right, and bottom beams of the half-frame.
	AppendQuad(verts, uvs, FramePart,
		GatePoint(xFront, yInnerMax, zMin),
		GatePoint(xFront, yMax, zMin),
		GatePoint(xFront, yMax, zMax),
		GatePoint(xFront, yInnerMax, zMax));
	AppendQuad(verts, uvs, FramePart,
		GatePoint(xFront, yInnerMin, zInnerMax),
		GatePoint(xFront, yInnerMax, zInnerMax),
		GatePoint(xFront, yInnerMax, zMax),
		GatePoint(xFront, yInnerMin, zMax));
	AppendQuad(verts, uvs, FramePart,
		GatePoint(xFront, yMin, zMin),
		GatePoint(xFront, yInnerMin, zMin),
		GatePoint(xFront, yInnerMin, zMax),
		GatePoint(xFront, yMin, zMax));

	// Matching back faces.
	AppendQuad(verts, uvs, FramePart,
		GatePoint(xBack, yInnerMax, zMax),
		GatePoint(xBack, yMax, zMax),
		GatePoint(xBack, yMax, zMin),
		GatePoint(xBack, yInnerMax, zMin));
	AppendQuad(verts, uvs, FramePart,
		GatePoint(xBack, yInnerMin, zMax),
		GatePoint(xBack, yInnerMax, zMax),
		GatePoint(xBack, yInnerMax, zInnerMax),
		GatePoint(xBack, yInnerMin, zInnerMax));
	AppendQuad(verts, uvs, FramePart,
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
		AppendQuad(verts, uvs, FramePart,
			GatePoint(xFront, from.x, from.y),
			GatePoint(xFront, to.x, to.y),
			GatePoint(xBack, to.x, to.y),
			GatePoint(xBack, from.x, from.y));
	}

}

QuantisationModel::QuantisationModel() :
	GuiModel(gui::GuiModelParams()),
	_seedSamps(0u),
	_overlayVisible(false),
	_overlayAlpha(0.0f),
	_confirmedAt(Timer::GetZero())
{
	_modelParams.ModelShaders = { "quantisation" };
	SetTiming(1u);
	SetVisible(false);
}

void QuantisationModel::Draw3d(base::DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	(void)numInstances;

	if (!_overlayVisible || (_overlayAlpha <= 0.001f) || (base::PASS_SCENE != pass))
		return;

	if (!SyncInstanceAttributes())
		_resourcesNeedInitialising = true;

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	auto pos = ModelPosition();
	auto scale = ModelScale();

	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	auto shaderWeak = GetShader();
	auto shader = shaderWeak.lock();
	if (!shader || (0u == _vertexArray) || (0u == _numTris) || (0u == InstanceCount()))
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

	glCtx.SetUniform("Highlight", highlight);
	glCtx.SetUniform("OverlayAlpha", _overlayAlpha);
	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);
	glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, InstanceCount());

	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void QuantisationModel::SetTiming(unsigned int seedSamps)
{
	if (seedSamps == 0u)
		seedSamps = 1u;

	if (seedSamps == _seedSamps)
		return;

	_seedSamps = seedSamps;

	auto verts = std::vector<float>();
	auto uvs = std::vector<float>();
	BuildGateMesh(verts, &uvs, GateInnerRadius, GateOuterRadius, GateHalfHeight);
	SetGeometry(std::move(verts), std::move(uvs));
}

void QuantisationModel::SetLoopTakeVisuals(unsigned int seedSamps,
	const std::vector<engine::QuantisationLoopTakeVisual>& visuals)
{
	if (seedSamps == 0u)
		seedSamps = 1u;

	SetTiming(seedSamps);

	std::vector<float> transforms;
	std::vector<float> fillHalfWidths;
	transforms.reserve(visuals.size() * 16u);
	fillHalfWidths.reserve(visuals.size() * 4u);

	for (const auto& visual : visuals)
	{
		if (visual.LoopLengthSamps == 0ul)
			continue;

		const auto counts = ResolveVisualCounts(visual);
		auto gateCount = counts.GrainFrameCount;
		if (0u == gateCount)
			continue;

		const auto totalGateCount = gateCount;
		gateCount = std::clamp(gateCount, 1u, MaxVisibleGates);

		const auto angleStep = static_cast<float>(constants::TWOPI) / static_cast<float>(gateCount);
		const auto fillHalfWidth = std::clamp(
			GateOuterRadius * std::tan(angleStep * 0.25f),
			6.0f,
			GateOuterRadius * 0.45f);
		const auto loopIndexAngle = std::fmod(
			static_cast<float>(constants::TWOPI * visual.LoopIndexFrac),
			static_cast<float>(constants::TWOPI));
		const auto phaseOffsetAngle = static_cast<float>(constants::TWOPI)
			* (static_cast<float>(visual.PhaseOffsetSamps)
				/ static_cast<float>(visual.LoopLengthSamps));
		const auto phaseOffset = loopIndexAngle + phaseOffsetAngle;
		const auto heightScale = std::max(visual.HalfHeight, MinVisualHalfHeight) / GateHalfHeight;
		const auto radiusScale = std::max(visual.Radius, MinVisualRadius) / GateOuterRadius;

		for (auto gate = 0u; gate < gateCount; ++gate)
		{
			const auto boundaryIndex = static_cast<std::uint32_t>(
				static_cast<std::uint64_t>(gate) * totalGateCount / gateCount);
			const auto gateAngle = visual.UseAbsoluteLocalGrid
				? loopIndexAngle + static_cast<float>(constants::TWOPI)
					* static_cast<float>(VisualBoundaryOffsetSamps(visual, boundaryIndex, 1u))
					/ static_cast<float>(visual.LoopLengthSamps)
				: phaseOffset + (angleStep * static_cast<float>(gate));
			transforms.push_back(gateAngle);
			transforms.push_back(visual.YCenter);
			transforms.push_back(heightScale);
			transforms.push_back(radiusScale);
			fillHalfWidths.push_back(fillHalfWidth);
		}

	}

	const auto instanceCount = static_cast<unsigned int>(fillHalfWidths.size());
	SetInstanceAttributes({
		{ 3u, 4u, std::move(transforms) },
		{ 4u, 1u, std::move(fillHalfWidths) }
	}, instanceCount);
}

QuantisationModel::VisualCounts QuantisationModel::ResolveVisualCounts(const engine::QuantisationLoopTakeVisual& visual) noexcept
{
	VisualCounts counts;
	if (visual.LoopLengthSamps == 0ul || visual.GrainSamps == 0u)
		return counts;

	const auto divisor = midi::MidiQuantisation::Divisor(visual.Fraction);
	counts.StepSamps = (divisor > 0u) ? (visual.GrainSamps / divisor) : 0u;
    if (!visual.GridIntervalSamps)
    {
        counts.GrainFrameCount = visual.LoopGrains ? visual.LoopGrains : static_cast<unsigned int>(visual.LoopLengthSamps / visual.GrainSamps);
        if (visual.UseAbsoluteLocalGrid)
            counts.FractionDivisionCount = counts.GrainFrameCount * divisor;
        else if (counts.StepSamps && visual.LoopLengthSamps % counts.StepSamps == 0ul)
            counts.FractionDivisionCount = static_cast<unsigned int>(visual.LoopLengthSamps / counts.StepSamps);
        return counts;
    }
	const auto interval = visual.GridIntervalSamps ? visual.GridIntervalSamps : visual.GrainSamps;
    const auto base = visual.GridBaseDivisions ? visual.GridBaseDivisions : 1u;
	// Beat frames follow the base grid. The selected fraction only changes the
	// subdivision markers, so tap-tempo subdivision changes leave beat geometry fixed.
	const auto baseCells = (static_cast<std::uint64_t>(visual.LoopLengthSamps) * base + interval - 1u) / interval;
	const auto fractionCells = (static_cast<std::uint64_t>(visual.LoopLengthSamps) * base * divisor
		+ interval - 1u) / interval;
	counts.GrainFrameCount = static_cast<unsigned int>((std::min)(baseCells, static_cast<std::uint64_t>(MaxVisibleGates)));
	counts.FractionDivisionCount = static_cast<unsigned int>((std::min)(fractionCells, static_cast<std::uint64_t>((std::numeric_limits<unsigned int>::max)())));

	return counts;
}

std::uint32_t QuantisationModel::VisualBoundaryOffsetSamps(
	const engine::QuantisationLoopTakeVisual& visual,
	std::uint32_t boundaryIndex, std::uint32_t divisionsPerGrain) noexcept
{
	if (!visual.UseAbsoluteLocalGrid || visual.GrainSamps == 0u
		|| visual.LoopLengthSamps == 0ul || divisionsPerGrain == 0u)
		return 0u;

	const auto start = static_cast<std::int64_t>(visual.TransportStartSamps);
	const auto first = midi::MidiQuantisation::NearestBoundaryIndex(
		start - visual.GridOriginSamps, visual.GridIntervalSamps ? visual.GridIntervalSamps : visual.GrainSamps,
		static_cast<std::uint64_t>(visual.GridBaseDivisions ? visual.GridBaseDivisions : 1u) * divisionsPerGrain);
	const auto boundary = midi::MidiQuantisation::BoundarySampleAt(
		first + boundaryIndex, visual.GridIntervalSamps ? visual.GridIntervalSamps : visual.GrainSamps,
		static_cast<std::uint64_t>(visual.GridBaseDivisions ? visual.GridBaseDivisions : 1u) * divisionsPerGrain);
	const auto length = static_cast<std::int64_t>(visual.LoopLengthSamps);
	auto local = (visual.GridOriginSamps + boundary - start + visual.PhaseOffsetSamps) % length;
	if (local < 0)
		local += length;
	return static_cast<std::uint32_t>(local);
}

void QuantisationModel::SetOverlayVisible(bool visible, bool confirm)
{
	_overlayVisible = visible;
	SetVisible(visible && (_overlayAlpha > 0.001f));
	if (confirm)
		_confirmedAt = Timer::GetTime();
}

void QuantisationModel::SetOverlayAlpha(float alpha) noexcept
{
	_overlayAlpha = std::clamp(alpha, 0.0f, 1.0f);
	SetVisible(_overlayVisible && (_overlayAlpha > 0.001f));
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

	BuildGateMesh(verts, nullptr, innerRadius, outerRadius, halfHeight);

	return verts;
}
