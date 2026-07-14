#include "GuiHud.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include "GuiButton.h"
#include "GuiLabel.h"
#include "GlUtils.h"
#include "../engine/Trigger.h"
#include "../graphics/GlDeleteQueue.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/ResourceLib.h"
#include "../resources/ShaderResource.h"

using namespace base;
using namespace gui;
using namespace utils;
using namespace graphics;
using namespace resources;

namespace gui
{
	class GuiHudTriggerBack : public GuiButton
	{
	public:
		GuiHudTriggerBack(GuiButtonParams params, std::weak_ptr<engine::Trigger> trigger) :
			GuiButton(params),
			_trigger(std::move(trigger))
		{
		}

		virtual void Draw(base::DrawContext& ctx) override
		{
			const auto previousTint = _guiParams.TintColor;
			_guiParams.TintColor = _TriggerTint();
			GuiButton::Draw(ctx);
			_guiParams.TintColor = previousTint;
		}

	private:
		glm::vec3 _TriggerTint() const
		{
			const auto trigger = _trigger.lock();
			if (!trigger)
				return { 0.92f, 0.92f, 0.92f };
			if (trigger->IsDitchDown())
				return { 0.24f, 0.68f, 0.98f };

			switch (trigger->GetState())
			{
			case engine::TRIGSTATE_RECORDING: return { 0.94f, 0.20f, 0.22f };
			case engine::TRIGSTATE_OVERDUBBING: return { 0.95f, 0.94f, 0.07f };
			case engine::TRIGSTATE_PUNCHEDIN: return { 0.70f, 0.30f, 0.92f };
			default: return { 0.92f, 0.92f, 0.92f };
			}
		}

		std::weak_ptr<engine::Trigger> _trigger;
	};

	class GuiHudTriggerPedal : public base::GuiElement
	{
	public:
		GuiHudTriggerPedal(base::GuiElementParams params,
			std::weak_ptr<engine::Trigger> trigger,
			bool isActivate) :
			GuiElement(params),
			_trigger(std::move(trigger)),
			_isActivate(isActivate)
		{
		}

		virtual void Draw(base::DrawContext& ctx) override
		{
			const auto trigger = _trigger.lock();
			const bool inputDown = trigger && (_isActivate ? trigger->IsActivateInputDown() : trigger->IsDitchInputDown());
			const auto pointerState = _state;
			_state = inputDown || pointerState == STATE_DOWN ? STATE_DOWN : pointerState;
			GuiElement::Draw(ctx);
			_state = pointerState;
		}

		virtual actions::ActionResult OnAction(actions::TouchAction action) override
		{
			if (!_isEnabled || !_isVisible)
				return actions::ActionResult::NoAction();

			if ((actions::TouchAction::TOUCH_DOWN == action.State) && HitTest(action.Position))
			{
				_state = STATE_DOWN;
				return _Dispatch(action);
			}

			if (actions::TouchAction::TOUCH_UP == action.State)
			{
				_state = HitTest(action.Position) ? STATE_OVER : STATE_NORMAL;
				return _Dispatch(action);
			}

			return actions::ActionResult::NoAction();
		}

	private:
		actions::ActionResult _Dispatch(const actions::TouchAction& action)
		{
			if (auto trigger = _trigger.lock())
			{
				auto result = trigger->QueueExternalControlAction(_isActivate,
					actions::TouchAction::TOUCH_DOWN == action.State,
					action);
				result.ActiveElement = std::static_pointer_cast<base::GuiElement>(shared_from_this());
				return result;
			}

			return actions::ActionResult::NoAction();
		}

		std::weak_ptr<engine::Trigger> _trigger;
		bool _isActivate;
	};
}

GuiHud::GuiHud(GuiHudParams params) :
	GuiPanel(params)
{
	_guiParams.Texture = "";
	_guiParams.OverTexture = "";
	_guiParams.DownTexture = "";
	_guiParams.GuiPassThrough = true;
	SetPosition({ 0, 0 });
	_cableControlPoints.reserve((12u + 8u) * 4u);
	_cableColors.reserve(12u + 8u);
	_cableRenderColors.reserve(12u + 8u);
	_BuildPanels();
	SetSize(params.Size);
}

