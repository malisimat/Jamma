#include "QuantisationDivisionModel.h"

#include <algorithm>
#include <cmath>
#include "glm/ext.hpp"
#include "GlDrawContext.h"
#include "QuantisationModel.h"
#include "../../include/Constants.h"

using namespace engine;
using namespace utils;
using namespace timing;

namespace
{
	constexpr float StripOuterRadius = 180.0f;
	constexpr float StripHalfHeight = 138.0f;
	constexpr float MinVisualHalfHeight = 8.0f;
	constexpr float MinVisualRadius = 24.0f;
	constexpr unsigned int MaxVisibleDivisions = 256u;

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

	void BuildDivisionMesh(std::vector<float>& verts, std::vector<float>& uvs)
	{
		constexpr float xMin = -1.0f;
		constexpr float xMax = 1.0f;
		constexpr float yMin = -StripHalfHeight;
		constexpr float yMax = StripHalfHeight;
		constexpr float zMin = StripOuterRadius - 1.0f;
		constexpr float zMax = StripOuterRadius + 2.0f;

		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMin, yMin, zMax),
			glm::vec3(xMax, yMin, zMax),
			glm::vec3(xMax, yMax, zMax),
			glm::vec3(xMin, yMax, zMax));
		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMax, yMin, zMin),
			glm::vec3(xMin, yMin, zMin),
			glm::vec3(xMin, yMax, zMin),
			glm::vec3(xMax, yMax, zMin));
		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMin, yMax, zMin),
			glm::vec3(xMin, yMax, zMax),
			glm::vec3(xMax, yMax, zMax),
			glm::vec3(xMax, yMax, zMin));
		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMax, yMin, zMin),
			glm::vec3(xMax, yMin, zMax),
			glm::vec3(xMin, yMin, zMax),
			glm::vec3(xMin, yMin, zMin));
		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMax, yMin, zMax),
			glm::vec3(xMax, yMin, zMin),
			glm::vec3(xMax, yMax, zMin),
			glm::vec3(xMax, yMax, zMax));
		AppendQuad(verts, &uvs, 0.0f,
			glm::vec3(xMin, yMin, zMin),
			glm::vec3(xMin, yMin, zMax),
			glm::vec3(xMin, yMax, zMax),
			glm::vec3(xMin, yMax, zMin));
	}
}

QuantisationDivisionModel::QuantisationDivisionModel() :
	GuiModel(gui::GuiModelParams()),
	_overlayVisible(false),
	_overlayAlpha(0.0f),
	_confirmedAt(Timer::GetZero())
{
	_modelParams.ModelShaders = { "quantisation_division" };
	std::vector<float> verts;
	std::vector<float> uvs;
	BuildDivisionMesh(verts, uvs);
	SetGeometry(std::move(verts), std::move(uvs));
	SetVisible(false);
}

void QuantisationDivisionModel::Draw3d(base::DrawContext& ctx,
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

	auto highlight = 0.25f;
	if (!Timer::IsZero(_confirmedAt))
	{
		const auto elapsed = Timer::GetElapsedSeconds(_confirmedAt, Timer::GetTime());
		if (elapsed < 1.0)
			highlight = 1.0f - static_cast<float>(elapsed * 0.75);
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

void QuantisationDivisionModel::SetLoopTakeVisuals(const std::vector<timing::QuantisationLoopTakeVisual>& visuals)
{
	std::vector<float> transforms;
	std::vector<float> bands;
	std::vector<float> fillHalfWidths;
	transforms.reserve(visuals.size() * 16u);
	bands.reserve(visuals.size() * 8u);
	fillHalfWidths.reserve(visuals.size() * 8u);

	for (const auto& visual : visuals)
	{
		const auto counts = QuantisationModel::ResolveVisualCounts(visual);
		auto divisionCount = counts.FractionDivisionCount;
		if (0u == divisionCount)
			continue;

		divisionCount = std::clamp(divisionCount, 1u, MaxVisibleDivisions);

		const auto angleStep = static_cast<float>(constants::TWOPI) / static_cast<float>(divisionCount);
		const auto centerAngleOffset = angleStep * 0.25f;
		const auto fillHalfWidth = std::clamp(
			StripOuterRadius * std::tan(angleStep * 0.25f),
			6.0f,
			StripOuterRadius * 0.45f);
		const auto loopIndexAngle = std::fmod(
			static_cast<float>(constants::TWOPI * visual.LoopIndexFrac),
			static_cast<float>(constants::TWOPI));
		const auto phaseOffsetAngle = static_cast<float>(constants::TWOPI)
			* (static_cast<float>(visual.PhaseOffsetSamps)
				/ static_cast<float>(visual.LoopLengthSamps));
		const auto phaseOffset = loopIndexAngle + phaseOffsetAngle;
		const auto heightScale = std::max(visual.HalfHeight, MinVisualHalfHeight) / StripHalfHeight;
		const auto radiusScale = std::max(visual.Radius, MinVisualRadius) / StripOuterRadius;

		for (auto division = 0u; division < divisionCount; ++division)
		{
			// Align strip edges to the actual quant-grid lines: each strip starts at
			// a division boundary and extends halfway toward the next boundary.
			transforms.push_back(phaseOffset + (angleStep * static_cast<float>(division)) + centerAngleOffset);
			transforms.push_back(visual.YCenter);
			transforms.push_back(heightScale);
			transforms.push_back(radiusScale);
			bands.push_back(static_cast<float>(division % 2u));
			fillHalfWidths.push_back(fillHalfWidth);
		}
	}

	const auto instanceCount = static_cast<unsigned int>(bands.size());
	SetInstanceAttributes({
		{ 3u, 4u, std::move(transforms) },
		{ 4u, 1u, std::move(bands) },
		{ 5u, 1u, std::move(fillHalfWidths) }
	}, instanceCount);
}

void QuantisationDivisionModel::SetOverlayVisible(bool visible, bool confirm)
{
	_overlayVisible = visible;
	SetVisible(visible && (_overlayAlpha > 0.001f));
	if (confirm)
		_confirmedAt = Timer::GetTime();
}

void QuantisationDivisionModel::SetOverlayAlpha(float alpha) noexcept
{
	_overlayAlpha = std::clamp(alpha, 0.0f, 1.0f);
	SetVisible(_overlayVisible && (_overlayAlpha > 0.001f));
}

bool QuantisationDivisionModel::OverlayVisible() const noexcept
{
	return _overlayVisible;
}
