#include "GuiHud.h"

#include <array>
#include <algorithm>
#include <cmath>
#include "GuiButton.h"
#include "GuiLabel.h"
#include "GlUtils.h"
#include "../graphics/GlDeleteQueue.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/ResourceLib.h"
#include "../resources/ShaderResource.h"

using namespace base;
using namespace gui;
using namespace utils;
using namespace graphics;
using namespace resources;

GuiHud::GuiHud(GuiHudParams params) :
	GuiPanel(params)
{
	_guiParams.Texture = "";
	_guiParams.OverTexture = "";
	_guiParams.DownTexture = "";
	_guiParams.GuiPassThrough = true;
	SetPosition({ 0, 0 });
	_cableControlPoints.reserve((7u + 8u) * 4u);
	_cableColors.reserve(7u + 8u);
	_BuildPanels();
	SetSize(params.Size);
}

void GuiHud::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	if (_topAudioRow)
		_topAudioRow->ComputeLayout();
	if (_topMidiRow)
		_topMidiRow->ComputeLayout();
	if (_topStrip)
		_topStrip->ComputeLayout();
	if (_triggerRail)
		_triggerRail->ComputeLayout();

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3((float)pos.X, (float)pos.Y, 0.0f)));

	_DrawCables(ctx);

	for (auto& child : _children)
		child->Draw(ctx);

	glCtx.PopMvp();
}

void GuiHud::SetSize(Size2d size)
{
	GuiPanel::SetSize(size);
	_LayoutPanels();
}

void GuiHud::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto valid = _InitCableShader(resourceLib);
	if (valid)
		valid = _InitCableVertexArray();

	GlUtils::CheckError("GuiHud::_InitResources()");
}

void GuiHud::_ReleaseResources()
{
	graphics::GlDeleteQueue::DeleteBuffers(1, &_cableVertexBuffer);
	_cableVertexBuffer = 0;

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_cableVertexArray);
	_cableVertexArray = 0;
}

void GuiHud::_BuildPanels()
{
	GuiStackPanelParams topParams;
	topParams.Direction = StackDirection::Vertical;
	topParams.Spacing = _TopStripSpacing;
	topParams.PaddingH = _TopStripPadding;
	topParams.PaddingV = _TopStripPadding;
	topParams.Size = { _TopStripWidth, _TopStripHeight };
	topParams.MinSize = { _TopStripMinWidth, _TopStripHeight };
	// No background on the input strip — only the level elements themselves show.
	_topStrip = std::make_shared<GuiStackPanel>(topParams);
	AddChild(_topStrip);

	GuiStackPanelParams railParams;
	railParams.Direction = StackDirection::Vertical;
	railParams.Spacing = _RightRailSpacing;
	railParams.PaddingH = _RightRailPadding;
	railParams.PaddingV = _RightRailPadding;
	railParams.Size = { _RightRailWidth, _RightRailHeight };
	railParams.MinSize = { _RightRailWidth, _RightRailMinHeight };
	railParams.TextureShader = "texture_tinted";
	railParams.Texture = "rounded_but";
	railParams.TintColor = glm::vec3(0.17f, 0.20f, 0.24f);
	_triggerRail = std::make_shared<GuiStackPanel>(railParams);
	AddChild(_triggerRail);

	_BuildTopStrip();
	_BuildTriggerRail();
}

void GuiHud::_BuildTopStrip()
{
	_topStrip->AddChild(_MakeHeader("Inputs", _TopStripWidth - (_TopStripPadding * 2u)));

	GuiStackPanelParams audioRowParams = GuiStackPanelParams::PanelHorizontalRow(
		_TopStripWidth - (_TopStripPadding * 2u),
		_SourceButtonHeight);
	audioRowParams.WrapContent = true;
	_topAudioRow = std::make_shared<GuiStackPanel>(audioRowParams);

	const std::array<std::string, 4> audioInputs = {
		"Audio In 1",
		"Audio In 2",
		"Audio In 3",
		"Audio In 4"
	};

	for (const auto& input : audioInputs)
	{
		auto button = _MakeSourceButton(input, glm::vec3(0.92f, 0.52f, 0.24f));
		_sourceButtons.push_back(button);
		_topAudioRow->AddChild(button);
	}

	_topStrip->AddChild(_topAudioRow);

	GuiStackPanelParams midiRowParams = GuiStackPanelParams::PanelHorizontalRow(
		_TopStripWidth - (_TopStripPadding * 2u),
		_SourceButtonHeight);
	midiRowParams.WrapContent = true;
	_topMidiRow = std::make_shared<GuiStackPanel>(midiRowParams);

	const std::array<std::string, 3> midiInputs = {
		"MIDI Drum Pad",
		"MIDI Keys",
		"MIDI Foot Ctrl"
	};

	for (const auto& input : midiInputs)
	{
		auto button = _MakeSourceButton(input, glm::vec3(0.22f, 0.72f, 0.66f));
		_sourceButtons.push_back(button);
		_topMidiRow->AddChild(button);
	}

	_topStrip->AddChild(_topMidiRow);
}

