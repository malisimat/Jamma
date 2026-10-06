#include "GuiHud.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include "GuiButton.h"
#include "GuiLabel.h"
#include "GuiPopup.h"
#include "GuiPopupManager.h"
#include "GuiScrollPanel.h"
#include "GlUtils.h"
#include "../engine/Trigger.h"
#include "../engine/RigSnapshot.h"
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
	class GuiHudStatusPanel : public GuiPanel
	{
	public:
		explicit GuiHudStatusPanel(base::GuiElementParams params) : GuiPanel(params) {}
		void Draw(base::DrawContext& ctx) override
		{
			auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
			glCtx.SetUniform("Color", glm::vec4(GuiStyle::Graphite(), 1.0f));
			GuiPanel::Draw(ctx);
		}
	};
	class GuiHudSocket : public base::GuiElement
	{
	public:
		GuiHudSocket(utils::Position2d position, unsigned int size, const glm::vec3& tint) :
			GuiElement(_Params(position, size, tint)), _tint(tint)
		{
		}

		void SetHighlighted(bool highlighted)
		{
			_highlighted = highlighted;
		}

		void SetTint(const glm::vec3& tint)
		{
			_tint = tint;
			_guiParams.TintColor = tint;
		}

		void Draw(base::DrawContext& ctx) override
		{
			const auto previousTint = _guiParams.TintColor;
			if (_highlighted)
				_guiParams.TintColor = glm::min(_tint * 1.35f + glm::vec3(0.08f), glm::vec3(1.0f));
			GuiElement::Draw(ctx);
			_guiParams.TintColor = previousTint;
		}

	private:
		static base::GuiElementParams _Params(utils::Position2d position, unsigned int size, const glm::vec3& tint)
		{
			base::GuiElementParams params;
			params.Position = position;
			params.Size = { size, size };
			params.MinSize = params.Size;
			params.Texture = size <= 10u ? "hud_socket_cable" : "hud_socket_fat";
			params.TextureShader = "texture_tinted";
			params.TintColor = tint;
			params.GuiPassThrough = true;
			return params;
		}

		glm::vec3 _tint;
		bool _highlighted = false;
	};
	class GuiHudActionButton : public GuiButton
	{
	public:
		GuiHudActionButton(GuiButtonParams params, std::function<void()> callback) :
			GuiButton(_Params(std::move(params))), _callback(std::move(callback)) {}
		void ClearPointerState() override
		{
			GuiButton::ClearPointerState();
			_pressed = false;
		}

		void Draw(base::DrawContext& ctx) override
		{
			const auto previousTint = _guiParams.TintColor;
			_guiParams.TintColor = IsEnabled() ? previousTint : previousTint * 0.38f;
			GuiButton::Draw(ctx);
			_guiParams.TintColor = previousTint;
		}

		actions::ActionResult OnAction(actions::TouchAction action) override
		{
			if (!IsEnabled() || !IsVisible())
			{
				_pressed = false;
				return actions::ActionResult::NoAction();
			}
			auto result = GuiButton::OnAction(action);
			if (action.State == actions::TouchAction::TOUCH_DOWN && result.IsEaten)
			{
				_pressed = true;
				result.ActiveElement = std::static_pointer_cast<base::GuiElement>(shared_from_this());
			}
			else if (action.State == actions::TouchAction::TOUCH_UP)
			{
				const bool invoke = _pressed && result.IsEaten && HitTest(action.Position);
				_pressed = false;
				if (invoke && _callback)
					_callback();
			}
			return result;
		}

	private:
		static GuiButtonParams _Params(GuiButtonParams params)
		{
			params.TextureShader = "texture_tinted";
			return params;
		}

		std::function<void()> _callback;
		bool _pressed = false;
	};

	class GuiHudPopupReceiver : public base::ActionReceiver
	{
	public:
		explicit GuiHudPopupReceiver(std::function<void(unsigned int)> callback) : _callback(std::move(callback)) {}
		actions::ActionResult OnAction(actions::GuiAction action) override
		{
			if (_callback) _callback(action.Index);
			return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	private:
		std::function<void(unsigned int)> _callback;
	};

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
			bool isActivate,
			std::uint64_t rigRevision,
			std::function<bool(std::uint64_t)> acceptInput) :
			GuiElement(params),
			_trigger(std::move(trigger)),
			_isActivate(isActivate),
			_rigRevision(rigRevision),
			_acceptInput(std::move(acceptInput))
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
			if (!_acceptInput || !_acceptInput(_rigRevision))
				return actions::ActionResult::NoAction();
			if (auto trigger = _trigger.lock())
			{
				auto result = trigger->QueueExternalControlAction(_isActivate,
					actions::TouchAction::TOUCH_DOWN == action.State,
					action,
					_rigRevision);
				result.ActiveElement = std::static_pointer_cast<base::GuiElement>(shared_from_this());
				return result;
			}

			return actions::ActionResult::NoAction();
		}

		std::weak_ptr<engine::Trigger> _trigger;
		bool _isActivate;
		std::uint64_t _rigRevision;
		std::function<bool(std::uint64_t)> _acceptInput;
	};
}

GuiHud::GuiHud(GuiHudParams params) :
	GuiPanel(params),
	_submitRigEdit(std::move(params.SubmitRigEdit)),
	_routingEditAvailability(std::move(params.RoutingEditAvailabilityState)),
	_acceptTriggerInput(std::move(params.AcceptTriggerInput)),
	_popupManager(params.PopupManager)
{
	_cableEndIcon = std::make_shared<GuiHudSocket>(utils::Position2d{}, _CableEndSize, glm::vec3(1.0f));
	_stationSocketIcon = std::make_shared<GuiHudSocket>(utils::Position2d{}, _SocketSize, glm::vec3(0.92f, 0.79f, 0.20f));
	_guiParams.Texture = "";
	_guiParams.OverTexture = "";
	_guiParams.DownTexture = "";
	_guiParams.GuiPassThrough = true;
	SetPosition({ 0, 0 });
	_cableControlPoints.reserve((12u + 8u) * 4u);
	_cableColors.reserve(12u + 8u);
	_cableRenderColors.reserve(12u + 8u);
	_deletePopup = std::make_shared<GuiPopup>(GuiPopupParams::PanelDefault());
	GuiPopupButtonConfig deleteConfig;
	deleteConfig.Actions = { { "Cancel", 2u }, { "Delete", 1u } };
	_deletePopup->ConfigureButtons(deleteConfig);
	_deletePopupReceiver = std::make_shared<GuiHudPopupReceiver>([this](unsigned int index)
	{
		if (index == 1u) _ConfirmDelete();
		else { _deleteTriggerIndex.reset(); if (_popupManager) _popupManager->Close(); }
	});
	_deletePopup->SetButtonReceiver(_deletePopupReceiver);
	GuiElementParams infoParams;
	infoParams.Texture = "rounded_but_on";
	infoParams.TextureShader = "texture_tinted";
	infoParams.TintColor = GuiStyle::Graphite();
	infoParams.TextureOpacity = GuiStyle::PanelFillOpacity;
	infoParams.GuiPassThrough = false;
	_sourceInfoPanel = std::make_shared<GuiPanel>(infoParams);
	auto infoHeader = GuiLabelParams::PanelHeader("Input identity (scroll to read; Esc closes)", 420u);
	infoHeader.Ellipsize = true;
	_sourceInfoPanel->AddChild(std::make_shared<GuiLabel>(infoHeader));
	GuiScrollPanelParams infoScroll;
	infoScroll.Orientation = GuiScrollOrientation::Horizontal;
	infoScroll.ScrollBarWidth = 12u;
	_sourceInfoScroll = std::make_shared<GuiScrollPanel>(infoScroll);
	GuiLabelParams identityParams;
	identityParams.Size = { 1u, 28u };
	identityParams.TextInsetX = 2;
	identityParams.TextInsetY = 2;
	_sourceInfoLabel = std::make_shared<GuiLabel>(identityParams);
	_sourceInfoScroll->SetContent(_sourceInfoLabel);
	_sourceInfoPanel->AddChild(_sourceInfoScroll);
	_BuildPanels();
	SetSize(params.Size);
}

void GuiHud::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	if (_topSourceRow)
		_topSourceRow->ComputeLayout();
	if (_topInputRow)
		_topInputRow->ComputeLayout();
	if (_topMidiRow)
		_topMidiRow->ComputeLayout();
	if (_triggerList)
		_triggerList->ComputeLayout();
	const int audioOffset = _topAudioScroll ? _topAudioScroll->ScrollOffset() : 0;
	const int midiOffset = _topMidiScroll ? _topMidiScroll->ScrollOffset() : 0;
	if (_lastAudioScrollOffset != audioOffset || _lastMidiScrollOffset != midiOffset)
	{
		_lastAudioScrollOffset = audioOffset;
		_lastMidiScrollOffset = midiOffset;
		_cablesDirty = true;
	}
	if (_triggerScroll && _lastTriggerScrollOffset != _triggerScroll->ScrollOffset())
	{
		_lastTriggerScrollOffset = _triggerScroll->ScrollOffset();
		_cablesDirty = true;
	}
	_UpdateRoutingEditPresentation();

	for (std::size_t i = 0u; i < _inputVus.size() && i < _sourceWidgets.size(); ++i)
	{
		const auto buttonPos = _sourceWidgets[i].Button->GlobalPosition();
		const auto buttonSize = _sourceWidgets[i].Button->GetSize();
		const auto rootPos = GlobalPosition();
		_inputVus[i]->SetPosition({ buttonPos.X - rootPos.X + static_cast<int>(buttonSize.Width) - 13,
			buttonPos.Y - rootPos.Y + 4 });
		_inputVus[i]->SetSize({ 7u, _SourceButtonHeight - 8u });
	}

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3((float)pos.X, (float)pos.Y, 0.0f)));

	for (auto& child : _children)
		child->Draw(ctx);

	_DrawCables(ctx);

	const auto drawSourceOverlays = [this, &ctx, &glCtx](const std::shared_ptr<GuiScrollPanel>& scroll,
		io::RigFileRouting::SourceKind kind)
	{
		if (!scroll)
			return;
		const auto clip = _ContentClip(scroll);
		glCtx.PushScissorRect({ clip.Left, clip.Bottom }, {
			static_cast<unsigned int>(clip.Right - clip.Left), static_cast<unsigned int>(clip.Top - clip.Bottom) });
		for (size_t i = 0u; i < _sourceWidgets.size(); ++i)
		{
			if (_sourceEndpoints[i].Kind != kind)
				continue;
			_inputVus[i]->Draw(ctx);
			_DrawOverlayElement(ctx, _sourceWidgets[i].Socket);
		}
		glCtx.PopScissorRect();
	};
	drawSourceOverlays(_topAudioScroll, io::RigFileRouting::SourceKind::Adc);
	drawSourceOverlays(_topMidiScroll, io::RigFileRouting::SourceKind::Midi);
	if (_triggerScroll)
	{
		const auto clip = _ContentClip(_triggerScroll);
		glCtx.PushScissorRect({ clip.Left, clip.Bottom }, {
			static_cast<unsigned int>(clip.Right - clip.Left), static_cast<unsigned int>(clip.Top - clip.Bottom) });
		for (const auto& widgets : _triggerWidgets)
		{
			_DrawOverlayElement(ctx, widgets.Close);
			_DrawOverlayElement(ctx, widgets.InputSocket);
			_DrawOverlayElement(ctx, widgets.OutputSocket);
		}
		glCtx.PopScissorRect();
	}
	_DrawCableSockets(ctx);

	glCtx.PopMvp();
}

