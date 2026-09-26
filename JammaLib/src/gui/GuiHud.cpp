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

		void Draw(base::DrawContext& ctx) override
		{
			const auto previousTint = _guiParams.TintColor;
			_guiParams.TintColor = IsEnabled() ? glm::vec3(1.0f) : glm::vec3(0.38f);
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
			params.TintColor = glm::vec3(1.0f);
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
	if (_triggerList)
		_triggerList->ComputeLayout();
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

	for (auto& vu : _inputVus)
		vu->Draw(ctx);
	for (const auto& widgets : _sourceWidgets)
		_DrawOverlayElement(ctx, widgets.Socket);
	if (_triggerScroll)
	{
		glCtx.PushScissorRect(_triggerScroll->GlobalPosition(),
			{ _triggerScroll->ViewportWidth(), _triggerScroll->ViewportHeight() });
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
	_LayoutPanels();
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
	_cableEndIcon->InitResources(resourceLib, forceInit);
	_stationSocketIcon->InitResources(resourceLib, forceInit);

	GlUtils::CheckError("GuiHud::_InitResources()");
}

void GuiHud::_ReleaseResources()
{
	graphics::GlDeleteQueue::DeleteBuffers(1, &_cableVertexBuffer);
	_cableVertexBuffer = 0;

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_cableVertexArray);
	_cableVertexArray = 0;

	for (auto& vu : _inputVus)
		vu->ReleaseResources();
	if (_deletePopup)
		_deletePopup->ReleaseResources();
	_cableEndIcon->ReleaseResources();
	_stationSocketIcon->ReleaseResources();
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

	base::GuiElementParams railParams;
	railParams.Size = { _RightRailWidth - 6u, _RightRailHeight };
	railParams.MinSize = { _RightRailWidth - 6u, _RightRailMinHeight };
	railParams.TextureShader = "texture_tinted";
	railParams.Texture = "rounded_but";
	railParams.TintColor = glm::vec3(0.17f, 0.20f, 0.24f);
	_triggerRail = std::make_shared<GuiPanel>(railParams);
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

	const unsigned int totalInputs = static_cast<unsigned int>(_sourceEndpoints.size());
	const unsigned int innerWidth = _TopStripWidth - (_TopStripPadding * 2u);
	const unsigned int totalSpacing = totalInputs > 1u ? GuiStackPanelParams::PanelRowSpacing * (totalInputs - 1u) : 0u;
	const unsigned int widthBudget = innerWidth > totalSpacing ? innerWidth - totalSpacing : innerWidth;
	const unsigned int sourceButtonWidth = totalInputs > 0u ? std::max(64u, widthBudget / totalInputs) : _SourceButtonWidth;

	for (const auto& source : _sourceEndpoints)
	{
		const auto isAdc = source.Kind == io::RigFileRouting::SourceKind::Adc;
		auto label = isAdc ? "Audio In " + std::to_string(source.AdcChannel + 1u) :
			"MIDI " + source.MidiDevice;
		if (!source.Available)
			label += " (unavailable)";
		auto button = _MakeSourceButton(label,
			source.Available ? (isAdc ? glm::vec3(0.92f, 0.52f, 0.24f) : glm::vec3(0.22f, 0.72f, 0.66f)) : glm::vec3(0.50f),
			sourceButtonWidth);
		auto socket = std::make_shared<GuiHudSocket>(
			utils::Position2d{ static_cast<int>(sourceButtonWidth / 2u - _SocketSize / 2u), 0 },
			_SocketSize,
			source.Available
			? (isAdc ? glm::vec3(0.92f, 0.52f, 0.24f) : glm::vec3(0.22f, 0.72f, 0.66f))
			: glm::vec3(0.35f));
		button->AddChild(socket);
		_sourceWidgets.push_back({ button, socket });
		_topInputRow->AddChild(button);
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

	_topStrip->AddChild(_topInputRow);
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
	_addTriggerButton = std::make_shared<GuiHudActionButton>(addParams, [this]() { _AddTrigger(); });
	_triggerRail->AddChild(_addTriggerButton);

	GuiLabelParams statusParams = GuiLabelParams::PanelScrollRow("Trigger routing ready", 0u);
	statusParams.Size = { 300u, GuiLabelParams::RowHeight };
	statusParams.MinSize = { 160u, GuiLabelParams::RowHeight };
	_routingStatusLabel = std::make_shared<GuiLabel>(statusParams);
	AddChild(_routingStatusLabel);
}

void GuiHud::_RebuildPanels()
{
	const int previousScrollOffset = _triggerScroll ? _triggerScroll->ScrollOffset() : 0;
	const bool revealNewest = _revealNewestTrigger;
	_sourceWidgets.clear();
	_triggerWidgets.clear();
	_inputVus.clear();
	_topStrip.reset();
	_topInputRow.reset();
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
	_lastTriggerScrollOffset = _triggerScroll ? _triggerScroll->ScrollOffset() : 0;
	_hoveredCableEndpoint.reset();
	_hoveredCableRoute.reset();
	_hoveredCableEnd.reset();
	_cablesDirty = true;
}

void GuiHud::SetCableRevealHeld(bool held)
{
	_cableRevealHeld = held;
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
		_sourceEndpoints.push_back({ io::RigFileRouting::SourceKind::Adc, channel, {}, channel < routing.Rig.User.Audio.NumChannelsIn });
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
	const auto railInnerHeight = _triggerRail->GetSize().Height;
	const unsigned int headerHeight = 34u;
	const unsigned int scrollHeight = railInnerHeight > headerHeight + _TriggerFooterHeight
		? railInnerHeight - headerHeight - _TriggerFooterHeight : 1u;
	if (_triggerScroll)
	{
		_triggerScroll->SetPosition({ 0, static_cast<int>(_TriggerFooterHeight) });
		_triggerScroll->SetSize({ _RightRailWidth - 6u, scrollHeight });
	}
	if (auto header = _triggerRail->TryGetChild(0u))
		header->SetPosition({ 0, static_cast<int>(railInnerHeight - headerHeight) });
	if (_addTriggerButton)
	{
		_addTriggerButton->SetPosition({ static_cast<int>((_RightRailWidth - 6u - _TriggerControlSize) / 2u), 10 });
	}
	if (_routingStatusLabel)
		_routingStatusLabel->SetPosition({ railPosX - 310, railPosY + 14 });
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

actions::ActionResult GuiHud::_BeginCableDrag(Position2d point)
{
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	_BuildInteractionGeometry(endpoints, cables);
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
		const auto movingEnd = cableEnd.has_value() ? cableEnd->second : CableInteraction::ClosestEnd(cable, point);
		const auto& fixed = movingEnd == CableInteraction::End::Start ? cable.Finish : cable.Start;
		if (!fixed.Available || (fixed.Source.has_value() && !fixed.Source->Available))
			return actions::ActionResult::NoAction();
		_cableDrag = CableInteraction::Drag{ cable.Route,
			movingEnd,
			fixed,
			cable.Route.Kind == CableInteraction::RouteKind::Capture ? cable.Start.Source : std::nullopt,
			point,
			std::nullopt };
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
	_cableRevealHeld = true;
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr,
		std::static_pointer_cast<base::GuiElement>(shared_from_this()) };
}

void GuiHud::_CancelCableDrag()
{
	CableInteraction::Cancel(_cableDrag);
	_UpdateSocketHighlights();
	_cablesDirty = true;
}

void GuiHud::_DrawCableSockets(base::DrawContext& ctx)
{
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	_BuildInteractionGeometry(endpoints, cables);
	for (const auto& anchor : _stationAnchors)
	{
		if (anchor.screenPos.X < -1000)
			continue;
		_stationSocketIcon->SetPosition({ anchor.screenPos.X - static_cast<int>(_SocketSize / 2u),
			anchor.screenPos.Y - static_cast<int>(_SocketSize / 2u) });
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
		_cableEndIcon->SetTint(cable.Route.Kind == CableInteraction::RouteKind::Capture
			? glm::vec3(0.86f, 0.24f, 0.26f) : glm::vec3(0.92f, 0.79f, 0.20f));
		_cableEndIcon->SetHighlighted(selected);
		_cableEndIcon->SetPosition({ endpoint.Position.X - static_cast<int>(_CableEndSize / 2u),
			endpoint.Position.Y - static_cast<int>(_CableEndSize / 2u) });
		_cableEndIcon->Draw(ctx);
	};
	for (const auto& cable : cables)
	{
		if (cable.Start.Kind != CableInteraction::EndpointKind::TriggerInput &&
			cable.Start.Kind != CableInteraction::EndpointKind::TriggerOutput)
			drawEnd(cable, CableInteraction::End::Start);
		if (cable.Finish.Kind != CableInteraction::EndpointKind::TriggerInput &&
			cable.Finish.Kind != CableInteraction::EndpointKind::TriggerOutput)
			drawEnd(cable, CableInteraction::End::Finish);
	}
	if (_triggerScroll)
	{
		auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
		glCtx.PushScissorRect(_triggerScroll->GlobalPosition(),
			{ _triggerScroll->ViewportWidth(), _triggerScroll->ViewportHeight() });
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
	_deletePopup->SetPosition({ std::max(0, static_cast<int>(GetSize().Width / 2u) - 230),
		std::max(0, static_cast<int>(GetSize().Height / 2u) - 105) });
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
	if (drag.Route.Revision == _displayedRevision)
	{
		const auto release = CableInteraction::ReleaseToCandidate(drag, _displayedRig);
		if (release.Changed && release.Candidate.has_value())
			_SubmitCandidate(release.Candidate.value());
	}
	_CancelCableDrag();
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {} };
}

actions::ActionResult GuiHud::OnAction(actions::TouchMoveAction action)
{
	if (!_cableDrag.has_value())
	{
		_UpdateCableHover(action.Position);
		// Keep visual feedback immediate while still resolving exactly one leaf.
		// The scene's deferred path repeats the same topmost selection during draw.
		ApplyExclusiveHoverPoint(action.Position);
		return actions::ActionResult::NoAction();
	}
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	_BuildInteractionGeometry(endpoints, cables);
	CableInteraction::Update(_cableDrag.value(), action.Position, endpoints, _displayedRig,
		_SnapRadius, _SnapHysteresis);
	_UpdateSocketHighlights();
	_cablesDirty = true;
	return { true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr,
		std::static_pointer_cast<base::GuiElement>(shared_from_this()) };
}

void GuiHud::_UpdateCableHover(Position2d point)
{
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	_BuildInteractionGeometry(endpoints, cables);
	const auto cableEnd = CableInteraction::HitCableEnd(cables, point, _CableEndSize);
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

void GuiHud::_BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
	std::vector<CableInteraction::Cable>& cables) const
{
	endpoints.clear();
	cables.clear();
	const auto rootPos = GlobalPosition();
	for (size_t i = 0u; i < _sourceWidgets.size() && i < _sourceEndpoints.size(); ++i)
	{
		const auto& source = _sourceEndpoints[i];
		endpoints.push_back({ source.Kind == io::RigFileRouting::SourceKind::Adc
			? CableInteraction::EndpointKind::AdcSource : CableInteraction::EndpointKind::MidiSource,
			_ElementCenter(_sourceWidgets[i].Socket),
			std::nullopt, std::nullopt, {}, source, source.Available });
	}

	std::vector<CableInteraction::Endpoint> triggerInputs(_triggerWidgets.size());
	std::vector<CableInteraction::Endpoint> triggerOutputs(_triggerWidgets.size());
	std::vector<bool> triggerInputVisible(_triggerWidgets.size(), true);
	std::vector<bool> triggerOutputVisible(_triggerWidgets.size(), true);
	const auto scrollPos = _triggerScroll ? _triggerScroll->GlobalPosition() : utils::Position2d{};
	const int scrollBottom = scrollPos.Y - rootPos.Y;
	const int scrollTop = scrollBottom + (_triggerScroll ? static_cast<int>(_triggerScroll->ViewportHeight()) : 0);
	for (size_t i = 0u; i < _triggerWidgets.size(); ++i)
	{
		triggerInputs[i] = { CableInteraction::EndpointKind::TriggerInput,
			_ElementCenter(_triggerWidgets[i].InputSocket), i };
		triggerOutputs[i] = { CableInteraction::EndpointKind::TriggerOutput,
			_ElementCenter(_triggerWidgets[i].OutputSocket), i };
		triggerInputVisible[i] = !_triggerScroll ||
			(triggerInputs[i].Position.Y >= scrollBottom && triggerInputs[i].Position.Y <= scrollTop);
		triggerOutputVisible[i] = !_triggerScroll ||
			(triggerOutputs[i].Position.Y >= scrollBottom && triggerOutputs[i].Position.Y <= scrollTop);
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
		if (anchor.screenPos.X < -1000)
			continue;
		const auto xs = CableInteraction::Spread(anchor.screenPos.X - 7, anchor.screenPos.X + 7,
			stationTriggers[anchorIndex].size() + 1u);
		for (size_t i = 0u; i < stationTriggers[anchorIndex].size(); ++i)
		{
			CableInteraction::Endpoint endpoint{ CableInteraction::EndpointKind::Station,
				{ xs[i], anchor.screenPos.Y }, stationTriggers[anchorIndex][i], anchor.StationIndex, anchor.StationName };
			stationEnds[stationTriggers[anchorIndex][i]] = endpoint;
			endpoints.push_back(endpoint);
		}
		endpoints.push_back({ CableInteraction::EndpointKind::Station,
			anchor.screenPos, std::nullopt, anchor.StationIndex, anchor.StationName });
	}

	for (const auto& trigger : _routingGraph)
	{
		if (trigger.TriggerIndex >= triggerInputs.size())
			continue;
		// Keep the cable fan centred on the visible socket, even while scrolling.
		if (triggerInputVisible[trigger.TriggerIndex])
		{
			const auto pinY = triggerInputs[trigger.TriggerIndex].Position.Y;
			const auto ys = CableInteraction::Spread(pinY - _TriggerInputFanHalfHeight,
				pinY + _TriggerInputFanHalfHeight, trigger.Sources.size());
			for (size_t routeIndex = 0u; routeIndex < trigger.Sources.size(); ++routeIndex)
			{
				const auto& source = trigger.Sources[routeIndex];
				const auto sourceEndpoint = std::find_if(endpoints.begin(), endpoints.end(), [&source](const auto& endpoint)
				{
					return endpoint.Source.has_value() && endpoint.Source->Kind == source.Kind &&
						endpoint.Source->AdcChannel == source.AdcChannel && endpoint.Source->MidiDevice == source.MidiDevice;
				});
				if (sourceEndpoint == endpoints.end())
					continue;
				auto input = triggerInputs[trigger.TriggerIndex];
				input.Position.Y = ys[routeIndex];
				cables.push_back({ { _displayedRevision, trigger.TriggerIndex,
					CableInteraction::RouteKind::Capture, routeIndex }, *sourceEndpoint, input });
			}
		}
		if (triggerOutputVisible[trigger.TriggerIndex] && trigger.TriggerIndex < stationEnds.size() &&
			stationEnds[trigger.TriggerIndex].has_value())
			cables.push_back({ { _displayedRevision, trigger.TriggerIndex,
				CableInteraction::RouteKind::Station, 0u }, triggerOutputs[trigger.TriggerIndex],
				stationEnds[trigger.TriggerIndex].value() });
	}
	// Each source cable gets its own visible end within the parent socket.
	for (const auto& sourceEndpoint : endpoints)
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
			cables[routeIndices[i]].Start.Position.X = xs[i];
	}
}

void GuiHud::_RebuildCableVertices()
{
	_cableControlPoints.clear();
	_cableColors.clear();
	const glm::vec4 inputToTriggerColor(0.86f, 0.24f, 0.26f, 0.90f);
	const glm::vec4 triggerToStationColor(0.92f, 0.79f, 0.20f, 0.88f);
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	_BuildInteractionGeometry(endpoints, cables);
	for (const auto& cable : cables)
	{
		if (!_cableRevealHeld && !_cableDrag.has_value() &&
			(!_hoveredCableEndpoint.has_value() ||
				!CableInteraction::Related(cable, _hoveredCableEndpoint.value())))
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
	if (_cableDrag.has_value())
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