void GuiHud::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	if (_topInputRow)
		_topInputRow->ComputeLayout();
	if (_topStrip)
		_topStrip->ComputeLayout();
	if (_triggerRail)
		_triggerRail->ComputeLayout();

	for (std::size_t i = 0u; i < _audioInputVus.size() && i < _sourceButtons.size(); ++i)
	{
		const auto buttonPos = _sourceButtons[i]->GlobalPosition();
		const auto buttonSize = _sourceButtons[i]->GetSize();
		const auto rootPos = GlobalPosition();
		_audioInputVus[i]->SetPosition({ buttonPos.X - rootPos.X + static_cast<int>(buttonSize.Width) - 13,
			buttonPos.Y - rootPos.Y + 4 });
		_audioInputVus[i]->SetSize({ 7u, _SourceButtonHeight - 8u });
	}

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3((float)pos.X, (float)pos.Y, 0.0f)));

	_DrawCables(ctx);

	for (auto& child : _children)
		child->Draw(ctx);

	for (auto& vu : _audioInputVus)
		vu->Draw(ctx);

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
	for (auto& vu : _audioInputVus)
		vu->InitResources(resourceLib, forceInit);

	GlUtils::CheckError("GuiHud::_InitResources()");
}

void GuiHud::_ReleaseResources()
{
	graphics::GlDeleteQueue::DeleteBuffers(1, &_cableVertexBuffer);
	_cableVertexBuffer = 0;

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_cableVertexArray);
	_cableVertexArray = 0;

	for (auto& vu : _audioInputVus)
		vu->ReleaseResources();
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
	railParams.PaddingH = _RightRailPaddingH;
	railParams.PaddingV = _RightRailPaddingV;
	railParams.Size = { _RightRailWidth - 6u, _RightRailHeight };
	railParams.MinSize = { _RightRailWidth - 6u, _RightRailMinHeight };
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

	GuiStackPanelParams inputRowParams = GuiStackPanelParams::PanelHorizontalRow(
		_TopStripWidth - (_TopStripPadding * 2u),
		_SourceButtonHeight);
	inputRowParams.WrapContent = false;
	_topInputRow = std::make_shared<GuiStackPanel>(inputRowParams);

	const unsigned int totalInputs = _audioInputCount + static_cast<unsigned int>(_midiInputNames.size());
	const unsigned int innerWidth = _TopStripWidth - (_TopStripPadding * 2u);
	const unsigned int totalSpacing = totalInputs > 1u ? GuiStackPanelParams::PanelRowSpacing * (totalInputs - 1u) : 0u;
	const unsigned int widthBudget = innerWidth > totalSpacing ? innerWidth - totalSpacing : innerWidth;
	const unsigned int sourceButtonWidth = totalInputs > 0u ? std::max(64u, widthBudget / totalInputs) : _SourceButtonWidth;

	for (auto i = 0u; i < _audioInputCount; ++i)
	{
		auto button = _MakeSourceButton("Audio In " + std::to_string(i + 1u), glm::vec3(0.92f, 0.52f, 0.24f), sourceButtonWidth);
		_sourceButtons.push_back(button);
		_topInputRow->AddChild(button);
		_audioInputVus.push_back(std::make_unique<GuiVu>());
	}

	for (const auto& midiName : _midiInputNames)
	{
		auto button = _MakeSourceButton("MIDI " + midiName, glm::vec3(0.22f, 0.72f, 0.66f), sourceButtonWidth);
		_sourceButtons.push_back(button);
		_topInputRow->AddChild(button);
	}

	_topStrip->AddChild(_topInputRow);
}

void GuiHud::_BuildTriggerRail()
{
	if (_triggerNames.empty())
		return;

	_triggerRail->AddChild(_MakeHeader("Triggers", _RightRailWidth, 38u));
	for (std::size_t i = 0u; i < _triggerNames.size(); ++i)
	{
		auto button = _MakeTriggerButton(_triggerNames[i],
			i < _triggers.size() ? _triggers[i] : std::weak_ptr<engine::Trigger>());
		_triggerButtons.push_back(button);
		_triggerRail->AddChild(button);
	}
}

void GuiHud::_RebuildPanels()
{
	_sourceButtons.clear();
	_triggerButtons.clear();
	_audioInputVus.clear();
	_topStrip.reset();
	_topInputRow.reset();
	_triggerRail.reset();
	_children.clear();

	_BuildPanels();
	_LayoutPanels();
	_cablesDirty = true;
}

void GuiHud::SetCableRevealHeld(bool held)
{
	_cableRevealHeld = held;
}

void GuiHud::SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps)
{
	if (channel < _audioInputVus.size())
		_audioInputVus[channel]->SetPeak(peak, numSamps);
}