void GuiHud::SetSize(Size2d size)
{
	GuiPanel::SetSize(size);
	if (_deletePopup) _deletePopup->FitToViewport(size);
	_LayoutPanels();
	if (_popupManager && _popupManager->Top() == _sourceInfoPanel)
		_LayoutSourceIdentity();
}

void GuiHud::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto valid = _InitCableShader(resourceLib);
	if (valid)
		valid = _InitCableVertexArray();
	for (auto& vu : _inputVus)
		vu->InitResources(resourceLib, forceInit);
	if (_deletePopup)
		_deletePopup->InitResources(resourceLib, forceInit);
	_sourceInfoPanel->InitResources(resourceLib, forceInit);
	_LayoutSourceIdentity();
	_cableEndIcon->InitResources(resourceLib, forceInit);
	_stationSocketIcon->InitResources(resourceLib, forceInit);

	GlUtils::CheckError("GuiHud::_InitResources()");
}

void GuiHud::_ReleaseResources()
{
	GuiPanel::_ReleaseResources();
	graphics::GlDeleteQueue::DeleteBuffers(1, &_cableVertexBuffer);
	_cableVertexBuffer = 0;

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_cableVertexArray);
	_cableVertexArray = 0;

	for (auto& vu : _inputVus)
		vu->ReleaseResources();
	if (_deletePopup)
		_deletePopup->ReleaseResources();
	_sourceInfoPanel->ReleaseResources();
	_cableEndIcon->ReleaseResources();
	_stationSocketIcon->ReleaseResources();
}

void GuiHud::_BuildPanels()
{
	GuiElementParams topParams;
	topParams.Size = { _TopStripWidth, _TopStripHeight };
	topParams.MinSize = { _TopStripMinWidth, _TopStripHeight };
	topParams.Texture = "rounded_but_on";
	topParams.TextureShader = "texture_tinted";
	topParams.TintColor = GuiStyle::Graphite();
	topParams.TextureOpacity = 0.35f;
	_topStrip = std::make_shared<GuiPanel>(topParams);
	AddChild(_topStrip);

	base::GuiElementParams railParams;
	railParams.Size = { _RightRailWidth, _RightRailHeight };
	railParams.MinSize = { _RightRailWidth, _RightRailMinHeight };
	railParams.TextureShader = "texture_tinted";
	railParams.Texture = "rounded_but_on";
	railParams.TintColor = GuiStyle::Graphite();
	railParams.TextureOpacity = GuiStyle::PanelFillOpacity;
	_triggerRail = std::make_shared<GuiPanel>(railParams);
	AddChild(_triggerRail);

	_BuildTopStrip();
	_BuildTriggerRail();
}

void GuiHud::_BuildTopStrip()
{
	_topStrip->AddChild(_MakeHeader("Inputs", _TopStripWidth - (_TopStripPadding * 2u)));

	const auto audioCount = static_cast<unsigned int>(std::count_if(_sourceEndpoints.begin(), _sourceEndpoints.end(),
		[](const auto& source) { return source.Kind == io::RigFileRouting::SourceKind::Adc; }));
	const auto midiCount = static_cast<unsigned int>(_sourceEndpoints.size()) - audioCount;
	const auto contentWidth = [](unsigned int count) {
		return count == 0u ? 0u : count * _SourceButtonWidth +
			(count - 1u) * GuiStackPanelParams::PanelRowSpacing;
	};
	const unsigned int innerWidth = _TopStripWidth - (_TopStripPadding * 2u);
	GuiStackPanelParams sourceRowParams = GuiStackPanelParams::PanelHorizontalRow(innerWidth, _SourceViewportHeight);
	sourceRowParams.Spacing = GuiStackPanelParams::PanelRowSpacing - 4u;
	_topSourceRow = std::make_shared<GuiStackPanel>(sourceRowParams);
	if (audioCount > 0u)
	{
		auto rowParams = GuiStackPanelParams::PanelHorizontalRow(contentWidth(audioCount), _SourceButtonHeight);
		_topInputRow = std::make_shared<GuiStackPanel>(rowParams);
	}
	if (midiCount > 0u)
	{
		auto rowParams = GuiStackPanelParams::PanelHorizontalRow(contentWidth(midiCount), _SourceButtonHeight);
		_topMidiRow = std::make_shared<GuiStackPanel>(rowParams);
	}

	for (const auto& source : _sourceEndpoints)
	{
		const auto isAdc = source.Kind == io::RigFileRouting::SourceKind::Adc;
		auto label = isAdc ? "Audio In " + std::to_string(source.AdcChannel + 1u) :
			"MIDI " + source.MidiDevice;
		if (!source.Available)
			label += " (unavailable)";
		auto button = _MakeSourceButton(label,
			source.Available ? (isAdc ? glm::vec3(0.92f, 0.52f, 0.24f) : glm::vec3(0.22f, 0.72f, 0.66f)) : glm::vec3(0.50f),
			_SourceButtonWidth, !isAdc, source.Available);
		auto socket = std::make_shared<GuiHudSocket>(
			utils::Position2d{ static_cast<int>(_SourceButtonWidth / 2u - _SocketSize / 2u), 0 },
			_SocketSize,
			source.Available
			? (isAdc ? glm::vec3(0.92f, 0.52f, 0.24f) : glm::vec3(0.22f, 0.72f, 0.66f))
			: glm::vec3(0.35f));
		button->AddChild(socket);
		_sourceWidgets.push_back({ button, socket });
		(isAdc ? _topInputRow : _topMidiRow)->AddChild(button);
		GuiVuParams vuParams;
		if (isAdc)
			vuParams.HoldSamps = _AudioInputPeakHoldSamps;
		else
		{
			vuParams.FallRate = _MidiInputFallRate;
			vuParams.HoldFallRate = _MidiInputHoldFallRate;
			vuParams.HoldSamps = _MidiInputPeakHoldSamps;
			vuParams.UseDecibelScale = false;
		}
		_inputVus.push_back(std::make_unique<GuiVu>(vuParams));
	}
	const auto makeScroll = [](const std::shared_ptr<GuiStackPanel>& row) {
		GuiScrollPanelParams params;
		params.Orientation = GuiScrollOrientation::Horizontal;
		params.ScrollBarWidth = _SourceScrollBarHeight;
		params.WheelStep = _SourceButtonWidth;
		params.Size = { row->GetSize().Width, _SourceViewportHeight };
		params.MinSize = { 60u, _SourceViewportHeight };
		params.Texture = "";
		params.ScrollBarTexture = "rounded_but";
		auto scroll = std::make_shared<GuiScrollPanel>(params);
		scroll->SetContent(row);
		return scroll;
	};
	if (_topInputRow)
	{
		_topAudioScroll = makeScroll(_topInputRow);
		_topSourceRow->AddChild(_topAudioScroll);
	}
	if (_topMidiRow)
	{
		_topMidiScroll = makeScroll(_topMidiRow);
		_topSourceRow->AddChild(_topMidiScroll);
	}
	_topStrip->AddChild(_topSourceRow);
}

void GuiHud::_BuildTriggerRail()
{
	auto header = _MakeHeader("Triggers", _RightRailWidth, 38u);
	header->SetPosition({ 0, 0 });
	_triggerRail->AddChild(header);

	GuiStackPanelParams listParams;
	listParams.Direction = StackDirection::Vertical;
	listParams.Spacing = _RightRailSpacing;
	listParams.PaddingH = 0u;
	listParams.PaddingV = 0u;
	listParams.Size = { _TriggerButtonWidth, 1u };
	listParams.MinSize = { _TriggerButtonWidth, 1u };
	_triggerList = std::make_shared<GuiStackPanel>(listParams);

	for (std::size_t i = 0u; i < _triggerNames.size(); ++i)
	{
		auto button = _MakeTriggerButton(_triggerNames[i],
			i < _triggers.size() ? _triggers[i] : std::weak_ptr<engine::Trigger>());
		GuiButtonParams closeParams;
		closeParams.Texture = "trigger_close";
		closeParams.OverTexture = "trigger_close_over";
		closeParams.DownTexture = "trigger_close_down";
		closeParams.Size = { _TriggerControlSize, _TriggerControlSize };
		closeParams.MinSize = closeParams.Size;
		closeParams.Position = { static_cast<int>(_TriggerButtonWidth - _TriggerControlSize - 4u),
			static_cast<int>(_TriggerButtonHeight - _TriggerControlSize - 4u) };
		closeParams.TextPadding = 0u;
		closeParams.TintColor = glm::vec3(1.0f);
		auto close = std::make_shared<GuiHudActionButton>(closeParams, [this, i]() { _OpenDeleteConfirmation(i); });
		button->AddChild(close);
		auto inputSocket = std::make_shared<GuiHudSocket>(
			utils::Position2d{ 0, static_cast<int>(_TriggerButtonHeight) -
				_TriggerInputPinTopOffset - static_cast<int>(_SocketSize / 2u) },
			_SocketSize,
			glm::vec3(0.86f, 0.24f, 0.26f));
		button->AddChild(inputSocket);

		auto outputSocket = std::make_shared<GuiHudSocket>(
			utils::Position2d{ 0, _TriggerOutputPinBottomOffset - static_cast<int>(_SocketSize / 2u) },
			_SocketSize,
			glm::vec3(0.92f, 0.79f, 0.20f));
		button->AddChild(outputSocket);
		_triggerWidgets.push_back({ button, close, inputSocket, outputSocket });
		_triggerList->AddChild(button);
	}
	const auto logicalHeight = _triggerNames.empty() ? 1u :
		static_cast<unsigned int>(_triggerNames.size()) * _TriggerButtonHeight +
		static_cast<unsigned int>(_triggerNames.size() - 1u) * _RightRailSpacing;
	_triggerList->SetSize({ _TriggerButtonWidth, logicalHeight });

	GuiScrollPanelParams scrollParams = GuiScrollPanelParams::PanelScroll(_TriggerButtonWidth, 1u);
	scrollParams.ScrollBarWidth = 8u;
	scrollParams.Texture = "";
	scrollParams.OverTexture = "";
	scrollParams.DownTexture = "";
	_triggerScroll = std::make_shared<GuiScrollPanel>(scrollParams);
	_triggerScroll->SetContent(_triggerList);
	_triggerRail->AddChild(_triggerScroll);

	GuiButtonParams addParams;
	addParams.Texture = "trigger_add";
	addParams.OverTexture = "trigger_add_over";
	addParams.DownTexture = "trigger_add_down";
	addParams.Size = { _TriggerControlSize, _TriggerControlSize };
	addParams.MinSize = addParams.Size;
	addParams.TextPadding = 0u;
	addParams.TintColor = glm::vec3(1.0f);
	_addTriggerButton = std::make_shared<GuiHudActionButton>(addParams, [this]() { _AddTrigger(); });
	_triggerRail->AddChild(_addTriggerButton);

	GuiLabelParams statusParams = GuiLabelParams::PanelScrollRow("Trigger routing ready", 0u);
	statusParams.Ellipsize = true;
	statusParams.Size = { 300u, GuiLabelParams::RowHeight };
	statusParams.MinSize = { 160u, GuiLabelParams::RowHeight };
	_routingStatusLabel = std::make_shared<GuiLabel>(statusParams);
	GuiElementParams statusPanelParams;
	statusPanelParams.Texture = "red";
	statusPanelParams.TextureShader = "colour";
	statusPanelParams.TintColor = GuiStyle::Graphite();
	statusPanelParams.TextureOpacity = 1.0f;
	_statusPanel = std::make_shared<GuiHudStatusPanel>(statusPanelParams);
	_statusPanel->AddChild(_routingStatusLabel);
	AddChild(_statusPanel);
}

