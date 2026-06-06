#include "MidiModel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "../include/Constants.h"
#include "GlDrawContext.h"
#include "../utils/VecUtils.h"

using namespace engine;
using base::DrawContext;
using graphics::GlDrawContext;

namespace
{
	static constexpr unsigned int BaseArcSegments = 16u;
	static constexpr unsigned int TimePitchAttribute = 3u;
	static constexpr unsigned int ShapeAttribute = 4u;

	void AddTri(std::vector<float>& verts,
	            float x1, float y1, float z1,
	            float x2, float y2, float z2,
	            float x3, float y3, float z3)
	{
		verts.push_back(x1); verts.push_back(y1); verts.push_back(z1);
		verts.push_back(x2); verts.push_back(y2); verts.push_back(z2);
		verts.push_back(x3); verts.push_back(y3); verts.push_back(z3);
	}

	void AddUvTri(std::vector<float>& uvs, float u1, float v1, float u2, float v2, float u3, float v3)
	{
		uvs.push_back(u1); uvs.push_back(v1);
		uvs.push_back(u2); uvs.push_back(v2);
		uvs.push_back(u3); uvs.push_back(v3);
	}
}

MidiModelParams::MidiModelParams()
	: gui::GuiModelParams(),
	  Radius(1.0f),
	  RadialThickness(0.035f),
	  NoteHeight(0.035f),
	  PitchStep(0.035f),
	  CenterPitch(60)
{
	ModelTextures = { "levels" };
	ModelShaders = { "midi_note" };
	Verts = MidiModel::BuildBaseVerts(BaseArcSegments);
	Uvs = MidiModel::BuildBaseUvs(BaseArcSegments);
}

MidiModelParams::MidiModelParams(gui::GuiModelParams params)
	: gui::GuiModelParams(params),
	  Radius(1.0f),
	  RadialThickness(0.035f),
	  NoteHeight(0.035f),
	  PitchStep(0.035f),
	  CenterPitch(60)
{
	if (ModelTextures.empty())
		ModelTextures = { "levels" };
	if (ModelShaders.empty())
		ModelShaders = { "midi_note" };
	if (Verts.empty())
		Verts = MidiModel::BuildBaseVerts(BaseArcSegments);
	if (Uvs.empty())
		Uvs = MidiModel::BuildBaseUvs(BaseArcSegments);
}

MidiModel::MidiModel(MidiModelParams params)
	: GuiModel(params),
	  _midiParams(params),
	  _loopIndexFrac(0.0),
	  _pendingModelUpdate(nullptr)
{
	SetInstanceAttributes({}, 0u);
}

MidiModel::~MidiModel()
{
}

void MidiModel::Draw3d(DrawContext& ctx, unsigned int numInstances, base::DrawPass pass)
{
	ApplyPendingModelUpdate();

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.PushMvp(glm::rotate(glm::mat4(1.0),
		(float)(constants::TWOPI * _loopIndexFrac),
		glm::vec3(0.0f, 1.0f, 0.0f)));

	switch (pass)
	{
	case base::PASS_PICKER:
	{
		auto idVec = GlobalId();
		idVec.resize(3);
		for (auto& idPart : idVec)
			idPart += 1;

		glCtx.SetUniform("ObjectId", utils::VecToId(idVec));
		glCtx.SetUniform("RenderMode", 1);
		break;
	}
	case base::PASS_HIGHLIGHT:
		glCtx.SetUniform("Highlight", _isSelected ? 1.0f : 0.0f);
		glCtx.SetUniform("RenderMode", 2);
		break;
	default:
		glCtx.SetUniform("LoopHover", _isPicking3d ? 1.0f : 0.0f);
		glCtx.SetUniform("RenderMode", 0);
		break;
	}

	GuiModel::Draw3d(glCtx, numInstances, pass);
	glCtx.PopMvp();
}

void MidiModel::SetLoopIndexFrac(double frac) noexcept
{
	_loopIndexFrac = frac;
}

void MidiModel::UpdateModel(const std::vector<MidiNote>& spans, std::uint32_t loopLengthSamps)
{
	auto data = BuildInstanceData(spans, loopLengthSamps);
	SetInstanceAttributes(std::move(data->Attributes), data->InstanceCount);
}

void MidiModel::QueueModelUpdate(const std::vector<MidiNote>& spans, std::uint32_t loopLengthSamps)
{
	_pendingModelUpdate.store(BuildInstanceData(spans, loopLengthSamps), std::memory_order_release);
}