void GuiHud::SetRoutingConfig(unsigned int audioInputCount,
	std::vector<std::string> midiInputNames,
	std::vector<std::shared_ptr<engine::Trigger>> triggers)
{
	_audioInputCount = audioInputCount;
	_midiInputNames.clear();
	for (auto& name : midiInputNames)
		if (!name.empty())
			_midiInputNames.push_back(std::move(name));

	_triggers.clear();
	_triggerNames.clear();
	for (const auto& trigger : triggers)
	{
		if (trigger && !trigger->Name().empty())
		{
			_triggers.push_back(trigger);
			_triggerNames.push_back(trigger->Name());
		}
	}
	_RebuildPanels();
}

void GuiHud::_LayoutPanels()
{
	const unsigned int minViewWidth = _TopStripMinWidth + _RightRailWidth + 3u * static_cast<unsigned int>(_OuterMargin);
	const unsigned int minViewHeight = std::max(_TopStripHeight, _RightRailMinHeight) +
		2u * static_cast<unsigned int>(_OuterMargin + _TopPosY);
	const unsigned int viewWidth = std::max(_sizeParams.Size.Width, minViewWidth);
	const unsigned int viewHeight = std::max(_sizeParams.Size.Height, minViewHeight);
	const unsigned int topWidth = std::max(_TopStripMinWidth,
		std::min(_TopStripWidth, viewWidth - _RightRailWidth - 3u * static_cast<unsigned int>(_OuterMargin)));
	const unsigned int railHeight = std::max(_RightRailMinHeight,
		viewHeight - 2u * static_cast<unsigned int>(_OuterMargin + _TopPosY));

	const int railPosX = static_cast<int>(viewWidth) - static_cast<int>(_RightRailWidth) - _OuterMargin + _RightRailOverhang;
	const int topPosX = std::max(_OuterMargin, railPosX - _OuterMargin - static_cast<int>(topWidth));
	const int topPosY = static_cast<int>(viewHeight) - static_cast<int>(_TopStripHeight) - _TopPosY;
	_topStrip->SetPosition({ topPosX, topPosY });
	_topStrip->SetSize({ topWidth, _TopStripHeight });

	if (_topInputRow)
		_topInputRow->SetSize({ topWidth - (_TopStripPadding * 2u), _SourceButtonHeight });

	const int railPosY = static_cast<int>(viewHeight) - static_cast<int>(railHeight) - _TopPosY + 42u;
	_triggerRail->SetPosition({ railPosX, railPosY });
	_triggerRail->SetSize({ _RightRailWidth - 6u, railHeight - _RightRailTopInset });

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
	const float fadeInStep = 0.16f;
	const float fadeOutStep = 0.08f;
	if (_cableRevealHeld)
		_cableRevealAlpha = std::min(1.0f, _cableRevealAlpha + fadeInStep);
	else
		_cableRevealAlpha = std::max(0.0f, _cableRevealAlpha - fadeOutStep);

	if (_cableRevealAlpha <= 0.001f)
		return;

	auto shader = _cableShader.lock();
	if (!shader || (_cableVertexArray == 0) || (_cableVertexBuffer == 0))
		return;

	if (_cablesDirty)
		_RebuildCableVertices();

	if (_cableColors.empty())
		return;

	_cableRenderColors = _cableColors;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	const auto program = shader->GetId();
	glUseProgram(program);
	shader->SetUniforms(glCtx);
	glUniform4fv(glGetUniformLocation(program, "CableControlPoints"),
		static_cast<GLint>(_cableControlPoints.size()),
		reinterpret_cast<const GLfloat*>(_cableControlPoints.data()));
	glUniform1i(glGetUniformLocation(program, "CableCount"), static_cast<GLint>(_cableColors.size()));
	glUniform1i(glGetUniformLocation(program, "SegmentCount"), _CableSegments);
	glBindVertexArray(_cableVertexArray);

	const auto colorUniform = glGetUniformLocation(program, "CableColors");
	const auto drawPass = [&](float width, float brightness, float alphaMultiplier)
	{
		for (std::size_t i = 0u; i < _cableColors.size(); ++i)
		{
			const auto& color = _cableColors[i];
			_cableRenderColors[i] = glm::vec4(
				std::clamp(color.r * brightness, 0.0f, 1.0f),
				std::clamp(color.g * brightness, 0.0f, 1.0f),
				std::clamp(color.b * brightness, 0.0f, 1.0f),
				std::clamp(color.a * alphaMultiplier * _cableRevealAlpha, 0.0f, 1.0f));
		}

		glLineWidth(width);
		glUniform4fv(colorUniform,
			static_cast<GLint>(_cableRenderColors.size()),
			reinterpret_cast<const GLfloat*>(_cableRenderColors.data()));
		glDrawArraysInstanced(GL_LINE_STRIP, 0, _CableSegments,
			static_cast<GLsizei>(_cableRenderColors.size()));
	};

	drawPass(6.5f, 0.52f, 0.95f);
	drawPass(3.2f, 1.08f, 1.00f);

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
	if (_sourceButtons.empty() || _triggerButtons.empty())
	{
		_cablesDirty = false;
		return;
	}

	const glm::vec4 inputToTriggerColor(0.86f, 0.24f, 0.26f, 0.90f);
	const glm::vec4 triggerToStationColor(0.92f, 0.79f, 0.20f, 0.88f);
	for (std::size_t sourceIndex = 0u; sourceIndex < _sourceButtons.size(); ++sourceIndex)
	{
		const auto triggerIndex = sourceIndex % _triggerButtons.size();
		_AppendCurve(_ButtonCenter(_sourceButtons[sourceIndex]),
			_TriggerAnchorFromTopLeft(_triggerButtons[triggerIndex], 7, 12),
			inputToTriggerColor);
	}

	// Trigger -> station cables: one per trigger button if a station anchor exists.
	for (std::size_t i = 0u; i < _triggerButtons.size() && i < _stationAnchors.size(); ++i)
	{
		const auto& anchor = _stationAnchors[i];
		// Skip off-screen stations (sentinel value set when projected behind camera).
		if (anchor.screenPos.X < -1000)
			continue;
		_AppendStationCurve(
			_TriggerAnchorFromBottomLeft(_triggerButtons[i], 7, 12),
			anchor.screenPos,
			triggerToStationColor);
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

utils::Position2d GuiHud::_TriggerAnchorFromTopLeft(const std::shared_ptr<GuiButton>& button,
	int offsetX,
	int offsetFromTopY) const
{
	const auto rootPos = GlobalPosition();
	const auto buttonPos = button->GlobalPosition();
	const auto buttonSize = button->GetSize();
	const int clampedX = std::clamp(offsetX, 0, static_cast<int>(buttonSize.Width));
	const int yFromBottom = std::clamp(static_cast<int>(buttonSize.Height) - offsetFromTopY,
		0,
		static_cast<int>(buttonSize.Height));
	return {
		buttonPos.X - rootPos.X + clampedX,
		buttonPos.Y - rootPos.Y + yFromBottom
	};
}

utils::Position2d GuiHud::_TriggerAnchorFromBottomLeft(const std::shared_ptr<GuiButton>& button,
	int offsetX,
	int offsetFromBottomY) const
{
	const auto rootPos = GlobalPosition();
	const auto buttonPos = button->GlobalPosition();
	const auto buttonSize = button->GetSize();
	const int clampedX = std::clamp(offsetX, 0, static_cast<int>(buttonSize.Width));
	const int clampedY = std::clamp(offsetFromBottomY, 0, static_cast<int>(buttonSize.Height));
	return {
		buttonPos.X - rootPos.X + clampedX,
		buttonPos.Y - rootPos.Y + clampedY
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

std::shared_ptr<GuiLabel> GuiHud::_MakeHeader(const std::string& text,
	unsigned int width,
	unsigned int horizontalInset) const
{
	auto params = GuiLabelParams::PanelHeader(text, width);
	params.TextInsetX = static_cast<int>(horizontalInset);
	return std::make_shared<GuiLabel>(params);
}

std::shared_ptr<GuiButton> GuiHud::_MakeSourceButton(const std::string& text,
	const glm::vec3& tint,
	unsigned int width) const
{
	auto buttonParams = GuiButtonParams::PanelButton(width);
	buttonParams.Texture = "rounded_but";
	buttonParams.OverTexture = "rounded_but";
	buttonParams.DownTexture = "rounded_but";
	buttonParams.Size = { width, _SourceButtonHeight };
	buttonParams.MinSize = { 64u, _SourceButtonHeight };
	buttonParams.TintColor = glm::vec3(0.02f, 0.02f, 0.02f);
	auto button = std::make_shared<GuiButton>(buttonParams);

	auto trim = [](std::string value)
	{
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
			value.erase(value.begin());
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
			value.pop_back();
		return value;
	};

	const std::size_t maxLineChars = 8u;
	std::string line1 = text;
	std::string line2;
	if (text.size() > maxLineChars)
	{
		std::size_t split = text.rfind(' ', maxLineChars);
		if (split == std::string::npos || split == 0u)
			split = maxLineChars;
		line1 = trim(text.substr(0, split));
		line2 = trim(text.substr(split));
		if (line2.size() > maxLineChars)
			line2 = trim(line2.substr(0, maxLineChars));
	}

	GuiLabelParams line1Params = GuiLabelParams::PanelScrollRow(line1, 0u);
	line1Params.Position = { 4, 18 };
	line1Params.Size = { width - 8u, 14u };
	button->AddChild(std::make_shared<GuiLabel>(line1Params));

	if (!line2.empty())
	{
		GuiLabelParams line2Params = GuiLabelParams::PanelScrollRow(line2, 0u);
		line2Params.Position = { 4, 4 };
		line2Params.Size = { width - 8u, 14u };
		button->AddChild(std::make_shared<GuiLabel>(line2Params));
	}
	return button;
}

std::shared_ptr<GuiButton> GuiHud::_MakeTriggerButton(const std::string& text,
	std::weak_ptr<engine::Trigger> trigger) const
{
	auto buttonParams = GuiButtonParams::PanelButton(_TriggerButtonWidth);
	buttonParams.Texture = "trigger_back";
	buttonParams.OverTexture = "trigger_back";
	buttonParams.DownTexture = "trigger_back";
	buttonParams.TextureShader = "texture_tinted";
	buttonParams.Size = { _TriggerButtonWidth, _TriggerButtonHeight };
	buttonParams.MinSize = { GuiButtonParams::DefaultMinWidth, _TriggerButtonHeight };
	auto button = std::make_shared<GuiHudTriggerBack>(buttonParams, trigger);

	const int socketPadding = 4;
	const int pedalSizeW = static_cast<int>(_TriggerButtonWidth * 0.45) - socketPadding;
	const int pedalSizeH = static_cast<int>(_TriggerButtonHeight * 0.70f);
	const int pedalPosX = 16;
	const int pedalPosY = 14;

	base::GuiElementParams activateParams;
	activateParams.Position = { pedalPosX, pedalPosY };
	activateParams.Size = { static_cast<unsigned int>(pedalSizeW), static_cast<unsigned int>(pedalSizeH) };
	activateParams.TextureShader = "texture";
	activateParams.Texture = "trigger_activate";
	activateParams.OverTexture = "trigger_activate_over";
	activateParams.DownTexture = "trigger_activate_down";
	activateParams.OutTexture = "trigger_activate_down_out";
	activateParams.GuiPassThrough = false;
	button->AddChild(std::make_shared<GuiHudTriggerPedal>(activateParams, trigger, true));

	base::GuiElementParams ditchParams;
	ditchParams.Position = { pedalPosX + pedalSizeW + socketPadding, pedalPosY };
	ditchParams.Size = { static_cast<unsigned int>(pedalSizeW), static_cast<unsigned int>(pedalSizeH) };
	ditchParams.TextureShader = "texture";
	ditchParams.Texture = "trigger_ditch";
	ditchParams.OverTexture = "trigger_ditch_over";
	ditchParams.DownTexture = "trigger_ditch_down";
	ditchParams.OutTexture = "trigger_ditch_down_out";
	ditchParams.GuiPassThrough = false;
	button->AddChild(std::make_shared<GuiHudTriggerPedal>(ditchParams, std::move(trigger), false));

	GuiLabelParams labelParams = GuiLabelParams::PanelScrollRow(text, 12u);
	const int approxCharWidth = 8;
	const unsigned int textWidth = std::min(_TriggerButtonWidth - 16u,
		static_cast<unsigned int>(std::max(40, static_cast<int>(text.size()) * approxCharWidth + 8)));
	const int textPosX = std::max(0, (static_cast<int>(_TriggerButtonWidth) - static_cast<int>(textWidth)) / 2);
	labelParams.Position = { textPosX, static_cast<int>(_TriggerButtonHeight) - static_cast<int>(GuiLabelParams::RowHeight) + 12 };
	labelParams.Size = { textWidth, GuiLabelParams::RowHeight };
	button->AddChild(std::make_shared<GuiLabel>(labelParams));
	return button;
}