void GuiHud::_RebuildPanels()
{
	const int previousScrollOffset = _triggerScroll ? _triggerScroll->ScrollOffset() : 0;
	const int previousAudioOffset = _topAudioScroll ? _topAudioScroll->ScrollOffset() : 0;
	const int previousMidiOffset = _topMidiScroll ? _topMidiScroll->ScrollOffset() : 0;
	const bool revealNewest = _revealNewestTrigger;
	_sourceWidgets.clear();
	_triggerWidgets.clear();
	_inputVus.clear();
	_topStrip.reset();
	_topSourceRow.reset();
	_topInputRow.reset();
	_topMidiRow.reset();
	_topAudioScroll.reset();
	_topMidiScroll.reset();
	_triggerRail.reset();
	_triggerScroll.reset();
	_triggerList.reset();
	_addTriggerButton.reset();
	_routingStatusLabel.reset();
	_lastRoutingEditAvailability.reset();
	_children.clear();

	_BuildPanels();
	_LayoutPanels();
	// The rebuilt controls and VU meters need GL resources on the render thread.
	_resourcesNeedInitialising.store(true, std::memory_order_release);
	if (_triggerScroll && !revealNewest)
		_triggerScroll->SetScrollOffset(previousScrollOffset);
	if (_topAudioScroll)
		_topAudioScroll->SetScrollOffset(previousAudioOffset);
	if (_topMidiScroll)
		_topMidiScroll->SetScrollOffset(previousMidiOffset);
	_lastTriggerScrollOffset = _triggerScroll ? _triggerScroll->ScrollOffset() : 0;
	_lastAudioScrollOffset = _topAudioScroll ? _topAudioScroll->ScrollOffset() : 0;
	_lastMidiScrollOffset = _topMidiScroll ? _topMidiScroll->ScrollOffset() : 0;
	_hoveredCableEndpoint.reset();
	_hoveredCableRoute.reset();
	_hoveredCableEnd.reset();
	_cablesDirty = true;
}

void GuiHud::SetCableRevealHeld(bool held)
{
	_cableRevealHeld = held;
	_cablesDirty = true;
	_hoveredCableRoute.reset();
	_hoveredCableEnd.reset();
	_hoveredCableEndpoint.reset();
	if (_cableHoverPoint) _UpdateCableHover(*_cableHoverPoint);
}

void GuiHud::SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps)
{
	if (channel < _inputVus.size())
		_inputVus[channel]->SetPeak(peak, numSamps);
}

void GuiHud::SetMidiInputPeak(unsigned int input, float peak, unsigned int numSamps)
{
	const auto vuIndex = _audioInputCount + input;
	if (vuIndex < _inputVus.size())
		_inputVus[vuIndex]->SetPeak(peak, numSamps);
}

void GuiHud::SetRoutingConfig(unsigned int audioInputCount,
	std::vector<std::string> midiInputNames,
	const engine::RigSnapshot& routing)
{
	if (_displayedRevision != 0u && _displayedRevision != routing.Revision)
	{
		_CancelCableDrag();
		_deleteTriggerIndex.reset();
		if (_popupManager && _deletePopup && _popupManager->Top() == _deletePopup)
			_popupManager->Close();
	}
	_displayedRevision = routing.Revision;
	_displayedRig = routing.Rig;
	_audioInputCount = audioInputCount;
	_midiInputNames.clear();
	for (auto& name : midiInputNames)
		if (!name.empty())
			_midiInputNames.push_back(std::move(name));

	_routingGraph = routing.Graph.Triggers;
	_sourceEndpoints.clear();
	for (unsigned int channel = 0u; channel < _audioInputCount; ++channel)
		_sourceEndpoints.push_back({ io::RigFileRouting::SourceKind::Adc, channel, {}, true });
	for (const auto& name : _midiInputNames)
		_sourceEndpoints.push_back({ io::RigFileRouting::SourceKind::Midi, 0u, name, true });
	for (const auto& resolvedTrigger : _routingGraph)
	{
		for (const auto& source : resolvedTrigger.Sources)
		{
			const auto exists = std::find_if(_sourceEndpoints.begin(), _sourceEndpoints.end(), [&source](const auto& endpoint)
			{
				return endpoint.Kind == source.Kind && endpoint.AdcChannel == source.AdcChannel &&
					endpoint.MidiDevice == source.MidiDevice;
			});
			if (exists == _sourceEndpoints.end())
				_sourceEndpoints.push_back(source);
		}
	}

	_triggers.assign(_routingGraph.size(), {});
	_triggerNames.clear();
	for (const auto& resolvedTrigger : _routingGraph)
	{
		auto label = resolvedTrigger.TriggerName;
		if (resolvedTrigger.Reason == io::RigFileRouting::Warning::TargetMissing)
			label += " [target missing]";
		else if (resolvedTrigger.Reason == io::RigFileRouting::Warning::TargetAmbiguous)
			label += " [target ambiguous]";
		else if (!resolvedTrigger.StationIndex.has_value())
			label += " [unbound]";
		_triggerNames.push_back(std::move(label));
	}
	for (const auto& runtimeTrigger : routing.Triggers)
	{
		if (runtimeTrigger.RigTriggerIndex < _triggers.size())
			_triggers[runtimeTrigger.RigTriggerIndex] = runtimeTrigger.Instance;
	}
	_RebuildPanels();
}

unsigned int GuiHud::SourceCardWidth(unsigned int viewportWidth, unsigned int count)
{
	if (count == 0u) return 0u;
	const auto gaps = static_cast<unsigned long long>(count - 1u) * GuiStackPanelParams::PanelRowSpacing + 4u;
	const auto usable = viewportWidth > gaps ? viewportWidth - gaps : 0u;
	return static_cast<unsigned int>(std::clamp(usable / count, 80ull, 160ull));
}