void GuiHud::_BuildTriggerRail()
{
	_triggerRail->AddChild(_MakeHeader("Triggers", _RightRailWidth - (_RightRailPadding * 2u)));

	const std::array<std::pair<std::string, glm::vec3>, 6> triggers = {{
		{ "Activate A", glm::vec3(0.95f, 0.49f, 0.26f) },
		{ "Ditch A", glm::vec3(0.80f, 0.22f, 0.20f) },
		{ "Activate B", glm::vec3(0.96f, 0.70f, 0.27f) },
		{ "Overdub B", glm::vec3(0.22f, 0.65f, 0.44f) },
		{ "Punch In", glm::vec3(0.24f, 0.55f, 0.83f) },
		{ "Scene Ditch", glm::vec3(0.55f, 0.34f, 0.84f) }
	}};

	for (const auto& trigger : triggers)
	{
		auto button = _MakeTriggerButton(trigger.first, trigger.second);
		_triggerButtons.push_back(button);
		_triggerRail->AddChild(button);
	}
}

void GuiHud::_LayoutPanels()
{
	const unsigned int minViewWidth = _TopStripMinWidth + _RightRailWidth + 3u * static_cast<unsigned int>(_OuterMargin);
	const unsigned int minViewHeight = _TopPosY + _TopStripHeight + _RightRailMinHeight + 2u * static_cast<unsigned int>(_OuterMargin);
	const unsigned int viewWidth = std::max(_sizeParams.Size.Width, minViewWidth);
	const unsigned int viewHeight = std::max(_sizeParams.Size.Height, minViewHeight);
	const unsigned int topWidth = std::max(_TopStripMinWidth,
		std::min(_TopStripWidth, viewWidth - _RightRailWidth - 3u * static_cast<unsigned int>(_OuterMargin)));
	const unsigned int railHeight = std::max(_RightRailMinHeight,
		viewHeight - static_cast<unsigned int>(_TopPosY) - _TopStripHeight - 2u * static_cast<unsigned int>(_OuterMargin));

	_topStrip->SetPosition({ _OuterMargin, _TopPosY });
	_topStrip->SetSize({ topWidth, _TopStripHeight });

	if (_topAudioRow)
		_topAudioRow->SetSize({ topWidth - (_TopStripPadding * 2u), _SourceButtonHeight });
	if (_topMidiRow)
		_topMidiRow->SetSize({ topWidth - (_TopStripPadding * 2u), _SourceButtonHeight });

	const int railPosX = static_cast<int>(viewWidth) - static_cast<int>(_RightRailWidth) - _OuterMargin;
	const int railPosY = _TopPosY + static_cast<int>(_TopStripHeight) + _OuterMargin;
	_triggerRail->SetPosition({ railPosX, railPosY });
	_triggerRail->SetSize({ _RightRailWidth, railHeight });

	_cablesDirty = true;
}

bool GuiHud::_InitCableShader(ResourceLib& resourceLib)
{
	auto shaderOpt = resourceLib.GetResource("cable");
	if (!shaderOpt.has_value())
		return false;

	auto resource = shaderOpt.value().lock();
	if (!resource || (SHADER != resource->GetType()))
		return false;

	_cableShader = std::dynamic_pointer_cast<ShaderResource>(resource);
	return true;
}

