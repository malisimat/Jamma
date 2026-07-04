#include "MidiModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "../include/Constants.h"
#include "GlDrawContext.h"
#include "GlDeleteQueue.h"
#include "../midi/MidiLoop.h"
#include "../midi/MidiRouter.h"
#include "../resources/ResourceLib.h"
#include "../resources/ShaderResource.h"
#include "../utils/VecUtils.h"

using namespace graphics;
using base::DrawContext;
using graphics::GlDrawContext;

namespace
{
	static constexpr unsigned int BaseArcSegments = 16u;
	static constexpr unsigned int TimePitchAttribute = 3u;
	static constexpr unsigned int ShapeAttribute = 4u;

	// Automation curtain tessellation around the loop circumference. Higher counts
	// give a smoother undulating ribbon at the cost of more vertices (built once).
	static constexpr unsigned int AutomationArcSegments = 160u;

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
	  DiscRadiusFactor(1.0f),
	  DiscRadialThicknessFactor(0.12f),
	  DiscHeightFactor(0.06f),
	  DiscAlpha(0.2f),
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
	  DiscRadiusFactor(1.0f),
	  DiscRadialThicknessFactor(0.12f),
	  DiscHeightFactor(0.06f),
	  DiscAlpha(0.2f),
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
	  _backNoteInstanceCount(0u),
	  _pendingModelUpdate(nullptr),
	  _automationSource(nullptr),
	  _displayLengthSamps(0u),
	  _automationGlReady(false),
	  _automationShader(),
	  _curtainVao(0u),
	  _curtainVbo(0u),
	  _curtainVertCount(0u),
	  _crownVao(0u),
	  _crownVbo(0u),
	  _crownVertCount(0u),
	  _playVao(0u),
	  _playVbo(0u),
	  _dotVao(0u),
	  _dotVbo(0u)
{
	// Emit a disc at the minimum radius so the loop target is visible
	// from the moment it is created (before the loop length is known).
	constexpr float defaultRadius = 50.0f;
	SetInstanceAttributes(
		{
			{ TimePitchAttribute, 4u, { 0.0f, 1.0f, 0.0f, 0.0f } },
			{ ShapeAttribute, 4u,
				{ defaultRadius * _midiParams.DiscRadiusFactor,
				  defaultRadius * _midiParams.DiscRadialThicknessFactor,
				  defaultRadius * _midiParams.DiscHeightFactor,
				  1.0f } }
		},
		1u);
}

MidiModel::~MidiModel()
{
	_ReleaseAutomationGl();
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
		glCtx.SetUniform("DiscAlpha", _midiParams.DiscAlpha);
		glCtx.SetUniform("RenderMode", 3);
		break;
	}

	if (base::PASS_SCENE == pass)
	{
		GLboolean prevDepthMask = GL_TRUE;
		glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

		glDepthMask(GL_TRUE);
		GuiModel::Draw3d(glCtx, numInstances, pass);

		glDepthMask(GL_FALSE);
		glCtx.SetUniform("RenderMode", 4);
		GuiModel::Draw3d(glCtx, numInstances, pass);

		_DrawAutomation(glCtx);

		glDepthMask(prevDepthMask);
	}
	else
	{
		GuiModel::Draw3d(glCtx, numInstances, pass);
	}
	glCtx.PopMvp();
}

void MidiModel::SetLoopIndexFrac(double frac) noexcept
{
	_loopIndexFrac = frac;
}

void MidiModel::UpdateModel(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps)
{
	_displayLengthSamps.store(loopLengthSamps, std::memory_order_relaxed);
	auto data = BuildInstanceData(spans, loopLengthSamps);
	_backNoteInstanceCount = data->NoteCount;
	SetInstanceAttributes(std::move(data->Attributes), data->InstanceCount);
}

void MidiModel::QueueModelUpdate(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps)
{
	_displayLengthSamps.store(loopLengthSamps, std::memory_order_relaxed);
	_pendingModelUpdate.store(BuildInstanceData(spans, loopLengthSamps), std::memory_order_release);
}