void GuiHud::_LayoutPanels()
{
	const int width = static_cast<int>(GetSize().Width);
	const int height = static_cast<int>(GetSize().Height);
	const int marginX = std::min(_OuterMargin, width / 2);
	const int marginY = std::min(_TopPosY, height / 2);
	const int railWidth = std::min(static_cast<int>(_RightRailWidth), std::max(0, width - 2 * marginX));
	const int railHeight = std::max(0, height - marginY - GuiStyle::StatusBarHeight - 8);
	const int railX = std::max(0, width - marginX - railWidth);
	_triggerRail->SetPosition({ railX, std::min(GuiStyle::StatusBarHeight + 8, height) });
	_triggerRail->SetSize({ static_cast<unsigned int>(railWidth), static_cast<unsigned int>(railHeight) });
	_triggerRail->SetVisible(railWidth > 0 && railHeight > 0);
	const int triggerHeader = std::min(34, railHeight);
	const int triggerFooter = std::min(static_cast<int>(_TriggerFooterHeight), railHeight - triggerHeader);
	const int triggerViewport = std::max(0, railHeight - triggerHeader - triggerFooter);
	_triggerScroll->SetPosition({ 0, triggerFooter });
	_triggerScroll->SetSize({ static_cast<unsigned int>(railWidth), static_cast<unsigned int>(triggerViewport) });
	_triggerScroll->SetVisible(railWidth > 0 && triggerViewport > 0);
	const int triggerSidePadding = std::max(0, (railWidth - static_cast<int>(_TriggerButtonWidth)) / 2);
	_triggerList->SetPadding(static_cast<unsigned int>(triggerSidePadding), 0u);
	_triggerList->SetSize({ static_cast<unsigned int>(railWidth), _triggerList->GetSize().Height });
	if (auto header = _triggerRail->TryGetChild(0u))
	{
		header->SetPosition({ 0, railHeight - triggerHeader });
		header->SetSize({ static_cast<unsigned int>(railWidth), static_cast<unsigned int>(triggerHeader) });
		header->SetVisible(railWidth >= 80 && triggerHeader >= 22);
	}
	_addTriggerButton->SetPosition({ std::max(0, (railWidth - static_cast<int>(_TriggerControlSize)) / 2),
		std::max(0, (triggerFooter - static_cast<int>(_TriggerControlSize)) / 2) });
	_addTriggerButton->SetVisible(railWidth >= static_cast<int>(_TriggerControlSize) && triggerFooter >= static_cast<int>(_TriggerControlSize));

	int topWidth = std::max(0, railX - marginX - static_cast<int>(_SourcePanelGap));
	int topHeight = std::min(static_cast<int>(_TopStripHeight), std::max(0, height - 2 * marginY));
	_topStrip->SetPosition({ marginX, std::max(0, height - marginY - topHeight) });
	_topStrip->SetSize({ static_cast<unsigned int>(topWidth), static_cast<unsigned int>(topHeight) });
	_topStrip->SetVisible(topWidth > 0 && topHeight > 0);
	const int padding = std::min(static_cast<int>(_TopStripPadding), std::min(topWidth, topHeight) / 2);
	const int innerWidth = std::max(0, topWidth - 2 * padding);
	int rowHeight = std::min(static_cast<int>(_SourceViewportHeight), std::max(0, topHeight - 2 * padding));
	_topSourceRow->SetVisible(innerWidth > 0 && rowHeight > 0);
	const auto audioCount = static_cast<unsigned int>(std::count_if(_sourceEndpoints.begin(), _sourceEndpoints.end(),
		[](const auto& source) { return source.Kind == io::RigFileRouting::SourceKind::Adc; }));
	const auto midiCount = static_cast<unsigned int>(_sourceEndpoints.size()) - audioCount;
	const int categoryGap = audioCount && midiCount ? std::min(static_cast<int>(GuiStackPanelParams::PanelRowSpacing - 4u), innerWidth) : 0;
	const int labelWidth = innerWidth >= static_cast<int>((audioCount + midiCount) * 84u) + 68 ? 68 : 0;
	const auto budget = static_cast<unsigned int>(std::max(0, innerWidth - labelWidth - categoryGap));
	auto audioWidth = audioCount + midiCount ? static_cast<unsigned int>(static_cast<unsigned long long>(budget) * audioCount / (audioCount + midiCount)) : 0u;
	// Keep a minority category usable too: one minimum-width card plus the
	// row's 2px padding on each side. Below this budget retain real clipping.
	constexpr unsigned int categoryMinimum = 84u;
	if (audioCount && midiCount && budget >= categoryMinimum * 2u)
		audioWidth = std::clamp(audioWidth, categoryMinimum, budget - categoryMinimum);
	auto midiWidth = midiCount ? budget - audioWidth : 0u;
	const auto audioCard = SourceCardWidth(audioWidth, audioCount);
	const auto midiCard = SourceCardWidth(midiWidth, midiCount);
	// Shrink category viewports to their occupied cards before right-aligning the group.
	const auto occupiedWidth = [](unsigned int count, unsigned int card) {
		return count ? count * card + (count - 1u) * GuiStackPanelParams::PanelRowSpacing + 4u : 0u;
	};
	audioWidth = std::min(audioWidth, occupiedWidth(audioCount, audioCard));
	midiWidth = std::min(midiWidth, occupiedWidth(midiCount, midiCard));
	const bool sourceScrolls = audioWidth < occupiedWidth(audioCount, audioCard) || midiWidth < occupiedWidth(midiCount, midiCard);
	const int desiredRowHeight = static_cast<int>(_SourceButtonHeight + 4u + (sourceScrolls ? _SourceScrollBarHeight : 0u));
	rowHeight = std::min(rowHeight, desiredRowHeight);
	topHeight = rowHeight + 2 * padding;
	// Keep the same right anchor, but stop the background growing beyond its contents.
	topWidth = std::min(topWidth, static_cast<int>(audioWidth + midiWidth) + categoryGap + labelWidth + 2 * padding);
	_topStrip->SetPosition({ railX - static_cast<int>(_SourcePanelGap) - topWidth, std::max(0, height - marginY - topHeight) });
	_topStrip->SetSize({ static_cast<unsigned int>(topWidth), static_cast<unsigned int>(topHeight) });
	_topSourceRow->SetSize({ audioWidth + midiWidth + static_cast<unsigned int>(categoryGap), static_cast<unsigned int>(rowHeight) });
	for (size_t i = 0; i < _sourceWidgets.size(); ++i)
	{
		const auto cardWidth = _sourceEndpoints[i].Kind == io::RigFileRouting::SourceKind::Adc ? audioCard : midiCard;
		auto& widgets = _sourceWidgets[i];
		widgets.Button->SetSize({ cardWidth, _SourceButtonHeight });
		widgets.Socket->SetPosition({ static_cast<int>(cardWidth / 2u) - static_cast<int>(_SocketSize / 2u), 0 });
		for (unsigned char line = 0; line < 2; ++line)
			if (auto label = widgets.Button->TryGetChild(line))
				label->SetSize({ cardWidth > 20u ? cardWidth - 20u : 0u, (_SourceButtonHeight - 4u) / 2u });
	}
	const auto layoutCategory = [rowHeight](const std::shared_ptr<GuiStackPanel>& row, const std::shared_ptr<GuiScrollPanel>& scroll,
		unsigned int count, unsigned int cardWidth, unsigned int viewportWidth)
	{
		if (!row || !scroll) return;
		const auto contentWidth = count ? count * cardWidth + (count - 1u) * GuiStackPanelParams::PanelRowSpacing + 4u : 0u;
		row->SetPadding(2u, 2u);
		row->SetSize({ contentWidth, _SourceButtonHeight + 4u });
		row->ComputeLayout();
		scroll->SetSize({ viewportWidth, static_cast<unsigned int>(rowHeight) });
		scroll->SetVisible(viewportWidth > 0 && rowHeight > 0);
	};
	layoutCategory(_topInputRow, _topAudioScroll, audioCount, audioCard, audioWidth);
	layoutCategory(_topMidiRow, _topMidiScroll, midiCount, midiCard, midiWidth);
	_topSourceRow->SetPosition({ topWidth - padding - static_cast<int>(_topSourceRow->GetSize().Width), topHeight - padding - rowHeight });
	if (auto header = _topStrip->TryGetChild(0u))
	{
		header->SetPosition({ _topSourceRow->Position().X - labelWidth, topHeight - padding - 28 });
		header->SetSize({ static_cast<unsigned int>(labelWidth), 22u });
		header->SetVisible(labelWidth > 0 && rowHeight >= 28);
	}
	_topSourceRow->ComputeLayout();
	_triggerList->ComputeLayout();
	const int statusWidth = GuiStyle::StatusBarWidth(width);
	const int statusHeight = std::min(GuiStyle::StatusBarHeight, height);
	_statusPanel->SetPosition({ width - statusWidth, 0 });
	_statusPanel->SetSize({ static_cast<unsigned int>(statusWidth), static_cast<unsigned int>(statusHeight) });
	_statusPanel->SetVisible(statusWidth > 0 && statusHeight > 0);
	_routingStatusLabel->SetPosition({ 0, std::max(0, (statusHeight - 22) / 2) });
	_routingStatusLabel->SetSize({ static_cast<unsigned int>(GuiStyle::StatusColumnWidth(statusWidth)), 22u });
	_routingStatusLabel->SetVisible(statusWidth > 0 && statusHeight >= 22);
	if (_revealNewestTrigger && !_triggerNames.empty())
	{
		_RevealTrigger(_triggerNames.size() - 1u);
		_revealNewestTrigger = false;
	}
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

void GuiHud::_DrawOverlayElement(base::DrawContext& ctx,
	const std::shared_ptr<base::GuiElement>& element) const
{
	if (!element)
		return;
	const auto parent = element->Parent();
	if (!parent)
		return;
	const auto parentPosition = parent->GlobalPosition() - GlobalPosition();
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0f),
		glm::vec3(static_cast<float>(parentPosition.X), static_cast<float>(parentPosition.Y), 0.0f)));
	element->Draw(ctx);
	glCtx.PopMvp();
}

void GuiHud::_DrawCables(base::DrawContext& ctx)
{
	const float fadeInStep = 0.16f;
	const float fadeOutStep = 0.08f;
	if (_cableRevealHeld || _hoveredCableEndpoint.has_value() || _cableDrag.has_value())
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
	const auto pointsUniform = glGetUniformLocation(program, "CableControlPoints");
	const auto countUniform = glGetUniformLocation(program, "CableCount");
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
		// cable.vert has 64 control points: four per curve. Retaining clipped
		// connections must not overflow that uniform array in populated scenes.
		for (size_t first = 0; first < _cableRenderColors.size(); first += _CableBatchSize)
		{
			const auto count = static_cast<GLsizei>(std::min(_CableBatchSize, _cableRenderColors.size() - first));
			glUniform4fv(pointsUniform, count * 4,
				reinterpret_cast<const GLfloat*>(_cableControlPoints.data() + first * 4));
			glUniform4fv(colorUniform, count,
				reinterpret_cast<const GLfloat*>(_cableRenderColors.data() + first));
			glUniform1i(countUniform, count);
			glDrawArraysInstanced(GL_LINE_STRIP, 0, _CableSegments, count);
		}
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

actions::ActionResult GuiHud::_BeginCableDrag(Position2d point)
{
	if (_RoutingEditAvailability() != RoutingEditAvailability::Ready)
		return actions::ActionResult::NoAction();
	_UpdateCableHover(point);
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	BuildInteractionGeometry(endpoints, cables);
	_FilterCableHits(endpoints, cables, point);
	const auto canEditTrigger = [this](size_t triggerIndex) { return _CanEditTrigger(triggerIndex); };

	const auto cableEnd = CableInteraction::HitCableEnd(cables, point, _CableEndSize);
	const auto cableIndex = cableEnd.has_value()
		? std::optional<size_t>(cableEnd->first)
		: CableInteraction::HitCable(cables, point, _CableHitRadius);
	if (cableIndex.has_value())
	{
		const auto& cable = cables[cableIndex.value()];
		if (!canEditTrigger(cable.Route.TriggerIndex))
			return actions::ActionResult::NoAction();
		const auto movingEnd = _loopEditorMode && !_cableRevealHeld && cable.Route.Kind == CableInteraction::RouteKind::Station
			? CableInteraction::End::Start
			: cableEnd.has_value() ? cableEnd->second : CableInteraction::ClosestEnd(cable, point);
		const auto& fixed = movingEnd == CableInteraction::End::Start ? cable.Finish : cable.Start;
		if (!fixed.Available || (fixed.Source.has_value() && !fixed.Source->Available))
			return actions::ActionResult::NoAction();
		_cableDrag = CableInteraction::Drag{ cable.Route,
			movingEnd,
			fixed,
			cable.Route.Kind == CableInteraction::RouteKind::Capture ? cable.Start.Source : std::nullopt,
			point,
			std::nullopt };
		_cableDrag->FromCable = true;
	}
	else if (const auto endpointIndex = CableInteraction::HitEndpoint(endpoints, point, _SocketHitRadius); endpointIndex.has_value())
	{
		const auto& endpoint = endpoints[endpointIndex.value()];
		if (!endpoint.Available || (endpoint.Source.has_value() && !endpoint.Source->Available))
			return actions::ActionResult::NoAction();

		CableInteraction::Handle handle{ _displayedRevision, endpoint.TriggerIndex.value_or(static_cast<size_t>(-1)),
			CableInteraction::RouteKind::Capture, 0u };
		CableInteraction::End movingEnd = CableInteraction::End::Finish;
		if (endpoint.Kind == CableInteraction::EndpointKind::AdcSource || endpoint.Kind == CableInteraction::EndpointKind::MidiSource)
		{
			handle.TriggerIndex = static_cast<size_t>(-1);
		}
		else if (endpoint.Kind == CableInteraction::EndpointKind::TriggerInput)
		{
			if (!endpoint.TriggerIndex.has_value() || !canEditTrigger(endpoint.TriggerIndex.value()))
				return actions::ActionResult::NoAction();
			movingEnd = CableInteraction::End::Start;
		}
		else if (endpoint.Kind == CableInteraction::EndpointKind::TriggerOutput)
		{
			if (!endpoint.TriggerIndex.has_value() || !canEditTrigger(endpoint.TriggerIndex.value()))
				return actions::ActionResult::NoAction();
			handle.Kind = CableInteraction::RouteKind::Station;
		}
		else
		{
			if (!endpoint.TriggerIndex.has_value() || !canEditTrigger(endpoint.TriggerIndex.value()))
				return actions::ActionResult::NoAction();
			handle.Kind = CableInteraction::RouteKind::Station;
			movingEnd = CableInteraction::End::Start;
		}
		_cableDrag = CableInteraction::Drag{ handle, movingEnd, endpoint, std::nullopt, point, std::nullopt };
	}

	if (!_cableDrag.has_value())
		return actions::ActionResult::NoAction();
	_cablesDirty = true;
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr,
		std::static_pointer_cast<base::GuiElement>(shared_from_this()) };
}

