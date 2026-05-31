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
	constexpr float MinVisualHalfHeight = 8.0f;
	constexpr float MinVisualRadius = 24.0f;
	constexpr float FramePart = 0.0f;
	constexpr float BackingPart = 1.0f;
	constexpr float ColumnPart = 2.0f;
	constexpr float GateInstance = 0.0f;
	constexpr float ColumnInstance = 1.0f;

	void AppendPartUvs(std::vector<float>* uvs, float partKind)
	{
		if (!uvs)
			return;

		for (auto i = 0u; i < 6u; ++i)
		{
			uvs->push_back(partKind);
			uvs->push_back(0.0f);
		}
	}

	void AppendQuad(std::vector<float>& verts,
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

	glm::vec3 GatePoint(float x, float y, float z)
	{
		return glm::vec3(x, y, z);
	}

	void BuildGateMesh(std::vector<float>& verts,
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

		verts.reserve(24u * 6u * 3u);
		if (uvs)
			uvs->reserve(24u * 6u * 2u);

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

		const auto columnRadius = std::max(frameWidth * 0.75f, 6.0f);
		constexpr auto ColumnSides = 8u;
		for (auto side = 0u; side < ColumnSides; ++side)
		{
			const auto a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(side) / static_cast<float>(ColumnSides);
			const auto a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(side + 1u) / static_cast<float>(ColumnSides);
			const auto p0 = glm::vec2(std::cos(a0) * columnRadius, std::sin(a0) * columnRadius);
			const auto p1 = glm::vec2(std::cos(a1) * columnRadius, std::sin(a1) * columnRadius);

			AppendQuad(verts, uvs, ColumnPart,
				GatePoint(p0.x, yMin, p0.y),
				GatePoint(p1.x, yMin, p1.y),
				GatePoint(p1.x, yMax, p1.y),
				GatePoint(p0.x, yMax, p0.y));
		}
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

	if (_instanceAttributesNeedUpdating)
	{
		if (!SyncInstanceAttributes())
			_resourcesNeedInitialising = true;
	}

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
	const std::vector<QuantisationLoopTakeVisual>& visuals)
{
	if (seedSamps == 0u)
		seedSamps = 1u;

	SetTiming(seedSamps);

	std::vector<float> transforms;
	std::vector<float> modes;
	transforms.reserve(visuals.size() * 16u);
	modes.reserve(visuals.size() * 4u);

	for (const auto& visual : visuals)
	{
		if (visual.LoopLengthSamps == 0ul)
			continue;

		auto gateCount = static_cast<unsigned int>(std::llround(
			static_cast<double>(visual.LoopLengthSamps) / static_cast<double>(seedSamps)));
		gateCount = std::clamp(gateCount, 1u, MaxVisibleGates);

		const auto angleStep = static_cast<float>(constants::TWOPI) / static_cast<float>(gateCount);
		const auto phaseOffset = std::fmod(
			static_cast<float>(constants::TWOPI * visual.LoopIndexFrac),
			static_cast<float>(constants::TWOPI));
		const auto heightScale = std::max(visual.HalfHeight, MinVisualHalfHeight) / GateHalfHeight;
		const auto radiusScale = std::max(visual.Radius, MinVisualRadius) / GateOuterRadius;

		for (auto gate = 0u; gate < gateCount; ++gate)
		{
			transforms.push_back(phaseOffset + (angleStep * static_cast<float>(gate)));
			transforms.push_back(visual.YCenter);
			transforms.push_back(heightScale);
			transforms.push_back(radiusScale);
			modes.push_back(GateInstance);
		}

		transforms.push_back(0.0f);
		transforms.push_back(visual.YCenter);
		transforms.push_back(heightScale);
		transforms.push_back(radiusScale);
		modes.push_back(ColumnInstance);
	}

	const auto instanceCount = static_cast<unsigned int>(modes.size());
	SetInstanceAttributes({
		{ 3u, 4u, std::move(transforms) },
		{ 4u, 1u, std::move(modes) }
	}, instanceCount);
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