bool GuiHud::_InitCableVertexArray()
{
	if (_cableVertexArray != 0)
		return true;

	glGenVertexArrays(1, &_cableVertexArray);
	glBindVertexArray(_cableVertexArray);

	glGenBuffers(1, &_cableVertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, _cableVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2u, nullptr, GL_DYNAMIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("GuiHud::_InitCableVertexArray()");
	return true;
}

void GuiHud::_DrawCables(base::DrawContext& ctx)
{
	auto shader = _cableShader.lock();
	if (!shader || (_cableVertexArray == 0) || (_cableVertexBuffer == 0))
		return;

	if (_cablesDirty)
		_RebuildCableVertices();

	if (_cableColors.empty())
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	const auto program = shader->GetId();
	glLineWidth(3.0f);
	glUseProgram(program);
	shader->SetUniforms(glCtx);
	glUniform4fv(glGetUniformLocation(program, "CableControlPoints"),
		static_cast<GLint>(_cableControlPoints.size()),
		reinterpret_cast<const GLfloat*>(_cableControlPoints.data()));
	glUniform4fv(glGetUniformLocation(program, "CableColors"),
		static_cast<GLint>(_cableColors.size()),
		reinterpret_cast<const GLfloat*>(_cableColors.data()));
	glUniform1i(glGetUniformLocation(program, "CableCount"), static_cast<GLint>(_cableColors.size()));
	glUniform1i(glGetUniformLocation(program, "SegmentCount"), _CableSegments);
	glBindVertexArray(_cableVertexArray);
	glDrawArraysInstanced(GL_LINE_STRIP, 0, _CableSegments, static_cast<GLsizei>(_cableColors.size()));
	glBindVertexArray(0);
	glUseProgram(0);
}

void GuiHud::SetStationAnchors(std::vector<StationAnchor> anchors)
{
	_stationAnchors = std::move(anchors);
	_cablesDirty = true;
}

void GuiHud::_RebuildCableVertices()
{
	_cableControlPoints.clear();
	_cableColors.clear();
	if (_sourceButtons.size() < 7u || _triggerButtons.size() < 6u)
		return;

	const std::array<std::tuple<std::size_t, std::size_t, glm::vec4>, 7> routes = {{
		{ 0u, 0u, glm::vec4(0.96f, 0.54f, 0.26f, 0.85f) },
		{ 1u, 1u, glm::vec4(0.86f, 0.29f, 0.20f, 0.82f) },
		{ 2u, 5u, glm::vec4(0.98f, 0.74f, 0.27f, 0.80f) },
		{ 3u, 4u, glm::vec4(0.30f, 0.61f, 0.93f, 0.78f) },
		{ 4u, 2u, glm::vec4(0.22f, 0.80f, 0.70f, 0.82f) },
		{ 5u, 3u, glm::vec4(0.18f, 0.71f, 0.46f, 0.82f) },
		{ 6u, 4u, glm::vec4(0.58f, 0.43f, 0.95f, 0.76f) }
	}};

	for (const auto& route : routes)
	{
		const auto sourceIndex = std::get<0>(route);
		const auto triggerIndex = std::get<1>(route);
		_AppendCurve(_ButtonCenter(_sourceButtons[sourceIndex]),
			_ButtonCenter(_triggerButtons[triggerIndex]),
			std::get<2>(route));
	}

	// Trigger -> station cables: one per trigger button if a station anchor exists.
	for (std::size_t i = 0u; i < _triggerButtons.size() && i < _stationAnchors.size(); ++i)
	{
		const auto& anchor = _stationAnchors[i];
		// Skip off-screen stations (sentinel value set when projected behind camera).
		if (anchor.screenPos.X < -1000)
			continue;
		_AppendStationCurve(
			_ButtonRightEdge(_triggerButtons[i]),
			anchor.screenPos,
			anchor.color);
	}

	_cablesDirty = false;
}

utils::Position2d GuiHud::_ButtonCenter(const std::shared_ptr<GuiButton>& button) const
{
	const auto rootPos = GlobalPosition();
	const auto buttonPos = button->GlobalPosition();
	const auto buttonSize = button->GetSize();
	return {
		buttonPos.X - rootPos.X + static_cast<int>(buttonSize.Width / 2u),
		buttonPos.Y - rootPos.Y + static_cast<int>(buttonSize.Height / 2u)
	};
}

utils::Position2d GuiHud::_ButtonRightEdge(const std::shared_ptr<GuiButton>& button) const
{
	const auto rootPos = GlobalPosition();
	const auto buttonPos = button->GlobalPosition();
	const auto buttonSize = button->GetSize();
	return {
		buttonPos.X - rootPos.X + static_cast<int>(buttonSize.Width),
		buttonPos.Y - rootPos.Y + static_cast<int>(buttonSize.Height / 2u)
	};
}

void GuiHud::_AppendCurve(const utils::Position2d& start,
	const utils::Position2d& end,
	const glm::vec4& color)
{
	const float x0 = static_cast<float>(start.X);
	const float y0 = static_cast<float>(start.Y);
	const float x3 = static_cast<float>(end.X);
	const float y3 = static_cast<float>(end.Y);

	// Departure tangent: leave the source button heading straight down.
	const float vertDrop = std::max(50.0f, (y3 - y0) * 0.55f);
	const float cp1x = x0;
	const float cp1y = y0 + vertDrop;

	// Arrival tangent: arrive at the trigger button heading straight right (from the left).
	const float horizPull = std::max(50.0f, (x3 - x0) * 0.40f);
	const float cp2x = x3 - horizPull;
	const float cp2y = y3;

	_cableControlPoints.push_back(glm::vec4(x0, y0, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(cp1x, cp1y, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(cp2x, cp2y, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(x3, y3, 0.0f, 0.0f));
	_cableColors.push_back(color);
}

void GuiHud::_AppendStationCurve(const utils::Position2d& start,
	const utils::Position2d& end,
	const glm::vec4& color)
{
	const float x0 = static_cast<float>(start.X);
	const float y0 = static_cast<float>(start.Y);
	const float x3 = static_cast<float>(end.X);
	const float y3 = static_cast<float>(end.Y);

	// Departure tangent: leave the trigger button horizontally toward the station.
	const float horizPull = std::max(50.0f, std::abs(x0 - x3) * 0.45f);
	const float cp1x = x0 - horizPull;
	const float cp1y = y0;

	// Arrival tangent: arrive at the station vertically from the trigger's Y-side.
	const float vertDrop = std::max(50.0f, std::abs(y0 - y3) * 0.45f);
	const float cp2x = x3;
	const float cp2y = y3 + (y0 < y3 ? -vertDrop : vertDrop);

	_cableControlPoints.push_back(glm::vec4(x0, y0, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(cp1x, cp1y, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(cp2x, cp2y, 0.0f, 0.0f));
	_cableControlPoints.push_back(glm::vec4(x3, y3, 0.0f, 0.0f));
	_cableColors.push_back(color);
}

std::shared_ptr<GuiLabel> GuiHud::_MakeHeader(const std::string& text, unsigned int width) const
{
	return std::make_shared<GuiLabel>(GuiLabelParams::PanelHeader(text, width));
}

std::shared_ptr<GuiButton> GuiHud::_MakeSourceButton(const std::string& text, const glm::vec3& tint) const
{
	auto buttonParams = GuiButtonParams::PanelButton(_SourceButtonWidth);
	buttonParams.Texture = "";
	buttonParams.OverTexture = "";
	buttonParams.DownTexture = "";
	buttonParams.Size = { _SourceButtonWidth, _SourceButtonHeight };
	buttonParams.MinSize = { 64u, _SourceButtonHeight };
	buttonParams.TintColor = tint;
	auto button = std::make_shared<GuiButton>(buttonParams);

	GuiLabelParams labelParams = GuiLabelParams::PanelScrollRow(text, 10u);
	labelParams.Position = { 0, 5 };
	labelParams.Size = { _SourceButtonWidth, GuiLabelParams::RowHeight };
	button->AddChild(std::make_shared<GuiLabel>(labelParams));
	return button;
}

std::shared_ptr<GuiButton> GuiHud::_MakeTriggerButton(const std::string& text, const glm::vec3& tint) const
{
	auto buttonParams = GuiButtonParams::PanelButton(_TriggerButtonWidth);
	buttonParams.Texture = "trigger_back";
	buttonParams.OverTexture = "trigger_back";
	buttonParams.DownTexture = "trigger_back";
	buttonParams.Size = { _TriggerButtonWidth, _TriggerButtonHeight };
	buttonParams.MinSize = { GuiButtonParams::DefaultMinWidth, _TriggerButtonHeight };
	buttonParams.TintColor = tint;
	auto button = std::make_shared<GuiButton>(buttonParams);

	const bool isDitch = (text.find("Ditch") != std::string::npos);
	base::GuiElementParams overlayParams;
	overlayParams.Position = { 10, 6 };
	overlayParams.Size = { 28, 28 };
	overlayParams.TextureShader = "texture_tinted";
	overlayParams.Texture = isDitch ? "trigger_ditch" : "trigger_activate";
	overlayParams.OverTexture = isDitch ? "trigger_ditch" : "trigger_activate";
	overlayParams.DownTexture = isDitch ? "trigger_ditch" : "trigger_activate";
	overlayParams.GuiPassThrough = true;
	overlayParams.TintColor = glm::vec3(1.0f, 1.0f, 1.0f);
	auto overlay = std::make_shared<base::GuiElement>(overlayParams);
	button->AddChild(overlay);

	GuiLabelParams labelParams = GuiLabelParams::PanelScrollRow(text, 12u);
	labelParams.Position = { 44, 5 };
	labelParams.Size = { _TriggerButtonWidth - 52u, GuiLabelParams::RowHeight };
	button->AddChild(std::make_shared<GuiLabel>(labelParams));
	return button;
}