void GuiHud::_CancelCableDrag()
{
	CableInteraction::Cancel(_cableDrag);
	_UpdateSocketHighlights();
	_cablesDirty = true;
}

void GuiHud::ClearPointerState()
{
	GuiPanel::ClearPointerState();
	if (_cableDrag) _CancelCableDrag();
}

void GuiHud::SetLoopEditorMode(bool enabled)
{
	if (_loopEditorMode == enabled) return;
	_loopEditorMode = enabled;
	_hoveredCableEndpoint.reset();
	_hoveredCableRoute.reset();
	_hoveredCableEnd.reset();
	_CancelCableDrag();
}

bool GuiHud::_CableVisible(const CableInteraction::Cable& cable) const
{
	return CableInteraction::Revealed(cable, _cableRevealHeld, _cableDrag.has_value(), _hoveredCableEndpoint);
}

void GuiHud::_FilterCableHits(std::vector<CableInteraction::Endpoint>& endpoints,
	std::vector<CableInteraction::Cable>& cables, Position2d point) const
{
	std::erase_if(endpoints, [this](const auto& endpoint) {
		return _loopEditorMode && endpoint.Kind == CableInteraction::EndpointKind::Station;
	});
	std::erase_if(cables, [this, point](const auto& cable) {
		return !_CableVisible(cable) || (!CableInteraction::CanGrabEnd(cable, CableInteraction::End::Finish, _loopEditorMode, _cableRevealHeld) &&
			!CableInteraction::HitTest(cable.Start, point, _CableEndSize));
	});
}

void GuiHud::_DrawCableSockets(base::DrawContext& ctx)
{
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	BuildInteractionGeometry(endpoints, cables);
	for (const auto& anchor : _stationAnchors)
	{
		if (!anchor.ScreenPosition)
			continue;
		const auto root = GlobalPosition();
		const auto point = CableInteraction::ResolveBoundary(*anchor.ScreenPosition - glm::dvec2{ root.X, root.Y },
			{ 0, 0, static_cast<int>(GetSize().Width), static_cast<int>(GetSize().Height) });
		if (!point || point->Clipped)
			continue;
		const bool revealed = _cableRevealHeld || std::any_of(cables.begin(), cables.end(),
			[this, &anchor](const auto& cable) {
				return cable.Finish.Kind == CableInteraction::EndpointKind::Station &&
					cable.Finish.StationIndex == anchor.StationIndex && _CableVisible(cable);
			});
		_stationSocketIcon->SetVisible(!_loopEditorMode || revealed);
		_stationSocketIcon->SetPosition({ point->Position.X - static_cast<int>(_SocketSize / 2u),
			point->Position.Y - static_cast<int>(_SocketSize / 2u) });
		const auto highlighted = _hoveredCableEndpoint.has_value() && !_hoveredCableRoute.has_value() &&
			_hoveredCableEndpoint->Kind == CableInteraction::EndpointKind::Station &&
			_hoveredCableEndpoint->StationIndex == anchor.StationIndex;
		const auto snapped = _cableDrag.has_value() && _cableDrag->Snap.has_value() &&
			_cableDrag->Snap->Kind == CableInteraction::EndpointKind::Station &&
			_cableDrag->Snap->StationIndex == anchor.StationIndex;
		_stationSocketIcon->SetHighlighted(highlighted || snapped);
		_stationSocketIcon->Draw(ctx);
	}

	const auto drawEnd = [this, &ctx](const CableInteraction::Cable& cable, CableInteraction::End end)
	{
		const auto& endpoint = end == CableInteraction::End::Start ? cable.Start : cable.Finish;
		const auto selected = _hoveredCableRoute.has_value() && _hoveredCableEnd == end &&
			_hoveredCableRoute->Revision == cable.Route.Revision &&
			_hoveredCableRoute->TriggerIndex == cable.Route.TriggerIndex &&
			_hoveredCableRoute->Kind == cable.Route.Kind &&
			_hoveredCableRoute->RouteIndex == cable.Route.RouteIndex;
		_cableEndIcon->SetSize(endpoint.Continuation ? Size2d{ 8u, 3u } : Size2d{ _CableEndSize, _CableEndSize });
		_cableEndIcon->SetTint(endpoint.Continuation ? glm::vec3(0.55f, 0.60f, 0.65f) : cable.Route.Kind == CableInteraction::RouteKind::Capture
			? glm::vec3(0.86f, 0.24f, 0.26f) : glm::vec3(0.92f, 0.79f, 0.20f));
		_cableEndIcon->SetHighlighted(selected && !endpoint.Continuation);
		const auto iconSize = _cableEndIcon->GetSize();
		auto iconPos = Position2d{ endpoint.Position.X - static_cast<int>(iconSize.Width / 2u),
			endpoint.Position.Y - static_cast<int>(iconSize.Height / 2u) };
		if (endpoint.Continuation && endpoint.HitBounds)
		{
			const auto bounds = *endpoint.HitBounds;
			iconPos.X = std::clamp(iconPos.X, bounds.Left, std::max(bounds.Left, bounds.Right - static_cast<int>(iconSize.Width)));
			iconPos.Y = std::clamp(iconPos.Y, bounds.Bottom, std::max(bounds.Bottom, bounds.Top - static_cast<int>(iconSize.Height)));
		}
		_cableEndIcon->SetPosition(iconPos);
		_cableEndIcon->Draw(ctx);
	};
	for (const auto& cable : cables)
	{
		// Plugs indicate connections even when curves cannot be edited.
		if ((!_loopEditorMode || _CableVisible(cable)) &&
			cable.Finish.Kind != CableInteraction::EndpointKind::TriggerInput &&
			cable.Finish.Kind != CableInteraction::EndpointKind::TriggerOutput)
			drawEnd(cable, CableInteraction::End::Finish);
	}
	const auto drawSourceEnds = [this, &ctx, &cables, &drawEnd](const std::shared_ptr<GuiScrollPanel>& scroll,
		CableInteraction::EndpointKind kind)
	{
		if (!scroll)
			return;
		const auto clip = _ContentClip(scroll);
		auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
		glCtx.PushScissorRect({ clip.Left, clip.Bottom }, {
			static_cast<unsigned int>(clip.Right - clip.Left), static_cast<unsigned int>(clip.Top - clip.Bottom) });
		for (const auto& cable : cables)
			if (cable.Start.Kind == kind)
				drawEnd(cable, CableInteraction::End::Start);
		glCtx.PopScissorRect();
	};
	drawSourceEnds(_topAudioScroll, CableInteraction::EndpointKind::AdcSource);
	drawSourceEnds(_topMidiScroll, CableInteraction::EndpointKind::MidiSource);
	if (_triggerScroll)
	{
		auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
		const auto clip = _ContentClip(_triggerScroll);
		glCtx.PushScissorRect({ clip.Left, clip.Bottom }, {
			static_cast<unsigned int>(clip.Right - clip.Left), static_cast<unsigned int>(clip.Top - clip.Bottom) });
		for (const auto& cable : cables)
		{
			if (cable.Start.Kind == CableInteraction::EndpointKind::TriggerInput ||
				cable.Start.Kind == CableInteraction::EndpointKind::TriggerOutput)
				drawEnd(cable, CableInteraction::End::Start);
			if (cable.Finish.Kind == CableInteraction::EndpointKind::TriggerInput ||
				cable.Finish.Kind == CableInteraction::EndpointKind::TriggerOutput)
				drawEnd(cable, CableInteraction::End::Finish);
		}
		glCtx.PopScissorRect();
	}
}

int GuiHud::RevealScrollOffset(int currentOffset, int viewportHeight,
	int contentHeight, int itemTop, int itemBottom)
{
	const auto maxOffset = std::max(0, contentHeight - viewportHeight);
	auto offset = std::clamp(currentOffset, 0, maxOffset);
	if (itemTop < offset)
		offset = itemTop;
	else if (itemBottom > offset + viewportHeight)
		offset = itemBottom - viewportHeight;
	return std::clamp(offset, 0, maxOffset);
}

bool GuiHud::_CanEditTrigger(size_t triggerIndex) const
{
	if (_RoutingEditAvailability() != RoutingEditAvailability::Ready || triggerIndex >= _triggers.size())
		return false;
	const auto trigger = _triggers[triggerIndex].lock();
	return trigger && trigger->CanApplyCaptureRouting();
}

bool GuiHud::_SubmitCandidate(const io::RigFile& candidate)
{
	if (!_submitRigEdit || _RoutingEditAvailability() != RoutingEditAvailability::Ready)
		return false;
	_CancelCableDrag();
	return _submitRigEdit(candidate);
}

void GuiHud::_AddTrigger()
{
	if (_RoutingEditAvailability() != RoutingEditAvailability::Ready)
		return;
	const auto candidate = io::RigFileRouting::WithUnboundTrigger(_displayedRig);
	_revealNewestTrigger = _SubmitCandidate(candidate);
}

RoutingEditAvailability GuiHud::_RoutingEditAvailability() const
{
	return _routingEditAvailability ? _routingEditAvailability() : RoutingEditAvailability::Ready;
}