std::shared_ptr<MidiModel::ModelInstanceData> MidiModel::BuildInstanceData(const std::vector<midi::MidiNote>& spans,
	std::uint32_t loopLengthSamps) const
{
	auto data = std::make_shared<ModelInstanceData>();
	std::vector<float> timePitchData;
	std::vector<float> shapeData;

	if (0u == loopLengthSamps)
	{
		data->Attributes = {
			{ TimePitchAttribute, 4u, std::move(timePitchData) },
			{ ShapeAttribute, 4u, std::move(shapeData) }
		};
		data->InstanceCount = 0u;
		data->NoteCount = 0u;
		return data;
	}

	const double rawRadius = 70.0 * std::log(static_cast<double>(loopLengthSamps)) - 600.0;
	const float baseRadius = static_cast<float>(std::clamp(rawRadius, 50.0, 400.0));

	const float radialThickness = baseRadius * _midiParams.RadialThickness;
	const float noteHeight = baseRadius * _midiParams.NoteHeight;

	timePitchData.reserve((spans.size() + 1u) * 4u);
	shapeData.reserve((spans.size() + 1u) * 4u);

	// Always emit a semi-transparent disc at the middle-C plane so every MIDI
	// loop presents a large, reliable hover/select target (the disc is opaque in
	// the picker pass and shares the loop's ObjectId). Tagged via shape.w = 1.0.
	timePitchData.push_back(0.0f);
	timePitchData.push_back(1.0f);
	timePitchData.push_back(PitchOffset(_midiParams.CenterPitch) * baseRadius);
	timePitchData.push_back(0.0f);

	shapeData.push_back(baseRadius * _midiParams.DiscRadiusFactor);
	shapeData.push_back(baseRadius * _midiParams.DiscRadialThicknessFactor);
	shapeData.push_back(baseRadius * _midiParams.DiscHeightFactor);
	shapeData.push_back(1.0f);

	unsigned int noteCount = 0u;
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
		++noteCount;
	}

	data->NoteCount = noteCount;
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

	_backNoteInstanceCount = pending->NoteCount;
	SetInstanceAttributes(std::move(pending->Attributes), pending->InstanceCount);
}

std::weak_ptr<resources::ShaderResource> MidiModel::GetShader()
{
	return GetShaderAt(0u);
}

void MidiModel::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	GuiModel::_InitResources(resourceLib, forceInit);
	_InitAutomationGl(resourceLib);
}

void MidiModel::_ReleaseResources()
{
	GuiModel::_ReleaseResources();
	_ReleaseAutomationGl();
}

