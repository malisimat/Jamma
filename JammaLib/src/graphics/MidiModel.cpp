#include "MidiModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "../include/Constants.h"
#include "GlDrawContext.h"
#include "GlDeleteQueue.h"
#include "../midi/MidiLoop.h"
#include "../midi/LoopGridGeometry.h"
#include "../midi/MidiRouter.h"
#include "../resources/ResourceLib.h"
#include "../resources/ShaderResource.h"
#include "../utils/VecUtils.h"

using namespace graphics;
using base::DrawContext;
using graphics::GlDrawContext;

void MidiModel::AddTri(std::vector<float>& verts,
            float x1, float y1, float z1,
            float x2, float y2, float z2,
            float x3, float y3, float z3)
{
	verts.push_back(x1); verts.push_back(y1); verts.push_back(z1);
	verts.push_back(x2); verts.push_back(y2); verts.push_back(z2);
	verts.push_back(x3); verts.push_back(y3); verts.push_back(z3);
}

void MidiModel::AddUvTri(std::vector<float>& uvs, float u1, float v1, float u2, float v2, float u3, float v3)
{
	uvs.push_back(u1); uvs.push_back(v1);
	uvs.push_back(u2); uvs.push_back(v2);
	uvs.push_back(u3); uvs.push_back(v3);
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
	  DrawSelectionRing(true),
	  CenterPitch(60)
{
	ModelTextures = { "levels" };
	ModelShaders = { "midi_note" };
	Verts = MidiModel::BuildBaseVerts(MidiModel::BaseArcSegments);
	Uvs = MidiModel::BuildBaseUvs(MidiModel::BaseArcSegments);
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
	  DrawSelectionRing(true),
	  CenterPitch(60)
{
	if (ModelTextures.empty())
		ModelTextures = { "levels" };
	if (ModelShaders.empty())
		ModelShaders = { "midi_note" };
	if (Verts.empty())
		Verts = MidiModel::BuildBaseVerts(MidiModel::BaseArcSegments);
	if (Uvs.empty())
		Uvs = MidiModel::BuildBaseUvs(MidiModel::BaseArcSegments);
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
	if (_midiParams.DrawSelectionRing)
	{
		// Keep the shared MIDI target visible before the loop length is known.
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
}

MidiModel::~MidiModel()
{
	_ReleaseResources();
}

void MidiModel::Draw3d(DrawContext& ctx, unsigned int numInstances, base::DrawPass pass)
{
	ApplyPendingModelUpdate();

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.PushMvp(glm::rotate(glm::mat4(1.0),
		(float)(constants::TWOPI * _loopIndexFrac) * (1.0f - _editorMorph),
		glm::vec3(0.0f, 1.0f, 0.0f)));
	glCtx.SetUniform("EditorMorph", _editorMorph);
	glCtx.SetUniform("EditorActive", _editorActive ? 1.0f : 0.0f);
	glCtx.SetUniform("EditorPlayFrac", _editorPlayFrac);
	glCtx.SetUniform("EditorTimeOrigin", _editorGridLength
		? static_cast<float>(_editorTimeOrigin) / _editorGridLength : 0.0f);
	glCtx.SetUniform("EditorBottomPitch", _editorBottomPitch);
	glCtx.SetUniform("EditorVisibleRows", _editorVisibleRows);
	glCtx.SetUniform("EditorHoverU", _editorHoverU);
	glCtx.SetUniform("EditorHoverPitch", _editorHoverPitch);
	glCtx.SetUniform("EditorTargetStart", _editorTargetStart);
	glCtx.SetUniform("EditorTargetEnd", _editorTargetEnd);
	glCtx.SetUniform("EditorTargetInstance", _editorTargetInstance);
	glCtx.SetUniform("EditorHeldInstance", _editorHeldInstance);
	glCtx.SetUniform("EditorPreviewVelocity", _editorPreviewVelocity < 0
		? -1.0f : static_cast<float>(_editorPreviewVelocity) / 127.0f);
	const auto editorLength = _displayLengthSamps.load(std::memory_order_relaxed);
	const auto editorRadius = editorLength == 0u ? 50.0f : static_cast<float>(std::clamp(
		70.0 * std::log(static_cast<double>(editorLength)) - 600.0, 50.0, 400.0));
	glCtx.SetUniform("EditorGridRadius", editorRadius);

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
		glCtx.SetUniform("LoopSelected", _isSelected ? 1.0f : 0.0f);
		glCtx.SetUniform("LoopPressed", _clickPressed ? 1.0f : 0.0f);
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

		// The morphed ring is the editor's solid backing plane: later scene
		// geometry must not draw through it. Normal loop rings stay translucent.
		glDepthMask(_editorActive ? GL_TRUE : GL_FALSE);
		glCtx.SetUniform("RenderMode", 4);
		GuiModel::Draw3d(glCtx, numInstances, pass);

		_DrawEditorGrid(glCtx);
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

void MidiModel::SetEditorPitchRange(int bottomPitch, int visibleRows) noexcept
{
	const auto rows = std::clamp(visibleRows, 1, 128);
	const auto bottom = std::clamp(bottomPitch, 0, 128 - rows);
	if (rows != _editorVisibleRows || bottom != _editorBottomPitch)
	{
		_editorVisibleRows = rows;
		_editorBottomPitch = bottom;
		_editorGridSignatureValid = false;
		_editorGridDirty = true;
	}
}

void MidiModel::SetEditorHover(float u, int pitch) noexcept
{
	_editorHoverU = u >= 0.0f && u < 1.0f ? u : -1.0f;
	_editorHoverPitch = pitch >= 0 && pitch < 128 ? pitch : -1;
	_editorTargetStart = _editorTargetEnd = -1.0f;
	_editorTargetInstance = -1;
}

void MidiModel::SetEditorTarget(float startU, float endU, int pitch, int noteIndex) noexcept
{
	SetEditorHover(startU, pitch);
	_editorTargetStart = startU; _editorTargetEnd = endU;
	_editorTargetInstance = noteIndex < 0 ? -1 : noteIndex + (_midiParams.DrawSelectionRing ? 1 : 0);
}

void MidiModel::SetEditorHeld(int noteIndex, int proposedVelocity) noexcept
{
	if (noteIndex < 0 || static_cast<unsigned int>(noteIndex) >= _backNoteInstanceCount)
	{
		ClearEditorHeld();
		return;
	}
	// Seam copies share gl_InstanceID with the original instance.
	_editorHeldInstance = noteIndex + (_midiParams.DrawSelectionRing ? 1 : 0);
	_editorPreviewVelocity = proposedVelocity < 0 ? -1 : std::clamp(proposedVelocity, 1, 127);
}

bool MidiModel::EditorNoteMatches(std::size_t index, const midi::MidiNote& note,
	std::uint32_t loopLength) const noexcept
{
	if (_editorModelLength != loopLength || index >= _editorNoteSpans.size()) return false;
	const auto& applied = _editorNoteSpans[index];
	return applied.StartSample == note.StartSample && applied.DurationSamples == note.DurationSamples
		&& applied.Channel == note.Channel && applied.Note == note.Note
		&& applied.Velocity == note.Velocity && applied.Flags == note.Flags;
}

void MidiModel::ClearEditorHeld() noexcept
{
	_editorHeldInstance = -1;
	_editorPreviewVelocity = -1;
}

void MidiModel::SetEditorPreview(std::vector<EditorPreviewSpan> spans,
	std::uint32_t loopLength)
{
	if (_editorPreviewLength == loopLength && _editorPreviewSpans == spans)
		return;

	_editorPreviewSpans = std::move(spans);
	_editorPreviewLength = loopLength;
	_editorGridDirty = true;
}

void MidiModel::UpdateEditorGrid(std::uint32_t loopLength,
	const midi::MidiQuantisationSettings& settings, std::uint64_t transportStart)
{
	if (_editorGridSignatureValid && _editorGridLength == loopLength
		&& _editorGridSettings == settings && _editorGridTransportStart == transportStart)
		return;
	SetEditorHover(-1.0f, -1);
	_editorGridSignatureValid = true;
	_editorGridLength = loopLength;
	_editorGridSettings = settings;
	_editorGridTransportStart = transportStart;
	const auto grid = midi::LoopGridGeometry::Resolve(loopLength, settings, transportStart);
	_editorGridResolved = grid.has_value();
	_editorTimeOrigin = grid ? midi::LoopGridGeometry::DisplayOrigin(loopLength, settings, transportStart) : 0u;
	auto displayedGrid = grid;
	if (displayedGrid && _editorTimeOrigin)
	{
		// Physical sample zero is an artificial cut, not a master-grid line.
		displayedGrid->Boundaries.clear();
		for (std::size_t i = 1u; i + 1u < grid->Boundaries.size(); ++i)
			displayedGrid->Boundaries.push_back(midi::LoopGridGeometry::DisplaySample(
				grid->Boundaries[i], loopLength, _editorTimeOrigin));
		displayedGrid->Boundaries.push_back(0u);
		displayedGrid->Boundaries.push_back(loopLength);
		std::sort(displayedGrid->Boundaries.begin(), displayedGrid->Boundaries.end());
		displayedGrid->Boundaries.erase(std::unique(displayedGrid->Boundaries.begin(),
			displayedGrid->Boundaries.end()), displayedGrid->Boundaries.end());
	}
	_editorGridVertices = BuildEditorGridVertices(displayedGrid ? &*displayedGrid : nullptr,
		loopLength, _editorBottomPitch, _editorVisibleRows);
	_editorGridDirty = true;
}

std::vector<float> MidiModel::BuildEditorGridVertices(const midi::LoopGridGeometry* grid,
	std::uint32_t loopLength, int bottomPitch, int visibleRows)
{
	std::vector<float> vertices;
	if (visibleRows <= 0 || visibleRows > 128 || bottomPitch < 0 || bottomPitch + visibleRows > 128)
		return vertices;
	if (grid && loopLength > 0u)
	{
		vertices.reserve((grid->Boundaries.size() + static_cast<std::size_t>(visibleRows) + 1u) * 6u);
		for (std::size_t i = 0u; i < grid->Boundaries.size(); ++i)
		{
			const auto u = static_cast<float>(midi::LoopGridGeometry::SampleU(grid->Boundaries[i], loopLength));
			const auto weight = i % 4u == 0u || i + 1u == grid->Boundaries.size() ? 1.0f : 0.45f;
			vertices.insert(vertices.end(), { u, 0.0f, weight, u, 1.0f, weight });
		}
	}
	for (int row = 0; row <= visibleRows; ++row)
	{
		const auto v = static_cast<float>(row) / static_cast<float>(visibleRows);
		const auto weight = (bottomPitch + row) % 12 == 0 ? 1.0f : 0.35f;
		vertices.insert(vertices.end(), { 0.0f, v, weight, 1.0f, v, weight });
	}
	return vertices;
}

void MidiModel::UpdateModel(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps)
{
	_displayLengthSamps.store(loopLengthSamps, std::memory_order_relaxed);
	auto data = BuildInstanceData(spans, loopLengthSamps);
	// Instance identity is valid only until the next applied model replacement.
	++_editorModelGeneration;
	ClearEditorHeld();
	SetEditorHover(-1.0f, -1);
	SetEditorPreview({}, loopLengthSamps);
	_editorNoteSpans = std::move(data->EditorNotes);
	_editorModelLength = data->LoopLength;
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
	data->LoopLength = loopLengthSamps;
	data->EditorNotes.reserve(spans.size());
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

	const auto ringCount = _midiParams.DrawSelectionRing ? 1u : 0u;
	timePitchData.reserve((spans.size() + ringCount) * 4u);
	shapeData.reserve((spans.size() + ringCount) * 4u);

	// Only the designated stream emits the shared take ring. The ring is opaque
	// in the picker pass and is tagged via shape.w = 1.0.
	if (_midiParams.DrawSelectionRing)
	{
		timePitchData.push_back(0.0f);
		timePitchData.push_back(1.0f);
		timePitchData.push_back(PitchOffset(_midiParams.CenterPitch) * baseRadius);
		timePitchData.push_back(0.0f);

		shapeData.push_back(baseRadius * _midiParams.DiscRadiusFactor);
		shapeData.push_back(baseRadius * _midiParams.DiscRadialThicknessFactor);
		shapeData.push_back(baseRadius * _midiParams.DiscHeightFactor);
		shapeData.push_back(1.0f);
	}

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
		// Negative tag retains exact ring geometry while carrying the unscaled
		// pitch needed to place every row on the unwrapped grid.
		shapeData.push_back(-static_cast<float>(span.Note) - 1.0f);
		data->EditorNotes.push_back(span);
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

	++_editorModelGeneration;
	ClearEditorHeld();
	SetEditorHover(-1.0f, -1);
	SetEditorPreview({}, _displayLengthSamps.load(std::memory_order_relaxed));
	_editorNoteSpans = std::move(pending->EditorNotes);
	_editorModelLength = pending->LoopLength;
	_backNoteInstanceCount = pending->NoteCount;
	SetInstanceAttributes(std::move(pending->Attributes), pending->InstanceCount);
}

std::weak_ptr<resources::ShaderResource> MidiModel::GetShader()
{
	return GetShaderAt(0u);
}

void MidiModel::DrawMesh(GLuint shaderProgram, unsigned int drawInstances)
{
	const auto geometryPass = glGetUniformLocation(shaderProgram, "GeometryPass");
	// The ring uses only the curved sides. Notes retain the complete mesh,
	// including their start and end faces.
	if (_midiParams.DrawSelectionRing && drawInstances != 0u)
	{
		glUniform1i(geometryPass, 1);
		glDrawArraysInstanced(GL_TRIANGLES, 0, BaseArcSegments * 8u * 3u, 1u);
	}
	glUniform1i(geometryPass, 2);
	if (drawInstances > (_midiParams.DrawSelectionRing ? 1u : 0u))
	{
		const auto copyLocation = glGetUniformLocation(shaderProgram, "EditorWrapCopy");
		glUniform1f(copyLocation, 0.0f);
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3u, drawInstances);
		if (_editorTimeOrigin && _editorMorph >= 1.0f)
		{
			// Shifted notes crossing the display seam need their clipped left copy.
			glUniform1f(copyLocation, -1.0f);
			glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3u, drawInstances);
			glUniform1f(copyLocation, 0.0f);
		}
	}
}

void MidiModel::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	if (!HasCurrentGlContext())
		return;
	GuiModel::_InitResources(resourceLib, forceInit);
	if (auto resOpt = resourceLib.GetResource("midi_grid"); resOpt.has_value())
	{
		if (auto res = resOpt.value().lock(); res && resources::SHADER == res->GetType())
			_editorGridShader = std::dynamic_pointer_cast<resources::ShaderResource>(res);
	}
	if (_editorGridVao == 0u)
		glGenVertexArrays(1, &_editorGridVao);
	if (_editorGridVbo == 0u)
		glGenBuffers(1, &_editorGridVbo);
	glBindVertexArray(_editorGridVao);
	glBindBuffer(GL_ARRAY_BUFFER, _editorGridVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	_editorGridDirty = true;
	_InitAutomationGl(resourceLib);
}

void MidiModel::_ReleaseResources()
{
	GuiModel::_ReleaseResources();
	if (_editorGridVbo != 0u)
	{
		graphics::GlDeleteQueue::DeleteBuffers(1, &_editorGridVbo);
		_editorGridVbo = 0u;
	}
	if (_editorGridVao != 0u)
	{
		graphics::GlDeleteQueue::DeleteVertexArrays(1, &_editorGridVao);
		_editorGridVao = 0u;
	}
	_editorGridVertexCount = 0u;
	_editorPreviewVertexCount = 0u;
	_editorGridDirty = true;
	_ReleaseAutomationGl();
}

void MidiModel::_DrawEditorGrid(GlDrawContext& glCtx)
{
	if (_editorMorph <= 0.0f || _editorGridVao == 0u || _editorGridVbo == 0u)
		return;
	const auto shader = _editorGridShader.lock();
	if (!shader)
		return;
	if (_editorGridDirty)
	{
		std::vector<float> vertices = _editorGridVertices;
		_editorGridVertexCount = static_cast<unsigned int>(vertices.size() / 3u);
		for (const auto& preview : _editorPreviewSpans)
		{
			if (_editorPreviewLength == 0u || preview.Start >= preview.End
				|| preview.End > _editorPreviewLength
				|| preview.Pitch < _editorBottomPitch
				|| preview.Pitch >= _editorBottomPitch + _editorVisibleRows) continue;
			const auto left = static_cast<float>(midi::LoopGridGeometry::DisplaySample(
				preview.Start, _editorPreviewLength, _editorTimeOrigin)) / _editorPreviewLength;
			const auto right = left + static_cast<float>(preview.End - preview.Start) / _editorPreviewLength;
			const auto bottom = (static_cast<float>(preview.Pitch - _editorBottomPitch) + 0.10f)
				/ _editorVisibleRows;
			const auto top = (static_cast<float>(preview.Pitch - _editorBottomPitch) + 0.90f)
				/ _editorVisibleRows;
			const auto weight = preview.Ghost ? -3.0f : preview.Fill ? -1.0f : -2.0f;
			const auto append = [&](float a, float b) {
				vertices.insert(vertices.end(), { a, bottom, weight, b, bottom, weight,
					a, top, weight, a, top, weight, b, bottom, weight, b, top, weight });
			};
			append(left, std::min(right, 1.0f));
			if (right > 1.0f) append(0.0f, right - 1.0f);
		}
		_editorPreviewVertexCount = static_cast<unsigned int>(vertices.size() / 3u)
			- _editorGridVertexCount;
		glBindBuffer(GL_ARRAY_BUFFER, _editorGridVbo);
		glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
			vertices.data(), GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		_editorGridDirty = false;
	}
	if (_editorGridVertexCount == 0u && _editorPreviewVertexCount == 0u)
		return;
	const auto pos = ModelPosition();
	const auto scale = ModelScale();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));
	glUseProgram(shader->GetId());
	glCtx.SetUniform("EditorPreviewFirstVertex", static_cast<int>(_editorGridVertexCount));
	shader->SetUniforms(glCtx);
	GLboolean wasBlend = glIsEnabled(GL_BLEND);
	GLint oldSrcRgb = GL_ONE, oldDstRgb = GL_ZERO;
	GLint oldSrcAlpha = GL_ONE, oldDstAlpha = GL_ZERO;
	glGetIntegerv(GL_BLEND_SRC_RGB, &oldSrcRgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &oldDstRgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &oldDstAlpha);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(_editorGridVao);
	glDrawArrays(GL_LINES, 0, _editorGridVertexCount);
	if (_editorPreviewVertexCount > 0u)
	{
		// Held spans must remain visible above tall notes at every velocity.
		const auto wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
		glDisable(GL_DEPTH_TEST);
		glDrawArrays(GL_TRIANGLES, _editorGridVertexCount, _editorPreviewVertexCount);
		if (wasDepthTest) glEnable(GL_DEPTH_TEST);
	}
	glBindVertexArray(0);
	glBlendFuncSeparate(oldSrcRgb, oldDstRgb, oldSrcAlpha, oldDstAlpha);
	if (!wasBlend)
		glDisable(GL_BLEND);
	glUseProgram(0);
	glCtx.PopMvp();
	glCtx.PopMvp();
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