void GuiHud::_UpdateRoutingEditPresentation()
{
	const auto availability = _RoutingEditAvailability();
	const bool ready = availability == RoutingEditAvailability::Ready;
	if (_addTriggerButton)
		_addTriggerButton->SetEnabled(ready);
	for (size_t i = 0u; i < _triggerWidgets.size(); ++i)
	{
		const auto trigger = i < _triggers.size() ? _triggers[i].lock() : nullptr;
		_triggerWidgets[i].Close->SetEnabled(ready && trigger && trigger->CanEditRouting());
	}

	if (!_routingStatusLabel || _lastRoutingEditAvailability == availability)
		return;
	_lastRoutingEditAvailability = availability;
	switch (availability)
	{
	case RoutingEditAvailability::Ready: _routingStatusLabel->SetString("Trigger routing ready"); break;
	case RoutingEditAvailability::Applying: _routingStatusLabel->SetString("Applying trigger routing..."); break;
	case RoutingEditAvailability::AudioCallbackInactive: _routingStatusLabel->SetString("Start audio to edit trigger routing"); break;
	case RoutingEditAvailability::TriggerBusy: _routingStatusLabel->SetString("Finish trigger action to edit routing"); break;
	}
}

void GuiHud::_OpenDeleteConfirmation(size_t triggerIndex)
{
	const auto trigger = triggerIndex < _triggers.size() ? _triggers[triggerIndex].lock() : nullptr;
	if (_RoutingEditAvailability() != RoutingEditAvailability::Ready || !trigger || !trigger->CanEditRouting() ||
		triggerIndex >= _displayedRig.Triggers.size() || !_popupManager)
		return;
	_CancelCableDrag();
	_deleteTriggerIndex = triggerIndex;
	const auto& name = _displayedRig.Triggers[triggerIndex].Name;
	_deletePopup->SetTitle("Delete trigger?");
	_deletePopup->SetBodyLines({ "Delete " + name + " and all of its routes?" });
	_deletePopup->FitToViewport(GetSize());
	_popupManager->Open(_deletePopup, shared_from_this());
}

void GuiHud::_ConfirmDelete()
{
	const auto index = _deleteTriggerIndex;
	_deleteTriggerIndex.reset();
	if (_popupManager) _popupManager->Close();
	const auto trigger = index.has_value() && index.value() < _triggers.size() ? _triggers[index.value()].lock() : nullptr;
	if (!index.has_value() || _RoutingEditAvailability() != RoutingEditAvailability::Ready || !trigger || !trigger->CanEditRouting())
		return;
	const auto candidate = io::RigFileRouting::WithoutTrigger(_displayedRig, index.value());
	if (candidate.has_value())
		_SubmitCandidate(candidate.value());
}

void GuiHud::_RevealTrigger(size_t triggerIndex)
{
	if (!_triggerScroll || triggerIndex >= _triggerWidgets.size())
		return;
	const int contentHeight = _triggerList ? static_cast<int>(_triggerList->GetSize().Height) :
		static_cast<int>((triggerIndex + 1u) * _TriggerButtonHeight + triggerIndex * _RightRailSpacing);
	const int itemTop = static_cast<int>(triggerIndex * (_TriggerButtonHeight + _RightRailSpacing));
	const int itemBottom = itemTop + static_cast<int>(_TriggerButtonHeight);
	_triggerScroll->SetScrollOffset(RevealScrollOffset(_triggerScroll->ScrollOffset(),
		static_cast<int>(_triggerScroll->ViewportHeight()), contentHeight, itemTop, itemBottom));
}

actions::ActionResult GuiHud::OnAction(actions::TouchAction action)
{
	if (action.Index == 2 && action.State == actions::TouchAction::TOUCH_DOWN && _cableDrag.has_value())
	{
		_CancelCableDrag();
		return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (action.Index != 0)
		return GuiPanel::OnAction(action);
	if (action.State == actions::TouchAction::TOUCH_DOWN)
	{
		auto result = _BeginCableDrag(action.Position);
		return result.IsEaten ? result : GuiPanel::OnAction(action);
	}
	if (!_cableDrag.has_value())
		return GuiPanel::OnAction(action);

	const auto drag = _cableDrag.value();
	if (_RoutingEditAvailability() == RoutingEditAvailability::Ready &&
		(drag.Route.TriggerIndex >= _triggers.size() || _CanEditTrigger(drag.Route.TriggerIndex)) &&
		drag.Route.Revision == _displayedRevision)
	{
		const auto release = CableInteraction::ReleaseToCandidate(drag, _displayedRig);
		if (release.Changed && release.Candidate.has_value())
			_SubmitCandidate(release.Candidate.value());
	}
	_CancelCableDrag();
	_UpdateCableHover(action.Position);
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
}

actions::ActionResult GuiHud::OnAction(actions::TouchMoveAction action)
{
	// The engaged editor forwards HUD moves before Scene's ordinary capture
	// routing. Handle native mouse capture loss here as well as via pointer clear.
	if (_cableDrag && action.Touch == actions::TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u)
	{
		ClearPointerState();
		return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_cableDrag && (_RoutingEditAvailability() != RoutingEditAvailability::Ready ||
		(_cableDrag->Route.TriggerIndex < _triggers.size() && !_CanEditTrigger(_cableDrag->Route.TriggerIndex))))
		_CancelCableDrag();
	if (!_cableDrag.has_value())
	{
		_UpdateCableHover(action.Position);
		// Resolve one leaf now for immediate feedback; drawing repeats the selection.
		ApplyExclusiveHoverPoint(action.Position);
		return actions::ActionResult::NoAction();
	}
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	BuildInteractionGeometry(endpoints, cables);
	std::erase_if(endpoints, [this](const auto& endpoint) {
		return (endpoint.TriggerIndex && endpoint.Kind != CableInteraction::EndpointKind::Station &&
			!_CanEditTrigger(*endpoint.TriggerIndex)) ||
			(_loopEditorMode && !_cableRevealHeld && endpoint.Kind == CableInteraction::EndpointKind::Station);
	});
	CableInteraction::Update(_cableDrag.value(), action.Position, endpoints, _displayedRig,
		_SnapRadius, _SnapHysteresis);
	_UpdateSocketHighlights();
	_cablesDirty = true;
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr,
		std::static_pointer_cast<base::GuiElement>(shared_from_this()) };
}

void GuiHud::_UpdateCableHover(Position2d point)
{
	_cableHoverPoint = point;
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	BuildInteractionGeometry(endpoints, cables);
	_FilterCableHits(endpoints, cables, point);
	// In the editor only sockets reveal cables; cable hover cannot sustain reveal.
	if (_loopEditorMode && !_cableRevealHeld) cables.clear();
	auto cableEnd = CableInteraction::HitCableEnd(cables, point, _CableEndSize);
	if (!cableEnd && !_loopEditorMode)
		if (const auto index = CableInteraction::HitCable(cables, point, _CableHitRadius))
			cableEnd = std::pair{ *index, CableInteraction::ClosestEnd(cables[*index], point) };
	const auto endpointIndex = cableEnd.has_value() ? std::nullopt :
		CableInteraction::HitEndpoint(endpoints, point, _SocketHitRadius);
	const auto next = cableEnd.has_value()
		? std::optional<CableInteraction::Endpoint>(cableEnd->second == CableInteraction::End::Start
			? cables[cableEnd->first].Start : cables[cableEnd->first].Finish)
		: endpointIndex.has_value()
			? std::optional<CableInteraction::Endpoint>(endpoints[endpointIndex.value()]) : std::nullopt;
	auto revealEndpoint = next;
	if (revealEndpoint.has_value() && revealEndpoint->Kind == CableInteraction::EndpointKind::Station)
		revealEndpoint->TriggerIndex.reset();
	const auto nextRoute = cableEnd.has_value()
		? std::optional<CableInteraction::Handle>(cables[cableEnd->first].Route) : std::nullopt;
	const auto sameSource = [](const std::optional<io::RigFileRouting::Source>& lhs,
		const std::optional<io::RigFileRouting::Source>& rhs)
	{
		return lhs.has_value() == rhs.has_value() &&
			(!lhs.has_value() || (lhs->Kind == rhs->Kind && lhs->AdcChannel == rhs->AdcChannel &&
				lhs->MidiDevice == rhs->MidiDevice));
	};
	const auto sameRoute = _hoveredCableRoute.has_value() == nextRoute.has_value() &&
		(!_hoveredCableRoute.has_value() ||
			(_hoveredCableRoute->Revision == nextRoute->Revision &&
				_hoveredCableRoute->TriggerIndex == nextRoute->TriggerIndex &&
				_hoveredCableRoute->Kind == nextRoute->Kind &&
				_hoveredCableRoute->RouteIndex == nextRoute->RouteIndex));
	const auto changed = !sameRoute || _hoveredCableEnd != (cableEnd.has_value()
		? std::optional<CableInteraction::End>(cableEnd->second) : std::nullopt) ||
		_hoveredCableEndpoint.has_value() != revealEndpoint.has_value() ||
		(_hoveredCableEndpoint.has_value() && revealEndpoint.has_value() &&
			(_hoveredCableEndpoint->Kind != revealEndpoint->Kind ||
				_hoveredCableEndpoint->TriggerIndex != revealEndpoint->TriggerIndex ||
				_hoveredCableEndpoint->StationIndex != revealEndpoint->StationIndex ||
				!sameSource(_hoveredCableEndpoint->Source, revealEndpoint->Source)));
	if (!changed)
		return;

	_hoveredCableEndpoint = revealEndpoint;
	_hoveredCableRoute = nextRoute;
	_hoveredCableEnd = cableEnd.has_value()
		? std::optional<CableInteraction::End>(cableEnd->second) : std::nullopt;
	_UpdateSocketHighlights();
	_cablesDirty = true;
}

void GuiHud::_UpdateSocketHighlights()
{
	const auto isHighlighted = [this](const CableInteraction::Endpoint& endpoint)
	{
		const auto hovered = _cableDrag.has_value() ? _cableDrag->Snap :
			(_hoveredCableRoute.has_value() ? std::optional<CableInteraction::Endpoint>{} : _hoveredCableEndpoint);
		if (!hovered.has_value())
			return false;
		if (hovered->Kind != endpoint.Kind || hovered->TriggerIndex != endpoint.TriggerIndex)
			return false;
		return !hovered->Source.has_value() || (endpoint.Source.has_value() &&
			hovered->Source->Kind == endpoint.Source->Kind &&
			hovered->Source->AdcChannel == endpoint.Source->AdcChannel &&
			hovered->Source->MidiDevice == endpoint.Source->MidiDevice);
	};

	for (size_t i = 0u; i < _sourceWidgets.size() && i < _sourceEndpoints.size(); ++i)
	{
		const CableInteraction::Endpoint endpoint{ _sourceEndpoints[i].Kind == io::RigFileRouting::SourceKind::Adc
			? CableInteraction::EndpointKind::AdcSource : CableInteraction::EndpointKind::MidiSource,
			{}, {}, {}, {}, _sourceEndpoints[i] };
		if (auto socket = std::dynamic_pointer_cast<GuiHudSocket>(_sourceWidgets[i].Socket))
			socket->SetHighlighted(isHighlighted(endpoint));
	}

	for (size_t i = 0u; i < _triggerWidgets.size(); ++i)
	{
		if (auto socket = std::dynamic_pointer_cast<GuiHudSocket>(_triggerWidgets[i].InputSocket))
			socket->SetHighlighted(isHighlighted({ CableInteraction::EndpointKind::TriggerInput, {}, i }));
		if (auto socket = std::dynamic_pointer_cast<GuiHudSocket>(_triggerWidgets[i].OutputSocket))
			socket->SetHighlighted(isHighlighted({ CableInteraction::EndpointKind::TriggerOutput, {}, i }));
	}
}