void MidiModel::_InitAutomationGl(resources::ResourceLib& resourceLib)
{
	if (_automationGlReady || !HasCurrentGlContext())
		return;

	if (auto resOpt = resourceLib.GetResource("automation"); resOpt.has_value())
	{
		if (auto res = resOpt.value().lock(); res && resources::SHADER == res->GetType())
			_automationShader = std::dynamic_pointer_cast<resources::ShaderResource>(res);
	}

	// Curtain: a closed triangle strip of bottom/top vertex pairs around the loop.
	std::vector<float> curtain;
	curtain.reserve((AutomationArcSegments + 1u) * 4u);
	for (unsigned int i = 0u; i <= AutomationArcSegments; ++i)
	{
		const float t = static_cast<float>(i) / static_cast<float>(AutomationArcSegments);
		curtain.push_back(t); curtain.push_back(0.0f); // base
		curtain.push_back(t); curtain.push_back(1.0f); // top
	}
	_curtainVertCount = (AutomationArcSegments + 1u) * 2u;

	// Crown: the top edge as a closed line loop.
	std::vector<float> crown;
	crown.reserve(AutomationArcSegments * 2u);
	for (unsigned int i = 0u; i < AutomationArcSegments; ++i)
	{
		const float t = static_cast<float>(i) / static_cast<float>(AutomationArcSegments);
		crown.push_back(t); crown.push_back(1.0f);
	}
	_crownVertCount = AutomationArcSegments;

	const float play[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float dot[2] = { 0.0f, 1.0f };

	const auto makeVao = [](GLuint& vao, GLuint& vbo, const float* data, std::size_t floatCount)
	{
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, floatCount * sizeof(GLfloat), data, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glBindVertexArray(0);
	};

	makeVao(_curtainVao, _curtainVbo, curtain.data(), curtain.size());
	makeVao(_crownVao, _crownVbo, crown.data(), crown.size());
	makeVao(_playVao, _playVbo, play, 4u);
	makeVao(_dotVao, _dotVbo, dot, 2u);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	_automationGlReady = true;
}

void MidiModel::_ReleaseAutomationGl()
{
	const auto dropBuffer = [](GLuint& id)
	{
		if (id != 0u)
		{
			graphics::GlDeleteQueue::DeleteBuffers(1, &id);
			id = 0u;
		}
	};
	const auto dropVao = [](GLuint& id)
	{
		if (id != 0u)
		{
			graphics::GlDeleteQueue::DeleteVertexArrays(1, &id);
			id = 0u;
		}
	};

	dropBuffer(_curtainVbo); dropVao(_curtainVao);
	dropBuffer(_crownVbo);   dropVao(_crownVao);
	dropBuffer(_playVbo);    dropVao(_playVao);
	dropBuffer(_dotVbo);     dropVao(_dotVao);

	_curtainVertCount = 0u;
	_crownVertCount = 0u;
	_automationGlReady = false;
}

void MidiModel::_DrawAutomation(GlDrawContext& glCtx)
{
	if (!_automationSource)
		return;

	auto shader = _automationShader.lock();
	if (!shader || 0u == _curtainVao)
		return;

	const auto lengthSamps = _displayLengthSamps.load(std::memory_order_relaxed);
	if (0u == lengthSamps)
		return;

	// Reproduce the placement GuiModel applies to the note instances so the curtain
	// sits concentric with the note ring.
	const auto pos = ModelPosition();
	const auto scale = ModelScale();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	const double rawRadius = 70.0 * std::log(static_cast<double>(lengthSamps)) - 600.0;
	const float baseRadius = static_cast<float>(std::clamp(rawRadius, 50.0, 400.0));
	const float laneHeight = baseRadius * 0.64f;
	const bool recording = midi::MidiRouter::IsAutomationRecordHeld();
	const float playFrac = static_cast<float>(_loopIndexFrac);

	const GLuint prog = shader->GetId();
	glUseProgram(prog);
	shader->SetUniforms(glCtx); // MVP

	const GLint locPoints = glGetUniformLocation(prog, "AutoPoints");
	const GLint locCount = glGetUniformLocation(prog, "AutoPointCount");
	const GLint locRadius = glGetUniformLocation(prog, "LaneRadius");
	const GLint locHeight = glGetUniformLocation(prog, "LaneHeight");
	const GLint locColor = glGetUniformLocation(prog, "LaneColor");
	const GLint locGlow = glGetUniformLocation(prog, "RecordGlow");
	const GLint locPlay = glGetUniformLocation(prog, "PlayFrac");
	const GLint locMode = glGetUniformLocation(prog, "RenderMode");

	glUniform1f(locPlay, playFrac);

	GLboolean prevDepthMask = GL_TRUE;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
	const GLboolean prevBlend = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glEnable(GL_PROGRAM_POINT_SIZE);

	static const std::array<glm::vec3, 8> palette = { {
		{ 0.20f, 0.85f, 1.00f }, { 1.00f, 0.55f, 0.20f }, { 0.55f, 1.00f, 0.45f }, { 1.00f, 0.35f, 0.65f },
		{ 0.70f, 0.55f, 1.00f }, { 1.00f, 0.90f, 0.30f }, { 0.30f, 0.95f, 0.80f }, { 0.95f, 0.45f, 0.40f }
	} };

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> pts;
	std::array<float, midi::AutomationLane::MaxPoints * 2u> flat;

	for (std::size_t lane = 0u; lane < midi::MidiLoop::MaxAutomationLanes; ++lane)
	{
		const bool active = _automationSource->IsAutomationLaneActive(lane);
		const auto count = _automationSource->SnapshotAutomationLanePoints(lane, pts.data(), pts.size());
		if (!active && 0u == count)
			continue;

		for (std::uint16_t i = 0u; i < count; ++i)
		{
			flat[i * 2u] = pts[i].first;
			flat[i * 2u + 1u] = pts[i].second;
		}

		const float laneRadius = baseRadius * 1.5f * (1.05f + static_cast<float>(lane) * 0.045f);
		const auto& col = palette[lane % palette.size()];

		glUniform2fv(locPoints, count, flat.data());
		glUniform1i(locCount, static_cast<GLint>(count));
		glUniform1f(locRadius, laneRadius);
		glUniform1f(locHeight, laneHeight);
		glUniform3f(locColor, col.x, col.y, col.z);
		glUniform1f(locGlow, (active && recording) ? 1.0f : 0.0f);

		glUniform1i(locMode, 0); // Curtain
		glBindVertexArray(_curtainVao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(_curtainVertCount));

		glUniform1i(locMode, 1); // Crown ring
		glBindVertexArray(_crownVao);
		glDrawArrays(GL_LINE_LOOP, 0, static_cast<GLsizei>(_crownVertCount));

		glUniform1i(locMode, 2); // Playhead line
		glBindVertexArray(_playVao);
		glDrawArrays(GL_LINES, 0, 2);

		glUniform1i(locMode, 3); // Play dot
		glBindVertexArray(_dotVao);
		glDrawArrays(GL_POINTS, 0, 1);
	}

	glBindVertexArray(0);
	glUseProgram(0);

	glDisable(GL_PROGRAM_POINT_SIZE);
	if (!prevBlend)
		glDisable(GL_BLEND);
	glDepthMask(prevDepthMask);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

std::vector<float> MidiModel::BuildBaseVerts(unsigned int segments)
{
	std::vector<float> verts;
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

	// Mark arc end-cap triangles with UV.y = 2.0 so the fragment shader can
	// discard them for full-circle disc instances (both caps land at the same
	// world-space angle, leaving a visible seam fin if not discarded).
	AddUvTri(uvs, 0.0f, 2.0f,  0.0f, 2.0f,  0.0f, 2.0f);
	AddUvTri(uvs, 0.0f, 2.0f,  0.0f, 2.0f,  0.0f, 2.0f);
	AddUvTri(uvs, 1.0f, 2.0f,  1.0f, 2.0f,  1.0f, 2.0f);
	AddUvTri(uvs, 1.0f, 2.0f,  1.0f, 2.0f,  1.0f, 2.0f);

	return uvs;
}

float MidiModel::PitchOffset(std::uint8_t note) const noexcept
{
	const auto delta = static_cast<int>(note) - static_cast<int>(_midiParams.CenterPitch);
	const auto offset = static_cast<float>(delta) * _midiParams.PitchStep;
	return std::clamp(offset, -0.9f, 0.9f);
}