std::shared_ptr<MidiModel::ModelInstanceData> MidiModel::BuildInstanceData(const std::vector<MidiNote>& spans,
	std::uint32_t loopLengthSamps) const
{
	auto data = std::make_shared<ModelInstanceData>();
	std::vector<float> timePitchData;
	std::vector<float> shapeData;

	if (0u == loopLengthSamps || spans.empty())
	{
		data->Attributes = {
			{ TimePitchAttribute, 4u, std::move(timePitchData) },
			{ ShapeAttribute, 4u, std::move(shapeData) }
		};
		data->InstanceCount = 0u;
		return data;
	}

	const double rawRadius = 70.0 * std::log(static_cast<double>(loopLengthSamps)) - 600.0;
	const float baseRadius = static_cast<float>(std::clamp(rawRadius, 50.0, 400.0));

	const float radialThickness = baseRadius * _midiParams.RadialThickness;
	const float noteHeight = baseRadius * _midiParams.NoteHeight;

	timePitchData.reserve(spans.size() * 4u);
	shapeData.reserve(spans.size() * 4u);

	for (const auto& span : spans)
	{
		if (0u == span.DurationSamples || span.StartSample >= loopLengthSamps)
			continue;

		const auto startFrac = static_cast<float>(span.StartSample) / static_cast<float>(loopLengthSamps);
		const auto durationFrac = static_cast<float>(span.DurationSamples) / static_cast<float>(loopLengthSamps);
		const auto velocity = std::clamp(static_cast<float>(span.Velocity) / 127.0f, 0.0f, 1.0f);

		timePitchData.push_back(startFrac);
		timePitchData.push_back(durationFrac);
		timePitchData.push_back(PitchOffset(span.Note) * baseRadius);
		timePitchData.push_back(velocity);

		shapeData.push_back(baseRadius);
		shapeData.push_back(radialThickness);
		shapeData.push_back(noteHeight);
		shapeData.push_back(0.0f);
	}

	data->InstanceCount = static_cast<unsigned int>(timePitchData.size() / 4u);
	data->Attributes = {
		{ TimePitchAttribute, 4u, std::move(timePitchData) },
		{ ShapeAttribute, 4u, std::move(shapeData) }
	};
	return data;
}

void MidiModel::ApplyPendingModelUpdate()
{
	auto pending = _pendingModelUpdate.exchange(nullptr, std::memory_order_acq_rel);
	if (!pending)
		return;

	SetInstanceAttributes(std::move(pending->Attributes), pending->InstanceCount);
}

std::weak_ptr<resources::ShaderResource> MidiModel::GetShader()
{
	if (!_modelShaders.empty())
		return *_modelShaders.begin();

	return std::weak_ptr<resources::ShaderResource>();
}

std::vector<float> MidiModel::BuildBaseVerts(unsigned int segments)
{
	std::vector<float> verts;
	if (0u == segments)
		return verts;

	verts.reserve((segments * 8u + 4u) * 9u);

	for (auto segment = 0u; segment < segments; ++segment)
	{
		const auto x1 = static_cast<float>(segment) / static_cast<float>(segments);
		const auto x2 = static_cast<float>(segment + 1u) / static_cast<float>(segments);

		AddTri(verts, x1, -0.5f,  1.0f, x2, -0.5f,  1.0f, x1,  0.5f,  1.0f);
		AddTri(verts, x1,  0.5f,  1.0f, x2, -0.5f,  1.0f, x2,  0.5f,  1.0f);
		AddTri(verts, x1, -0.5f, -1.0f, x1,  0.5f, -1.0f, x2, -0.5f, -1.0f);
		AddTri(verts, x1,  0.5f, -1.0f, x2,  0.5f, -1.0f, x2, -0.5f, -1.0f);
		AddTri(verts, x1,  0.5f, -1.0f, x1,  0.5f,  1.0f, x2,  0.5f, -1.0f);
		AddTri(verts, x1,  0.5f,  1.0f, x2,  0.5f,  1.0f, x2,  0.5f, -1.0f);
		AddTri(verts, x1, -0.5f, -1.0f, x2, -0.5f, -1.0f, x1, -0.5f,  1.0f);
		AddTri(verts, x1, -0.5f,  1.0f, x2, -0.5f, -1.0f, x2, -0.5f,  1.0f);
	}

	AddTri(verts, 0.0f, -0.5f, -1.0f,  0.0f, -0.5f,  1.0f,  0.0f,  0.5f, -1.0f);
	AddTri(verts, 0.0f, -0.5f,  1.0f,  0.0f,  0.5f,  1.0f,  0.0f,  0.5f, -1.0f);
	AddTri(verts, 1.0f, -0.5f, -1.0f,  1.0f,  0.5f, -1.0f,  1.0f,  0.5f,  1.0f);
	AddTri(verts, 1.0f, -0.5f, -1.0f,  1.0f,  0.5f,  1.0f,  1.0f, -0.5f,  1.0f);

	return verts;
}

std::vector<float> MidiModel::BuildBaseUvs(unsigned int segments)
{
	std::vector<float> uvs;
	if (0u == segments)
		return uvs;

	uvs.reserve((segments * 8u + 4u) * 6u);

	for (auto segment = 0u; segment < segments; ++segment)
	{
		const auto x1 = static_cast<float>(segment) / static_cast<float>(segments);
		const auto x2 = static_cast<float>(segment + 1u) / static_cast<float>(segments);
		for (auto face = 0u; face < 4u; ++face)
		{
			AddUvTri(uvs, x1, 0.0f, x2, 0.0f, x1, 1.0f);
			AddUvTri(uvs, x1, 1.0f, x2, 0.0f, x2, 1.0f);
		}
	}

	AddUvTri(uvs, 0.0f, 0.0f,  0.0f, 0.0f,  0.0f, 1.0f);
	AddUvTri(uvs, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f, 1.0f);
	AddUvTri(uvs, 1.0f, 0.0f,  1.0f, 1.0f,  1.0f, 1.0f);
	AddUvTri(uvs, 1.0f, 0.0f,  1.0f, 1.0f,  1.0f, 0.0f);

	return uvs;
}

float MidiModel::PitchOffset(std::uint8_t note) const noexcept
{
	const auto delta = static_cast<int>(note) - static_cast<int>(_midiParams.CenterPitch);
	const auto offset = static_cast<float>(delta) * _midiParams.PitchStep;
	return std::clamp(offset, -0.9f, 0.9f);
}