actions::ActionResult GuiHud::OnAction(actions::KeyAction action)
{
	if (_cableDrag.has_value() && action.KeyChar == VK_ESCAPE && action.KeyActionType == actions::KeyAction::KEY_DOWN)
	{
		_CancelCableDrag();
		return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	return GuiPanel::OnAction(action);
}

std::vector<GuiHud::CableRoute> GuiHud::BuildCableRoutes(const engine::RoutingGraph& graph)
{
	std::vector<CableRoute> routes;
	for (const auto& trigger : graph.Triggers)
	{
		for (const auto& source : trigger.Sources)
			routes.push_back({ CableRoute::Kind::Capture, trigger.TriggerIndex, source, std::nullopt });
		if (trigger.StationIndex.has_value())
			routes.push_back({ CableRoute::Kind::Station, trigger.TriggerIndex, std::nullopt, trigger.StationIndex });
	}
	return routes;
}

Rect2d GuiHud::_ContentClip(const std::shared_ptr<GuiScrollPanel>& scroll) const
{
	return scroll ? scroll->EffectiveContentRect(Rect2d{ 0, 0,
		static_cast<int>(GetSize().Width), static_cast<int>(GetSize().Height) }.Translated(GlobalPosition())) : Rect2d{};
}

bool GuiHud::_SourceVisible(size_t index) const
{
	if (index >= _sourceEndpoints.size() || index >= _sourceWidgets.size())
		return false;
	const auto& scroll = _sourceEndpoints[index].Kind == io::RigFileRouting::SourceKind::Adc
		? _topAudioScroll : _topMidiScroll;
	if (!scroll)
		return false;
	const auto center = _sourceWidgets[index].Socket->GlobalPosition() +
		Position2d{ static_cast<int>(_SocketSize / 2u), static_cast<int>(_SocketSize / 2u) };
	const auto clip = _ContentClip(scroll);
	// Keep the whole hit target within its own viewport at the scroll edges.
	return clip.Contains(center) && center.X >= clip.Left + static_cast<int>(_SocketHitRadius) &&
		center.X < clip.Right - static_cast<int>(_SocketHitRadius);
}

void GuiHud::BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
	std::vector<CableInteraction::Cable>& cables) const
{
	endpoints.clear();
	cables.clear();
	const auto rootPos = GlobalPosition();
	std::vector<CableInteraction::Endpoint> sources;
	sources.reserve(_sourceWidgets.size());
	for (size_t i = 0u; i < _sourceWidgets.size() && i < _sourceEndpoints.size(); ++i)
	{
		const auto& source = _sourceEndpoints[i];
		sources.push_back({ source.Kind == io::RigFileRouting::SourceKind::Adc
			? CableInteraction::EndpointKind::AdcSource : CableInteraction::EndpointKind::MidiSource,
			_ElementCenter(_sourceWidgets[i].Socket),
			std::nullopt, std::nullopt, {}, source, source.Available,
			_ContentClip(source.Kind == io::RigFileRouting::SourceKind::Adc ? _topAudioScroll : _topMidiScroll)
				.Translated({ -rootPos.X, -rootPos.Y }) });
		if (_SourceVisible(i))
			endpoints.push_back(sources.back());
	}

	std::vector<CableInteraction::Endpoint> triggerInputs(_triggerWidgets.size());
	std::vector<CableInteraction::Endpoint> triggerOutputs(_triggerWidgets.size());
	std::vector<bool> triggerInputVisible(_triggerWidgets.size(), true);
	std::vector<bool> triggerOutputVisible(_triggerWidgets.size(), true);
	const auto triggerClip = _ContentClip(_triggerScroll).Translated({ -rootPos.X, -rootPos.Y });
	for (size_t i = 0u; i < _triggerWidgets.size(); ++i)
	{
		triggerInputs[i] = { CableInteraction::EndpointKind::TriggerInput,
			_ElementCenter(_triggerWidgets[i].InputSocket), i };
		triggerOutputs[i] = { CableInteraction::EndpointKind::TriggerOutput,
			_ElementCenter(_triggerWidgets[i].OutputSocket), i };
		triggerInputs[i].HitBounds = triggerClip;
		triggerOutputs[i].HitBounds = triggerClip;
		triggerInputVisible[i] = triggerClip.Contains(triggerInputs[i].Position);
		triggerOutputVisible[i] = triggerClip.Contains(triggerOutputs[i].Position);
		if (triggerInputVisible[i])
			endpoints.push_back(triggerInputs[i]);
		if (triggerOutputVisible[i])
			endpoints.push_back(triggerOutputs[i]);
	}

	std::vector<std::vector<size_t>> stationTriggers(_stationAnchors.size());
	for (const auto& trigger : _routingGraph)
		if (trigger.TriggerIndex < _triggerWidgets.size() && trigger.StationIndex.has_value())
		{
			const auto anchor = std::find_if(_stationAnchors.begin(), _stationAnchors.end(), [&trigger](const auto& value)
			{
				return value.StationIndex == trigger.StationIndex.value();
			});
			if (anchor != _stationAnchors.end())
				stationTriggers[static_cast<size_t>(std::distance(_stationAnchors.begin(), anchor))].push_back(trigger.TriggerIndex);
		}

	std::vector<std::optional<CableInteraction::Endpoint>> stationEnds(_triggerWidgets.size());
	for (size_t anchorIndex = 0u; anchorIndex < _stationAnchors.size(); ++anchorIndex)
	{
		const auto& anchor = _stationAnchors[anchorIndex];
		if (!anchor.ScreenPosition)
			continue;
		const auto offsets = CableInteraction::Spread(-7, 7,
			stationTriggers[anchorIndex].size());
		const Rect2d window{ 0, 0, static_cast<int>(GetSize().Width), static_cast<int>(GetSize().Height) };
		const auto actual = *anchor.ScreenPosition - glm::dvec2{ rootPos.X, rootPos.Y };
		const auto base = CableInteraction::ResolveBoundary(actual, window);
		if (!base)
			continue;
		for (size_t i = 0u; i < stationTriggers[anchorIndex].size(); ++i)
		{
			const auto fanned = CableInteraction::FannedPoint(actual, offsets[i], true, window);
			const auto point = CableInteraction::ResolveBoundary(fanned, window);
			if (!point)
				continue;
			CableInteraction::Endpoint endpoint{ CableInteraction::EndpointKind::Station,
				point->Position, stationTriggers[anchorIndex][i], anchor.StationIndex, anchor.StationName,
				{}, true, window, base->Clipped || point->Clipped, fanned };
			stationEnds[stationTriggers[anchorIndex][i]] = endpoint;
			if (!endpoint.Continuation)
				endpoints.push_back(endpoint);
		}
		const auto point = CableInteraction::ResolveBoundary(actual, window);
		if (point && !point->Clipped)
			endpoints.push_back({ CableInteraction::EndpointKind::Station,
				point->Position, std::nullopt, anchor.StationIndex, anchor.StationName, {}, true, window });
	}

	for (const auto& trigger : _routingGraph)
	{
		if (trigger.TriggerIndex >= triggerInputs.size())
			continue;
		// Connections come from routing identity, independent of socket visibility.
		{
			const auto pinY = triggerInputs[trigger.TriggerIndex].Position.Y;
			const auto ys = CableInteraction::Spread(pinY - _TriggerInputFanHalfHeight,
				pinY + _TriggerInputFanHalfHeight, trigger.Sources.size());
			for (size_t routeIndex = 0u; routeIndex < trigger.Sources.size(); ++routeIndex)
			{
				const auto& source = trigger.Sources[routeIndex];
				const auto sourceEndpoint = std::find_if(sources.begin(), sources.end(), [&source](const auto& endpoint)
				{
					return endpoint.Source.has_value() && endpoint.Source->Kind == source.Kind &&
						endpoint.Source->AdcChannel == source.AdcChannel && endpoint.Source->MidiDevice == source.MidiDevice;
				});
				if (sourceEndpoint == sources.end())
					continue;
				auto input = triggerInputs[trigger.TriggerIndex];
				input.ActualPosition = CableInteraction::FannedPoint(
					glm::dvec2{ input.Position.X, input.Position.Y }, ys[routeIndex] - pinY, false, triggerClip);
				input.Continuation = !triggerInputVisible[trigger.TriggerIndex];
				cables.push_back({ { _displayedRevision, trigger.TriggerIndex,
					CableInteraction::RouteKind::Capture, routeIndex }, *sourceEndpoint, input });
			}
		}
		if (trigger.TriggerIndex < stationEnds.size() &&
			stationEnds[trigger.TriggerIndex].has_value())
			cables.push_back({ { _displayedRevision, trigger.TriggerIndex,
				CableInteraction::RouteKind::Station, 0u }, triggerOutputs[trigger.TriggerIndex],
				stationEnds[trigger.TriggerIndex].value() });
	}
	// Each source cable gets its own visible end within the parent socket.
	for (const auto& sourceEndpoint : sources)
	{
		if (!sourceEndpoint.Source.has_value())
			continue;
		std::vector<size_t> routeIndices;
		for (size_t i = 0u; i < cables.size(); ++i)
			if (cables[i].Route.Kind == CableInteraction::RouteKind::Capture &&
				cables[i].Start.Source.has_value() &&
				cables[i].Start.Source->Kind == sourceEndpoint.Source->Kind &&
				cables[i].Start.Source->AdcChannel == sourceEndpoint.Source->AdcChannel &&
				cables[i].Start.Source->MidiDevice == sourceEndpoint.Source->MidiDevice)
				routeIndices.push_back(i);
		const auto xs = CableInteraction::Spread(sourceEndpoint.Position.X - 7,
			sourceEndpoint.Position.X + 7, routeIndices.size());
		for (size_t i = 0u; i < routeIndices.size(); ++i)
		{
			auto& start = cables[routeIndices[i]].Start;
			start.ActualPosition = CableInteraction::FannedPoint(
				glm::dvec2{ sourceEndpoint.Position.X, sourceEndpoint.Position.Y },
				xs[i] - sourceEndpoint.Position.X, true, *sourceEndpoint.HitBounds);
			start.Continuation = !sourceEndpoint.HitBounds->Contains(sourceEndpoint.Position);
		}
	}
	const auto present = [](CableInteraction::Endpoint& endpoint)
	{
		const auto actual = endpoint.ActualPosition.value_or(glm::dvec2{ endpoint.Position.X, endpoint.Position.Y });
		const auto point = endpoint.HitBounds ? CableInteraction::ResolveBoundary(actual, *endpoint.HitBounds) : std::nullopt;
		if (!point)
			return false;
		endpoint.ActualPosition = actual;
		endpoint.Position = point->Position;
		endpoint.Continuation = endpoint.Continuation || point->Clipped;
		return true;
	};
	// An empty effective viewport has no presentation. The routing graph remains
	// authoritative and unchanged, and will produce curves when space returns.
	std::erase_if(cables, [&present](auto& cable) { return !present(cable.Start) || !present(cable.Finish); });
}

void GuiHud::_RebuildCableVertices()
{
	_cableControlPoints.clear();
	_cableColors.clear();
	const glm::vec4 inputToTriggerColor(0.86f, 0.24f, 0.26f, 0.90f);
	const glm::vec4 triggerToStationColor(0.92f, 0.79f, 0.20f, 0.88f);
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	BuildInteractionGeometry(endpoints, cables);
	bool previewRenderable = true;
	if (_cableDrag && _cableDrag->FromCable)
	{
		const auto current = std::find_if(cables.begin(), cables.end(), [this](const auto& cable)
		{
			const auto& route = _cableDrag->Route;
			return cable.Route.Revision == route.Revision && cable.Route.TriggerIndex == route.TriggerIndex &&
				cable.Route.Kind == route.Kind && cable.Route.RouteIndex == route.RouteIndex;
		});
		previewRenderable = current != cables.end();
		if (previewRenderable)
			_cableDrag->Fixed = _cableDrag->MovingEnd == CableInteraction::End::Start ? current->Finish : current->Start;
	}
	for (const auto& cable : cables)
	{
		if (!_CableVisible(cable))
			continue;
		const bool selected = _hoveredCableRoute.has_value() &&
			_hoveredCableRoute->Revision == cable.Route.Revision &&
			_hoveredCableRoute->TriggerIndex == cable.Route.TriggerIndex &&
			_hoveredCableRoute->Kind == cable.Route.Kind &&
			_hoveredCableRoute->RouteIndex == cable.Route.RouteIndex;
		if (cable.Route.Kind == CableInteraction::RouteKind::Capture)
		{
			const auto color = cable.Start.Available ? inputToTriggerColor : glm::vec4(0.55f, 0.55f, 0.55f, 0.78f);
			_AppendCurve(cable.Start.Position, cable.Finish.Position,
				selected ? glm::vec4(glm::min(glm::vec3(color) * 1.35f, glm::vec3(1.0f)), 1.0f) : color);
			continue;
		}
		_AppendStationCurve(cable.Start.Position, cable.Finish.Position,
			selected ? glm::vec4(glm::min(glm::vec3(triggerToStationColor) * 1.3f, glm::vec3(1.0f)), 1.0f)
			: triggerToStationColor);
	}
	if (_cableDrag.has_value() && previewRenderable)
	{
		const auto preview = CableInteraction::Preview(_cableDrag.value());
		if (_cableDrag->Route.Kind == CableInteraction::RouteKind::Station)
			_AppendStationCurve(preview.first, preview.second, glm::vec4(0.35f, 0.82f, 1.0f, 0.95f));
		else
			_AppendCurve(preview.first, preview.second, glm::vec4(0.35f, 0.82f, 1.0f, 0.95f));
	}

	_cablesDirty = false;
}

utils::Position2d GuiHud::_ElementCenter(const std::shared_ptr<base::GuiElement>& element) const
{
	const auto rootPos = GlobalPosition();
	const auto elementPos = element->GlobalPosition();
	const auto elementSize = element->GetSize();
	return {
		elementPos.X - rootPos.X + static_cast<int>(elementSize.Width / 2u),
		elementPos.Y - rootPos.Y + static_cast<int>(elementSize.Height / 2u)
	};
}



void GuiHud::_AppendCurve(const utils::Position2d& start,
	const utils::Position2d& end,
	const glm::vec4& color)
{
	for (const auto& point : CableInteraction::CurveControls(CableInteraction::RouteKind::Capture, start, end))
		_cableControlPoints.emplace_back(point.x, point.y, 0.0f, 0.0f);
	_cableColors.push_back(color);
}

void GuiHud::_AppendStationCurve(const utils::Position2d& start,
	const utils::Position2d& end,
	const glm::vec4& color)
{
	for (const auto& point : CableInteraction::CurveControls(CableInteraction::RouteKind::Station, start, end))
		_cableControlPoints.emplace_back(point.x, point.y, 0.0f, 0.0f);
	_cableColors.push_back(color);
}
std::shared_ptr<GuiLabel> GuiHud::_MakeHeader(const std::string& text,
	unsigned int width,
	unsigned int horizontalInset) const
{
	auto params = GuiLabelParams::PanelHeader(text, width);
	params.Ellipsize = true;
	params.TextInsetX = static_cast<int>(horizontalInset);
	return std::make_shared<GuiLabel>(params);
}

void GuiHud::_OpenSourceIdentity(const std::string& identity)
{
	if (!_popupManager || GetSize().Width == 0u || GetSize().Height == 0u) return;
	_sourceIdentity = identity;
	_sourceInfoLabel->SetString(identity);
	_LayoutSourceIdentity();
	_sourceInfoScroll->SetScrollOffset(0);
	_CancelCableDrag();
	_popupManager->Open(_sourceInfoPanel, shared_from_this());
}

void GuiHud::_LayoutSourceIdentity()
{
	const auto width = std::min(480u, GetSize().Width);
	const auto height = std::min(112u, GetSize().Height);
	_sourceInfoPanel->SetSize({ width, height });
	_sourceInfoPanel->SetPosition({ static_cast<int>((GetSize().Width - width) / 2u),
		static_cast<int>((GetSize().Height - height) / 2u) });
	const unsigned int padding = std::min(12u, std::min(width, height) / 2u);
	const unsigned int inner = width - 2u * padding;
	if (auto header = _sourceInfoPanel->TryGetChild(0u))
	{
		header->SetPosition({ static_cast<int>(padding), std::max(0, static_cast<int>(height) - static_cast<int>(padding) - 22) });
		header->SetSize({ inner, 22u });
		header->SetVisible(height >= 46u);
	}
	_sourceInfoScroll->SetPosition({ static_cast<int>(padding), static_cast<int>(padding) });
	_sourceInfoScroll->SetSize({ inner, height > 46u ? height - 46u : 0u });
	const auto measured = static_cast<unsigned int>(std::ceil(_sourceInfoLabel->MeasureText(_sourceIdentity).value_or(0.0f)));
	_sourceInfoLabel->SetSize({ std::max(inner, measured + 4u), 28u });
	// Content extents changed in place; refresh metrics without replacing the tree.
	_sourceInfoScroll->SetSize(_sourceInfoScroll->GetSize());
}

std::shared_ptr<GuiButton> GuiHud::_MakeSourceButton(const std::string& text,
	const glm::vec3& tint,
	unsigned int width, bool midi, bool available)
{
	auto buttonParams = GuiButtonParams::PanelButton(width);
	buttonParams.Texture = "rounded_but";
	buttonParams.OverTexture = "rounded_but";
	buttonParams.DownTexture = "rounded_but";
	buttonParams.Size = { width, _SourceButtonHeight };
	buttonParams.MinSize = { 80u, _SourceButtonHeight };
	buttonParams.TintColor = GuiStyle::Graphite() * 0.5f + tint * 0.025f;
	auto button = std::make_shared<GuiHudActionButton>(buttonParams, [this, text]() { _OpenSourceIdentity(text); });
	const auto identity = available ? text : text.substr(0, text.size() - std::string(" (unavailable)").size());
	const auto heading = available ? (midi ? "MIDI" : "Audio") : "Offline";
	const auto name = midi ? identity.substr(5) : identity;
	const unsigned int lineHeight = (_SourceButtonHeight - 4u) / 2u;
	for (unsigned int line = 0; line < 2; ++line)
	{
		auto labelParams = GuiLabelParams::PanelScrollRow(line == 0 ? heading : name, 0u);
		labelParams.Position = { 4, static_cast<int>(2u + (1u - line) * lineHeight) };
		labelParams.Size = { width > 20u ? width - 20u : 0u, lineHeight };
		labelParams.Ellipsize = true;
		button->AddChild(std::make_shared<GuiLabel>(labelParams));
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
	const int pedalSizeH = static_cast<int>(_TriggerButtonHeight) - 38;
	const int pedalPosX = 16;
	const int pedalPosY = 8;

	base::GuiElementParams activateParams;
	activateParams.Position = { pedalPosX, pedalPosY };
	activateParams.Size = { static_cast<unsigned int>(pedalSizeW), static_cast<unsigned int>(pedalSizeH) };
	activateParams.TextureShader = "texture";
	activateParams.Texture = "trigger_activate";
	activateParams.OverTexture = "trigger_activate_over";
	activateParams.DownTexture = "trigger_activate_down";
	activateParams.OutTexture = "trigger_activate_down_out";
	activateParams.GuiPassThrough = false;
	button->AddChild(std::make_shared<GuiHudTriggerPedal>(activateParams, trigger, true,
		_displayedRevision, _acceptTriggerInput));

	base::GuiElementParams ditchParams;
	ditchParams.Position = { pedalPosX + pedalSizeW + socketPadding, pedalPosY };
	ditchParams.Size = { static_cast<unsigned int>(pedalSizeW), static_cast<unsigned int>(pedalSizeH) };
	ditchParams.TextureShader = "texture";
	ditchParams.Texture = "trigger_ditch";
	ditchParams.OverTexture = "trigger_ditch_over";
	ditchParams.DownTexture = "trigger_ditch_down";
	ditchParams.OutTexture = "trigger_ditch_down_out";
	ditchParams.GuiPassThrough = false;
	button->AddChild(std::make_shared<GuiHudTriggerPedal>(ditchParams, std::move(trigger), false,
		_displayedRevision, _acceptTriggerInput));

	GuiLabelParams labelParams = GuiLabelParams::PanelScrollRow(text, 0u);
	labelParams.Position = { static_cast<int>(_SocketSize + 4u), static_cast<int>(_TriggerButtonHeight) - 24 };
	labelParams.Size = { _TriggerButtonWidth - _SocketSize - _TriggerControlSize - 12u, 18u };
	labelParams.Ellipsize = true;
	labelParams.CenterHorizontally = true;
	button->AddChild(std::make_shared<GuiLabel>(labelParams));
	return button;
}
