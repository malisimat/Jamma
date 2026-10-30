#include "GuiVstRack.h"
#include "../graphics/GlDeleteQueue.h"
#include "../resources/ShaderResource.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

using namespace gui;
using namespace utils;
using namespace actions;

class GuiVstRack::Socket : public base::GuiElement
{
public:
	Socket(base::GuiElementParams params) : GuiElement(params) {}
	void ApplyHoverState(bool inside) override
	{
		_guiParams.TintColor = inside ? glm::vec3{1.0f, 0.90f, 0.35f} : glm::vec3{0.4f, 0.85f, 1.0f};
	}
	void ClearPointerState() override { ApplyHoverState(false); GuiElement::ClearPointerState(); }
};

class GuiVstRack::Button : public GuiButton
{
public:
	Button(GuiButtonParams params) : GuiButton(params) {}
	void SetSize(Size2d size) override
	{
		// Layout runs each frame; unchanged sizes must not dirty nine-patch buffers.
		if (GetSize().Width == size.Width && GetSize().Height == size.Height) return;
		GuiButton::SetSize(size);
	}
	std::function<Command()> Capture;
	std::function<void(Command)> Submit;
	bool KeepReadableWhenDisabled = false;
	void ClearPointerState() override { _press.reset(); GuiButton::ClearPointerState(); }
	ActionResult OnAction(TouchAction action) override
	{
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.Index != 0) return ActionResult::NoAction();
		if (!IsEnabled() || !IsVisible()) { _press.reset(); return ActionResult::NoAction(); }
		auto result = GuiButton::OnAction(action);
		if (action.State == TouchAction::TOUCH_DOWN && result.IsEaten && Capture)
		{
			_press = Capture(); _press->Modifiers = action.Modifiers;
			_indexDown = action.Index; _sourceDown = action.Touch;
		}
		else if (action.State == TouchAction::TOUCH_UP)
		{
			auto captured = std::exchange(_press, std::nullopt);
			if (captured && action.Index == _indexDown && action.Touch == _sourceDown && HitTest(action.Position) && Submit)
				Submit(std::move(*captured));
		}
		return result;
	}
	void Draw(base::DrawContext& context) override
	{
		auto opacity = context.WithOpacity(IsEnabled() || KeepReadableWhenDisabled ? 1.0f : 0.40f);
		GuiButton::Draw(context);
	}
private:
	std::optional<Command> _press;
	int _indexDown = 0;
	TouchAction::TouchType _sourceDown = TouchAction::TOUCH_MOUSE;
};

class GuiVstRack::Expander : public GuiToggle
{
public:
	Expander() : GuiToggle(_Params()) {}
	std::function<void()> ToggleChannels;
	void ClearPointerState() override { _press.reset(); GuiToggle::ClearPointerState(); }
	ActionResult OnAction(TouchAction action) override
	{
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.Index != 0) return ActionResult::NoAction();
		if (!IsEnabled() || !IsVisible()) { _press.reset(); return ActionResult::NoAction(); }
		if (action.State == TouchAction::TOUCH_UP)
		{
			const bool matched = _press && _press->first == action.Index && _press->second == action.Touch && HitTest(action.Position);
			_press.reset();
			if (!matched) return ActionResult::NoAction();
		}
		auto result = GuiToggle::OnAction(action);
		if (action.State == TouchAction::TOUCH_DOWN && result.IsEaten)
			_press = std::pair{action.Index, action.Touch};
		if (result.IsEaten && action.State == TouchAction::TOUCH_UP && ToggleChannels)
			ToggleChannels();
		return result;
	}
	ActionResult OnAction(KeyAction action) override
	{
		auto result = GuiToggle::OnAction(action);
		if (result.IsEaten && result.ResultType == ACTIONRESULT_TOGGLE && ToggleChannels)
			ToggleChannels();
		return result;
	}
private:
	std::optional<std::pair<int, TouchAction::TouchType>> _press;
	static GuiToggleParams _Params()
	{
		auto params = GuiToggleParams::RackExpander(true);
		params.Size = {32u, 64u}; params.MinSize = params.Size;
		return params;
	}
};

std::shared_ptr<GuiVstRack::Button> GuiVstRack::_MakeButton(
	std::string text, std::string texture, std::function<Command()> capture,
	graphics::FontOptions::FontSize fontSize)
{
	auto params = GuiButtonParams::PanelButton();
	params.Text = std::move(text); params.TextFontSize = fontSize;
	params.TextPadding = 4u; params.TextEllipsize = true;
	if (!texture.empty())
	{
		params.TextureShader = "texture"; params.Texture = texture;
		params.OverTexture = texture + "_over"; params.DownTexture = texture + "_down";
		if (texture == "plus") params.OutTexture = texture + "_down_out";
	}
	params.Size = {_width, _RowHeight}; params.MinSize = {1u, _RowHeight};
	auto button = std::make_shared<Button>(params);
	button->Capture = [life = std::weak_ptr<int>(_lifetime), capture = std::move(capture)] {
		return life.expired() ? Command{} : capture();
	};
	button->Submit = [this, life = std::weak_ptr<int>(_lifetime)](Command command) {
		if (!life.expired() && _submit) { if (command.Operation != Command::Kind::Diagnostic) _feedback.clear(); _submit(std::move(command)); }
	};
	return button;
}

GuiVstRack::GuiVstRack(std::shared_ptr<base::GuiElement> channels,
	const std::vector<std::shared_ptr<GuiToggle>>& toggles) :
	GuiElement(base::GuiElementParams{}), _channels(channels ? std::move(channels) : std::make_shared<base::GuiElement>(base::GuiElementParams{})), _toggles(toggles)
{
	GuiScrollPanelParams scroll; scroll.ScrollBarWidth = 8u; scroll.WheelStep = _RowPitch;
	scroll.MinSize = {1u, 1u}; scroll.Size = {_width, _RowPitch};
	_scroll = std::make_shared<GuiScrollPanel>(scroll);
	_content = std::make_shared<GuiPanel>(base::GuiElementParams{});
	_scroll->SetContent(_content);
	for (auto channel = 0u; channel < _toggles.size(); ++channel)
	{
		base::GuiElementParams socket; socket.Size = {8u, 8u}; socket.MinSize = socket.Size;
		socket.Texture = "hud_socket_cable"; socket.TextureShader = "texture_tinted";
		socket.TintColor = {0.4f, 0.85f, 1.0f};
		auto icon = std::make_shared<Socket>(socket); _channels->AddChild(icon); _sourceSockets.push_back(icon);
	}
	const auto command = [this](Command::Kind kind) { Command result; result.Operation = kind; result.Revision = _model.Revision; return result; };
	_append = _MakeButton("", "plus", [command] { return command(Command::Kind::Append); });
	_append->SetMinSize({_RowHeight, _RowHeight});
	_recover = _MakeButton("Resume", "", [command] { return command(Command::Kind::Recover); });
	_clearRoutes = _MakeButton("Clear MIDI", "", [command] { return command(Command::Kind::ClearInactiveRoutes); });
	_diagnostic = _MakeButton("Rack info", "", [command] { return command(Command::Kind::Diagnostic); });
	_collapse = std::make_shared<Expander>();
	_collapse->ToggleChannels = [this, life = std::weak_ptr<int>(_lifetime)] { if (!life.expired()) SetChannelsCollapsed(!_collapsed); };
	auto labelParams = GuiLabelParams::PanelHeader("MIDI", 148, 22u);
	labelParams.CenterHorizontally = true;
	labelParams.UseDarkFlatBackground();
	_midiLabel = std::make_shared<GuiLabel>(labelParams);
	_channels->AddChild(_midiLabel);
	_previous = _MakeButton("<", "", [command] { return command(Command::Kind::Diagnostic); });
	_next = _MakeButton(">", "", [command] { return command(Command::Kind::Diagnostic); });
	_previous->Submit = [this, life = std::weak_ptr<int>(_lifetime)](Command) { if (!life.expired() && _pageOffset >= _PageSize) _pageOffset -= _PageSize; };
	_next->Submit = [this, life = std::weak_ptr<int>(_lifetime)](Command) { if (!life.expired() && _pageOffset + _PageSize < _model.TotalSlots) _pageOffset += _PageSize; };
	AddChild(_channels); AddChild(_scroll); AddChild(_append); AddChild(_collapse);
	AddChild(_recover); AddChild(_clearRoutes); AddChild(_diagnostic);
	AddChild(_previous); AddChild(_next);
	_collapse->SetVisible(!_toggles.empty()); SetChannelsCollapsed(true);
	Layout(_width, _faderBottom, {-50, 42}, {32u, 64u});
}

GuiVstRack::~GuiVstRack()
{
	_lifetime.reset();
	ReleaseResources();
	_scroll->SetContent(nullptr);
}

std::shared_ptr<GuiButton> GuiVstRack::AppendButton() const { return _append; }
std::shared_ptr<GuiToggle> GuiVstRack::ChannelsExpander() const { return _collapse; }
std::shared_ptr<GuiButton> GuiVstRack::SlotButton(std::size_t index) const
{
	if (index >= _rows.size()) return {};
	for (const auto& button : _rows[index].Buttons) if (button->IsVisible()) return button;
	return {};
}

void GuiVstRack::Update(Model model, std::function<void(Command)> submit)
{
	if (!model.Editable || model.Inactive || (_drag && _drag->Revision != model.Revision)
		|| (_slotMove && (_slotMove->Move.Revision != model.Revision || _slotMove->Move.OwnerId != model.OwnerId
			|| _slotMove->Move.OwnerScope != model.OwnerScope))) CancelInteraction();
	bool rebuild = _model.Inactive != model.Inactive || _model.Slots.size() != model.Slots.size();
	if (!rebuild) for (std::size_t i = 0u; i < model.Slots.size(); ++i)
		if (_model.Slots[i].Value.Identity != model.Slots[i].Value.Identity) { rebuild = true; break; }
	if (_model.ChannelRoutes != model.ChannelRoutes) rebuild = true;
	if (!_rows.empty() && _rows.front().Ordinal != _pageOffset) rebuild = true;
	bool report = rebuild || !_rowsBuilt;
	if (!report) for (std::size_t i = 0u; i < model.Slots.size(); ++i)
		if (_model.Slots[i].Value.Plugin != model.Slots[i].Value.Plugin
			|| _model.Slots[i].Value.Pending != model.Slots[i].Value.Pending
			|| _model.Slots[i].Value.Failed != model.Slots[i].Value.Failed) { report = true; break; }
	_model = std::move(model); _submit = std::move(submit);
	_RefreshChannelToggles();
	if (_pageOffset >= std::max(std::size_t{1u}, _model.TotalSlots))
	{
		_pageOffset = _model.TotalSlots ? ((_model.TotalSlots - 1u) / _PageSize) * _PageSize : 0u;
		_model.Slots.clear(); rebuild = true; // The presenter obtains this page next frame.
	}
	if (rebuild || !_rowsBuilt) _RebuildRows();
	if (report)
	{
		std::clog << "[VST][rack] station='" << _model.OwnerName << "' saved-slots=" << _model.TotalSlots
			<< " page=" << _pageOffset << " presented-slots=" << _model.Slots.size()
			<< " rows=" << _rows.size() << " inactive=" << _model.Inactive << std::endl;
		for (const auto& slot : _model.Slots)
			std::clog << "[VST][rack] id='" << slot.Value.Identity << "' path='" << utils::EncodeUtf8(slot.Value.Path)
				<< "' processor=" << bool(slot.Value.Plugin) << " loaded=" << (slot.Value.Plugin && slot.Value.Plugin->IsLoaded())
				<< " pending=" << slot.Value.Pending << " failed=" << slot.Value.Failed << std::endl;
		for (const auto& row : _rows) if (row.Missing)
			std::cerr << "[ERROR][VST][rack] station='" << _model.OwnerName << "' missing cable target id='" << row.Id << "'" << std::endl;
	}
	for (std::size_t index = 0u; index < _model.Slots.size(); ++index)
	{
		auto& row = _rows[index]; const auto& slot = _model.Slots[index];
		const auto state = slot.Value.Pending ? 1u : slot.Value.Failed ? 3u : slot.Value.Plugin ? (slot.EditorOpen ? 2u : 1u) : 0u;
		row.Handle->SetEnabled(_model.Editable && !_model.Inactive && !slot.Value.Pending && _model.TotalSlots > 1u);
		row.Eligible = slot.Value.Plugin && slot.Value.Plugin->IsLoaded() && !slot.Value.Plugin->IsBypassed() && slot.Value.Plugin->MidiInput().Known
			&& (slot.Value.Plugin->MidiInput().Notes || slot.Value.Plugin->MidiInput().Controllers);
		const auto savedName = !slot.Value.Failed || slot.Value.Pending || slot.Value.Path.empty() ? std::string{} :
			utils::EncodeUtf8(std::filesystem::path(slot.Value.Path).stem().wstring());
		auto label = slot.Value.Pending ? "Loading..." : slot.Value.Plugin ? slot.Value.Plugin->Name() :
			slot.Value.Failed ? "Retry " + (savedName.empty() ? "VST" : savedName) : "Add VST";
		if (!_toggles.empty() && slot.Value.Plugin && !slot.Value.Plugin->MidiInput().Known) label = "MIDI? " + label;
		for (auto i = 0u; i < row.Buttons.size(); ++i)
		{
			row.Buttons[i]->SetVisible(i == state);
			row.Buttons[i]->SetEnabled(_model.Editable && !slot.Value.Pending);
			row.Buttons[i]->SetText(label.empty() ? "VST" : label);
		}
		const bool linked = std::any_of(_model.ChannelRoutes.begin(), _model.ChannelRoutes.end(), [&](const auto& route) {
			return std::find(route.PluginIds.begin(), route.PluginIds.end(), row.Id) != route.PluginIds.end();
		});
		row.Socket->SetVisible(!_model.Inactive && !_toggles.empty() && (row.Eligible || linked));
	}
	_append->SetText("");
	_append->SetEnabled(_model.Editable && !_model.Inactive && _model.TotalSlots < 32u);
	_recover->SetVisible(_model.Inactive); _recover->SetEnabled(_model.Editable && _model.TotalSlots <= 32u);
	_clearRoutes->SetVisible(_model.Inactive); _clearRoutes->SetEnabled(_model.Editable);
	_diagnostic->SetVisible(!_model.Diagnostic.empty() || !_feedback.empty());
	_diagnostic->SetText(_model.Inactive || _model.Suspended ? "Rack paused" : "Rack info");
	_collapse->SetEnabled(_model.Editable && !_model.Inactive);
	for (const auto& toggle : _toggles) toggle->SetEnabled(_model.Editable && !_model.Inactive);
	_previous->SetVisible(_model.TotalSlots > _PageSize); _next->SetVisible(_model.TotalSlots > _PageSize);
	_previous->SetEnabled(_pageOffset >= _PageSize); _next->SetEnabled(_pageOffset + _PageSize < _model.TotalSlots);
	_LayoutRows();
}

void GuiVstRack::_RebuildRows()
{
	_rowsBuilt = true;
	CancelInteraction();
	for (auto& row : _rows)
	{
		for (auto& button : row.Buttons)
		{
			button->ClearPointerState(); button->SetEnabled(false); button->SetVisible(false);
			button->ReleaseResources(); _content->RemoveChild(button); button->DetachGuiOwnershipForRetirement();
		}
		row.Socket->ReleaseResources(); _content->RemoveChild(row.Socket); row.Socket->DetachGuiOwnershipForRetirement();
		row.Handle->ReleaseResources(); _content->RemoveChild(row.Handle); row.Handle->DetachGuiOwnershipForRetirement();
	}
	_rows.clear();
	std::vector<std::string> identities;
	for (const auto& slot : _model.Slots) identities.push_back(slot.Value.Identity);
	if (!_model.Inactive)
		for (const auto& route : _model.ChannelRoutes)
			for (const auto& id : route.PluginIds)
				if (std::find(identities.begin(), identities.end(), id) == identities.end()) identities.push_back(id);
	for (std::size_t index = 0u; index < identities.size(); ++index)
	{
		Row row; row.Id = identities[index]; row.Ordinal = _pageOffset + index;
		row.Missing = index >= _model.Slots.size();
		const std::array<std::string, 4> textures{"vst_empty", "vst_closed", "vst_open", "vst_failed"};
		for (auto i = 0u; i < textures.size(); ++i)
		{
			row.Buttons[i] = _MakeButton(row.Missing ? "Missing" : "VST", textures[i],
				[this, id = row.Id, ordinal = row.Ordinal] { Command command; command.Identity = id;
					command.Ordinal = ordinal; command.Revision = _model.Revision; return command; },
				graphics::FontOptions::FONT_COMPACT);
			row.Buttons[i]->SetVisible(row.Missing ? i == 3u : i == 0u);
			row.Buttons[i]->SetEnabled(!row.Missing && _model.Editable);
			row.Buttons[i]->KeepReadableWhenDisabled = row.Missing;
			_content->AddChild(row.Buttons[i]);
		}
		row.Handle = _MakeButton("=", "", [] { return Command{}; });
		row.Handle->SetVisible(!row.Missing);
		row.Handle->SetEnabled(!row.Missing && _model.Editable);
		_content->AddChild(row.Handle);
		base::GuiElementParams socket; socket.Size = {10u, 10u}; socket.MinSize = socket.Size;
		socket.Texture = "hud_socket_cable"; socket.TextureShader = "texture_tinted";
		socket.TintColor = row.Missing ? glm::vec3{1.0f, 0.25f, 0.2f} : glm::vec3{0.4f, 0.85f, 1.0f};
		row.Socket = std::make_shared<base::GuiElement>(socket); _content->AddChild(row.Socket);
		_rows.push_back(std::move(row));
	}
	_resourcesNeedInitialising.store(true, std::memory_order_release);
}

void GuiVstRack::Layout(unsigned int width, int faderBottom,
	Position2d expanderPosition, Size2d expanderSize)
{
	_width = width; _faderBottom = faderBottom;
	SetSize({width, static_cast<unsigned int>(std::max(1, faderBottom))});
	const Size2d channelPanelSize{148u, 148u};
	_channels->SetSize(channelPanelSize);
	_channels->SetPosition({expanderPosition.X - static_cast<int>(channelPanelSize.Width) - 2, faderBottom});
	for (auto channel = 0u; channel < _toggles.size(); ++channel)
	{
		_toggles[channel]->SetPosition({static_cast<int>((channel % 4u) * 38u), static_cast<int>((3u - channel / 4u) * 38u)});
		_toggles[channel]->SetSize({32u, 32u});
		_sourceSockets[channel]->SetPosition({static_cast<int>((channel % 4u) * 38u + 25u), static_cast<int>((3u - channel / 4u) * 38u + 12u)});
	}
	_midiLabel->SetPosition({0, 154}); _midiLabel->SetSize({148u, 22u});
	_collapse->SetPosition(expanderPosition); _collapse->SetSize(expanderSize);
	_LayoutRows();
}

void GuiVstRack::_LayoutRows()
{
	const auto rowsHeight = static_cast<unsigned int>(_rows.size()) * _RowPitch + 4u;
	const int top = _faderBottom - 4;
	unsigned int footerRows = _previous->IsVisible() ? 1u : 0u;
	for (const auto& control : {_append, _recover, _clearRoutes, _diagnostic})
		if (control->IsVisible()) ++footerRows;
	const int available = std::clamp(GlobalPosition().Y + top - GuiStyle::StatusBarHeight - static_cast<int>(footerRows * _RowPitch) - 4, 40, 280);
	const auto height = std::min(rowsHeight, static_cast<unsigned int>(available));
	const auto scrollHeight = _rows.empty() ? 0u : height;
	_scroll->SetPosition({0, top - static_cast<int>(scrollHeight)}); _scroll->SetSize({_width, scrollHeight});
	_scroll->SetVisible(!_rows.empty());
	_content->SetSize({_width, rowsHeight});
	// Keep the button borders inside the scroll panel's inset clipping rectangle.
	const auto contentRect = _scroll->ContentRect();
	const auto rowWidth = static_cast<unsigned int>(std::max(1, contentRect.Right - contentRect.Left));
	for (std::size_t i = 0u; i < _rows.size(); ++i)
	{
		const auto y = static_cast<int>(rowsHeight - (i + 1u) * _RowPitch);
		for (auto& button : _rows[i].Buttons) { button->SetPosition({contentRect.Left, y}); button->SetSize({std::max(1u, rowWidth > 18u ? rowWidth - 18u : 1u), _RowHeight}); }
		_rows[i].Socket->SetPosition({contentRect.Left + 1, y + 13});
		_rows[i].Handle->SetPosition({contentRect.Left + static_cast<int>(rowWidth) - 18, y});
		_rows[i].Handle->SetSize({18u, _RowHeight});
	}
	_scroll->SetSize({_width, scrollHeight}); // Refresh content metrics without resetting scroll.
	int bottom = top - static_cast<int>(scrollHeight) - static_cast<int>(_RowPitch);
	for (const auto& control : {_append, _recover, _clearRoutes, _diagnostic})
	{
		if (control == _append)
		{
			control->SetPosition({std::max(0, (static_cast<int>(_width) - static_cast<int>(_RowHeight)) / 2), bottom + 4});
			control->SetSize({_RowHeight, _RowHeight});
		}
		else { control->SetPosition({0, bottom}); control->SetSize({_width, _RowHeight}); }
		if (control->IsVisible()) bottom -= static_cast<int>(_RowPitch);
	}
	_previous->SetPosition({0, bottom}); _next->SetPosition({static_cast<int>(_width / 2u), bottom});
	_previous->SetSize({_width / 2u, _RowHeight}); _next->SetSize({_width - _width / 2u, _RowHeight});
}

void GuiVstRack::SetViewport(Size2d viewport) { _viewport = viewport; _LayoutRows(); }
void GuiVstRack::SetFeedback(std::string message) { _feedback = std::move(message); }
void GuiVstRack::SetChannelsCollapsed(bool collapsed)
{
	CancelInteraction(); _collapsed = collapsed; _channels->SetVisible(!collapsed);
	_collapse->SetToggleState(collapsed ? GuiToggleParams::TOGGLE_OFF : GuiToggleParams::TOGGLE_ON, true);
}

void GuiVstRack::SetAllowedMidiChannels(const std::vector<int>& channels)
{
	_allowedChannels.fill(false);
	for (const auto channel : channels)
		if (channel >= 1 && channel <= 16) _allowedChannels[static_cast<std::size_t>(channel - 1)] = true;
	_RefreshChannelToggles();
}

bool GuiVstRack::ChannelShownActive(unsigned int channel) const noexcept
{
	if (channel >= _allowedChannels.size()) return false;
	return _allowedChannels[channel];
}

void GuiVstRack::_RefreshChannelToggles()
{
	for (auto channel = 0u; channel < _toggles.size() && channel < 16u; ++channel)
		_toggles[channel]->SetToggleState(ChannelShownActive(channel) ? GuiToggleParams::TOGGLE_ON : GuiToggleParams::TOGGLE_OFF, true);
}
void GuiVstRack::CancelInteraction()
{
	if (_drag || _slotMove) _consumeRelease = true;
	if (_slotMove) _feedback.clear();
	_slotMove.reset();
	for (const auto& row : _rows) row.Handle->ApplyHoverState(false);
	if (_channelPress) _toggles[_channelPress->Channel]->ClearPointerState();
	_channelPress.reset();
	_drag.reset(); _hovered = false;
	if (_hoveredMidiSocket) _toggles[*_hoveredMidiSocket]->ApplyHoverState(false);
	_hoveredMidiSocket.reset();
	for (const auto& socket : _sourceSockets) socket->ApplyHoverState(false);
}
void GuiVstRack::ClearPointerState() { CancelInteraction(); GuiElement::ClearPointerState(); }
void GuiVstRack::SetVisible(bool visible)
{
	if (!visible) { ClearPointerState(); _revealHeld = false; }
	GuiElement::SetVisible(visible);
}
bool GuiVstRack::IsInView(Size2d viewport) const
{
	if (!IsVisible()) return false;
	const Rect2d window{0, 0, static_cast<int>(viewport.Width), static_cast<int>(viewport.Height)};
	for (const std::shared_ptr<GuiButton>& control : {std::static_pointer_cast<GuiButton>(_append), std::static_pointer_cast<GuiButton>(_collapse)})
	{
		const auto p = control->GlobalPosition(); const auto s = control->GetSize();
		if (!Rect2d{p.X, p.Y, p.X + static_cast<int>(s.Width), p.Y + static_cast<int>(s.Height)}.Intersected(window).IsEmpty()) return true;
	}
	return !_scroll->EffectiveContentRect(window).IsEmpty();
}

void GuiVstRack::_Geometry(std::vector<CableInteraction::Endpoint>& endpoints,
	std::vector<CableInteraction::Cable>& cables, std::vector<Link>& links) const
{
	if (_model.Inactive || _toggles.size() != 16u) return;
	const auto origin = GlobalPosition();
	for (auto channel = 0u; channel < _toggles.size(); ++channel)
	{
		CableInteraction::Endpoint endpoint; endpoint.Kind = CableInteraction::EndpointKind::RackMidiChannel;
		endpoint.MidiChannel = channel;
		const auto control = _collapsed ? std::static_pointer_cast<GuiButton>(_collapse) : std::static_pointer_cast<GuiButton>(_toggles[channel]);
		const auto p = control->GlobalPosition() - origin; const auto s = control->GetSize();
		endpoint.Position = {p.X + static_cast<int>(s.Width) - 3, p.Y + static_cast<int>(s.Height / 2u)};
		if (_collapsed) endpoint.Position.Y += static_cast<int>(channel) - 8;
		endpoint.Continuation = _collapsed;
		endpoints.push_back(std::move(endpoint));
	}
	const auto clip = _scroll->EffectiveContentRect({0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)})
		.Translated({-origin.X, -origin.Y});
	for (const auto& row : _rows)
	{
		CableInteraction::Endpoint endpoint; endpoint.Kind = CableInteraction::EndpointKind::RackVstPlugin;
		endpoint.PluginId = row.Id; endpoint.Available = row.Eligible && !_model.Suspended;
		const auto p = row.Socket->GlobalPosition() - origin;
		endpoint.Position = {p.X + 5, p.Y + 5};
		if (const auto bounded = CableInteraction::ResolveBoundary(glm::dvec2{endpoint.Position.X, endpoint.Position.Y}, clip))
		{ endpoint.Position = bounded->Position; endpoint.Continuation = bounded->Clipped; }
		else endpoint.Continuation = true;
		endpoints.push_back(std::move(endpoint));
	}
	for (auto channel = 0u; channel < 16u; ++channel)
		for (const auto& id : _model.ChannelRoutes[channel].PluginIds)
		{
			const auto finish = std::find_if(endpoints.begin() + 16u, endpoints.end(), [&](const auto& endpoint) { return endpoint.PluginId == id; });
			if (finish == endpoints.end()) continue;
			CableInteraction::Cable cable;
			cable.Route.Revision = _model.Revision; cable.Route.Kind = CableInteraction::RouteKind::StationRack;
			cable.Route.RouteIndex = links.size(); cable.Start = endpoints[channel]; cable.Finish = *finish;
			cables.push_back(std::move(cable)); links.push_back({channel, id});
		}
}

void GuiVstRack::BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
	std::vector<CableInteraction::Cable>& cables) const
{
	std::vector<Link> links; _Geometry(endpoints, cables, links);
}

ActionResult GuiVstRack::_Captured()
{
	return {true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, shared_from_this()};
}

bool GuiVstRack::_BeginDrag(Position2d point)
{
	if (!_model.Editable || _model.Inactive || _model.Suspended) return false;
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	const auto end = CableInteraction::HitCableEnd(cables, point, 9.0f);
	const auto cable = end ? std::optional<std::size_t>(end->first) : CableInteraction::HitCable(cables, point, 5.0f);
	if (cable)
	{
		const auto& connection = cables[*cable];
		if (connection.Start.Continuation) return false; // Expand channels to edit their individual identities.
		const auto moving = end ? end->second : CableInteraction::ClosestEnd(connection, point);
		if ((moving == CableInteraction::End::Start ? connection.Start : connection.Finish).Continuation) return false;
		const bool source = moving == CableInteraction::End::Start;
		if (source && !connection.Finish.Available) return false;
		const auto& link = links[*cable];
		_drag = Drag{_model.Revision, source, link, link.Channel, link.Id, point, {}};
	}
	else if (const auto index = CableInteraction::HitEndpoint(endpoints, point, 9.0f))
	{
		const auto& endpoint = endpoints[*index];
		if (endpoint.Continuation || !endpoint.Available) return false;
		const bool source = endpoint.Kind == CableInteraction::EndpointKind::RackMidiChannel;
		_drag = Drag{_model.Revision, !source, {}, endpoint.MidiChannel.value_or(0u), endpoint.PluginId, point, {}};
	}
	if (!_drag) return false;
	_UpdateDrag(point); return true;
}

void GuiVstRack::_UpdateDrag(Position2d point)
{
	if (!_drag) return;
	_drag->Pointer = point; _drag->Snap.reset();
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	if (!_drag->MoveSource && endpoints.size() == 16u + _rows.size())
	{
		// Drop onto the visible slot control while keeping the cable anchored to its socket.
		const auto origin = GlobalPosition();
		const auto clip = _scroll->EffectiveContentRect({0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)});
		for (std::size_t i = 0u; i < _rows.size(); ++i)
		{
			const auto& control = _rows[i].Buttons[0];
			const auto p = control->GlobalPosition(); const auto s = control->GetSize();
			const auto bounds = Rect2d{p.X, p.Y, p.X + static_cast<int>(s.Width), p.Y + static_cast<int>(s.Height)}.Intersected(clip);
			const auto& endpoint = endpoints[16u + i];
			if (endpoint.Available && bounds.Contains(point + origin))
			{ _drag->Snap = endpoint; return; }
		}
	}
	for (const auto& endpoint : endpoints)
		if (!endpoint.Continuation && endpoint.Available &&
			((endpoint.Kind == CableInteraction::EndpointKind::RackMidiChannel) == _drag->MoveSource) &&
			CableInteraction::HitTest(endpoint, point, 13.0f)) { _drag->Snap = endpoint; break; }
}

void GuiVstRack::_RemoveLink(Routes& routes, const Link& link) const
{
	auto& route = routes[link.Channel]; std::erase(route.PluginIds, link.Id);
}

void GuiVstRack::_FinishDrag()
{
	const auto drag = std::exchange(_drag, std::nullopt);
	if (!drag || drag->Revision != _model.Revision || !_model.Editable) return;
	Command command; command.Operation = Command::Kind::Routes; command.Revision = drag->Revision;
	command.ChannelRoutes = _model.ChannelRoutes;
	if (!drag->Snap && drag->Original)
	{
		std::vector<CableInteraction::Endpoint> endpoints; std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
		_Geometry(endpoints, cables, links);
		if (CableInteraction::HitEndpoint(endpoints, drag->Pointer, 13.0f))
		{ SetFeedback("This socket cannot accept that cable."); return; }
	}
	if (drag->Original)
	{
		if (drag->Snap && !drag->MoveSource) std::erase(command.ChannelRoutes[drag->Original->Channel].PluginIds, drag->Original->Id);
		else _RemoveLink(command.ChannelRoutes, *drag->Original);
	}
	if (drag->Snap)
	{
		const auto channel = drag->MoveSource ? drag->Snap->MidiChannel.value() : drag->Channel;
		const auto& id = drag->MoveSource ? drag->Id : drag->Snap->PluginId;
		if (drag->Original && channel == drag->Original->Channel && id == drag->Original->Id) return;
		auto& route = command.ChannelRoutes[channel];
		if (std::find(route.PluginIds.begin(), route.PluginIds.end(), id) == route.PluginIds.end()) route.PluginIds.push_back(id);
	}
	else if (!drag->Original) return;
	if (command.ChannelRoutes != _model.ChannelRoutes && _submit) _submit(std::move(command));
}

void GuiVstRack::MovePointer(TouchMoveAction action)
{
	if (_hoveredMidiSocket) _toggles[*_hoveredMidiSocket]->ApplyHoverState(false);
	_hoveredMidiSocket.reset();
	for (const auto& socket : _sourceSockets) socket->ApplyHoverState(false);
	if (!IsVisible()) { _hovered = false; return; }
	if (_slotMove) { _UpdateSlotMove(action.Position); return; }
	if (_drag) { _UpdateDrag(action.Position); return; }
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	const auto hoveredEndpoint = CableInteraction::HitEndpoint(endpoints, action.Position, 12.0f);
	if (!_collapsed && hoveredEndpoint && endpoints[*hoveredEndpoint].MidiChannel)
	{
		_hoveredMidiSocket = *endpoints[*hoveredEndpoint].MidiChannel;
		_sourceSockets[*_hoveredMidiSocket]->ApplyHoverState(true);
		_toggles[*_hoveredMidiSocket]->ApplyHoverState(true);
	}
	_hovered = CableInteraction::HitEndpoint(endpoints, action.Position, 12.0f).has_value()
		|| CableInteraction::HitCable(cables, action.Position, 7.0f).has_value()
		|| (_collapsed && _collapse->HitTest(_collapse->ParentToLocal(action.Position)));
	const auto point = action.Position + GlobalPosition();
	const auto hitsControl = [point](const auto& control, Rect2d clip)
	{
		if (!control->IsVisible()) return false;
		const auto p = control->GlobalPosition(); const auto s = control->GetSize();
		return Rect2d{p.X, p.Y, p.X + static_cast<int>(s.Width), p.Y + static_cast<int>(s.Height)}
			.Intersected(clip).Contains(point);
	};
	const auto window = Rect2d{0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)};
	if (!_collapsed)
		for (const auto& toggle : _toggles)
			_hovered = _hovered || hitsControl(toggle, window);
	const auto clip = _scroll->EffectiveContentRect(window);
	for (const auto& row : _rows)
		for (const auto& button : row.Buttons)
			_hovered = _hovered || hitsControl(button, clip);
}

ActionResult GuiVstRack::OnAction(TouchMoveAction action)
{
	if (_slotMove)
	{
		if (action.Index != _slotMove->Index || action.Touch != _slotMove->Touch) return _Captured();
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u) CancelInteraction();
		else _UpdateSlotMove(action.Position);
		return _Captured();
	}
	if (_channelPress && action.Index == _channelPress->Index && action.Touch == _channelPress->Touch)
	{
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u)
		{
			CancelInteraction(); return _Captured();
		}
		if (!_drag && (std::abs(action.Position.X - _channelPress->Position.X) > 4 ||
			std::abs(action.Position.Y - _channelPress->Position.Y) > 4))
		{
			const auto channel = _channelPress->Channel;
			if (_model.Editable && !_model.Inactive && !_model.Suspended)
			{
				_drag = Drag{_model.Revision, false, {}, channel, {}, action.Position, {}};
				_UpdateDrag(action.Position);
			}
		}
		// Keep the pressed channel lit even after the pointer leaves its bounds.
		if (_drag) _toggles[_channelPress->Channel]->ApplyHoverState(true);
	}
	MovePointer(action);
	if (_drag || _channelPress) return _Captured();
	return GuiElement::OnAction(action);
}

ActionResult GuiVstRack::OnAction(TouchAction action)
{
	if (!IsVisible() || !IsEnabled()) return ActionResult::NoAction();
	if (_consumeRelease && action.State == TouchAction::TOUCH_DOWN && action.Index == 0) _consumeRelease = false;
	if (_consumeRelease)
	{
		if (action.State == TouchAction::TOUCH_UP && action.Index == 0) _consumeRelease = false;
		return _Captured();
	}
	if (_slotMove)
	{
		if (action.Index == 1 && action.State == TouchAction::TOUCH_DOWN) CancelInteraction();
		else if (action.Index == _slotMove->Index && action.Touch == _slotMove->Touch && action.State == TouchAction::TOUCH_UP)
		{ _UpdateSlotMove(action.Position); _FinishSlotMove(); }
		return _Captured();
	}
	if (_drag)
	{
		if (action.Index == 1 && action.State == TouchAction::TOUCH_DOWN) CancelInteraction();
		else if (action.Index == 0 && action.State == TouchAction::TOUCH_UP)
		{
			_UpdateDrag(action.Position); _FinishDrag();
			if (_channelPress) _toggles[_channelPress->Channel]->ClearPointerState();
			_channelPress.reset();
		}
		return _Captured();
	}
	if (_channelPress && action.State == TouchAction::TOUCH_UP)
	{
		const auto press = std::exchange(_channelPress, std::nullopt);
		if (action.Index == press->Index && action.Touch == press->Touch)
		{
			action.Position = _toggles[press->Channel]->GlobalToLocal(action.Position + GlobalPosition());
			return _toggles[press->Channel]->OnAction(action);
		}
		_toggles[press->Channel]->ClearPointerState();
		return _Captured();
	}
	if (action.Index == 0 && action.State == TouchAction::TOUCH_DOWN)
	{
		// Disconnect all cables on this channel. Enabled channels then broadcast.
		if ((action.Modifiers & base::Action::MODIFIER_ALT) && !_collapsed && _model.Editable && !_model.Inactive)
		{
			std::vector<CableInteraction::Endpoint> endpoints; std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
			_Geometry(endpoints, cables, links);
			for (const auto& endpoint : endpoints)
				if (endpoint.MidiChannel && CableInteraction::HitTest(endpoint, action.Position, 9.0f))
				{
					Command command; command.Operation = Command::Kind::Routes; command.Revision = _model.Revision;
					command.ChannelRoutes = _model.ChannelRoutes; command.ChannelRoutes[*endpoint.MidiChannel] = {};
					if (_submit) _submit(std::move(command)); _consumeRelease = true; return _Captured();
				}
		}
		if (_BeginSlotMove(action) || _BeginDrag(action.Position)) return _Captured();
	}
	auto result = GuiElement::OnAction(action);
	if (action.Index == 0 && action.State == TouchAction::TOUCH_DOWN && !_collapsed && result.IsEaten)
	{
		for (auto channel = 0u; channel < _toggles.size(); ++channel)
			if (result.ActiveElement.lock() == _toggles[channel])
			{
				_channelPress = ChannelPress{channel, action.Position, action.Index, action.Touch};
				result.ActiveElement = shared_from_this();
				break;
			}
	}
	return result;
}

ActionResult GuiVstRack::OnAction(KeyAction action)
{
	if (action.KeyChar == 27u && (_drag || _slotMove || _consumeEscape))
	{
		if (action.KeyActionType == KeyAction::KEY_DOWN) { CancelInteraction(); _consumeEscape = true; }
		else _consumeEscape = false;
		return {true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {}};
	}
	return GuiElement::OnAction(action);
}

bool GuiVstRack::RouteHitTest(Position2d point)
{
	if (!IsEnabled() || !IsVisible()) return false;
	std::vector<CableInteraction::Endpoint> endpoints; std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	return _slotMove.has_value() || _drag.has_value() || CableInteraction::HitEndpoint(endpoints, point, 9.0f).has_value()
		|| CableInteraction::HitCable(cables, point, 5.0f).has_value() || _ChildRectHitTest(point);
}

std::shared_ptr<base::GuiElement> GuiVstRack::FindTopmostDescendant(Position2d point)
{
	// Handles own their capture, independently of MIDI cable gestures.
	if (IsEnabled() && IsVisible())
	{
		const auto global = point + GlobalPosition();
		const auto clip = _scroll->EffectiveContentRect({0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)});
		for (const auto& row : _rows)
			if (row.Handle->IsVisible() && row.Handle->IsEnabled() && clip.Contains(global)
				&& row.Handle->HitTest(row.Handle->GlobalToLocal(global))) return shared_from_this();
	}
	// Socket/cable gestures precede the underlying slot/channel buttons.
	std::vector<CableInteraction::Endpoint> endpoints; std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	if (IsEnabled() && IsVisible() && (CableInteraction::HitEndpoint(endpoints, point, 9.0f) || CableInteraction::HitCable(cables, point, 5.0f))) return shared_from_this();
	return GuiElement::FindTopmostDescendant(point);
}

void GuiVstRack::_InitResources(resources::ResourceLib& resources, bool force)
{
	GuiElement::_InitResources(resources, force);
	if (const auto resource = resources.GetResource("cable"); resource)
		_cableShader = std::dynamic_pointer_cast<resources::ShaderResource>(resource->lock());
	glGenVertexArrays(1, &_vertexArray); glBindVertexArray(_vertexArray);
	glGenBuffers(1, &_vertexBuffer); glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2u, nullptr, GL_DYNAMIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0); glBindVertexArray(0);
}

void GuiVstRack::_ReleaseResources()
{
	GuiElement::_ReleaseResources();
	graphics::GlDeleteQueue::DeleteBuffers(1, &_vertexBuffer);
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexBuffer = _vertexArray = 0u; _cableShader.reset();
}

void GuiVstRack::Draw(base::DrawContext& context)
{
	if (!IsVisible()) return;
	// Scene hover picking may select the socket/rack instead of the channel button.
	if (_hoveredMidiSocket)
	{
		_toggles[*_hoveredMidiSocket]->ApplyHoverState(true);
		_sourceSockets[*_hoveredMidiSocket]->ApplyHoverState(true);
	}
	GuiElement::Draw(context);
	auto& gl = dynamic_cast<graphics::GlDrawContext&>(context);
	const auto p = Position(); gl.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(p.X, p.Y, 0.0f)));
	_DrawCables(context); gl.PopMvp();
}

void GuiVstRack::_DrawCables(base::DrawContext& context)
{
	if (!_revealHeld && !_hovered && !_drag) return;
	const auto shader = _cableShader.lock(); if (!shader || !_vertexArray) return;
	std::vector<CableInteraction::Endpoint> endpoints; std::vector<CableInteraction::Cable> cables; std::vector<Link> links;
	_Geometry(endpoints, cables, links);
	if (_drag)
	{
		const auto fixed = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
			return _drag->MoveSource ? endpoint.PluginId == _drag->Id : endpoint.MidiChannel == _drag->Channel;
		});
		if (fixed != endpoints.end())
		{
			CableInteraction::Cable preview; preview.Route.Kind = CableInteraction::RouteKind::StationRack;
			CableInteraction::Endpoint moving; moving.Position = _drag->Snap ? _drag->Snap->Position : _drag->Pointer;
			preview.Start = _drag->MoveSource ? moving : *fixed; preview.Finish = _drag->MoveSource ? *fixed : moving;
			cables.push_back(std::move(preview)); links.push_back({_drag->Channel, _drag->Id});
		}
	}
	auto& gl = dynamic_cast<graphics::GlDrawContext&>(context);
	const auto program = shader->GetId(); glUseProgram(program); shader->SetUniforms(gl);
	glUniform1i(glGetUniformLocation(program, "SegmentCount"), CableInteraction::CurveVertexCount);
	glBindVertexArray(_vertexArray);
	for (const auto width : {6.5f, 3.2f})
	{
		glLineWidth(width);
		for (std::size_t first = 0u; first < cables.size(); first += 16u)
		{
			std::array<glm::vec4, 64> points{}; std::array<glm::vec4, 16> colors{};
			const auto count = std::min(std::size_t{16u}, cables.size() - first);
			for (std::size_t i = 0u; i < count; ++i)
			{
				const auto& cable = cables[first + i];
				const auto curve = CableInteraction::CurveControls(CableInteraction::RouteKind::StationRack, cable.Start.Position, cable.Finish.Position);
				for (auto point = 0u; point < 4u; ++point) points[i * 4u + point] = {curve[point].x, curve[point].y, 0.0f, 0.0f};
				const auto channel = links[first + i].Channel;
			const bool enabled = _allowedChannels[channel] && !_model.Suspended;
				const float hue = static_cast<float>((channel * 7u) % 16u) / 15.0f;
			colors[i] = enabled && cable.Finish.Available
				? glm::vec4{0.3f + hue * 0.6f, 0.9f - hue * 0.4f, 1.0f - hue * 0.3f, 0.85f}
				: glm::vec4{0.55f, 0.55f, 0.55f, 0.55f};
				if (width > 4.0f) colors[i] *= glm::vec4{0.45f, 0.45f, 0.45f, 1.0f};
				colors[i].a *= context.Opacity();
			}
			glUniform4fv(glGetUniformLocation(program, "CableControlPoints"), static_cast<GLsizei>(count * 4u), reinterpret_cast<const GLfloat*>(points.data()));
			glUniform4fv(glGetUniformLocation(program, "CableColors"), static_cast<GLsizei>(count), reinterpret_cast<const GLfloat*>(colors.data()));
			glUniform1i(glGetUniformLocation(program, "CableCount"), static_cast<int>(count));
			glDrawArraysInstanced(GL_LINE_STRIP, 0, CableInteraction::CurveVertexCount, static_cast<GLsizei>(count));
		}
	}
	glBindVertexArray(0); glUseProgram(0);
}

std::shared_ptr<GuiButton> GuiVstRack::MoveHandle(std::size_t index) const
{
	return index < _rows.size() ? _rows[index].Handle : nullptr;
}

bool GuiVstRack::_BeginSlotMove(const TouchAction& action)
{
	if (!_model.Editable || _model.Inactive) return false;
	const auto point = action.Position + GlobalPosition();
	const auto clip = _scroll->EffectiveContentRect({0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)});
	if (!clip.Contains(point)) return false;
	for (const auto& row : _rows)
		if (row.Handle->IsEnabled() && row.Handle->IsVisible() && row.Handle->HitTest(row.Handle->GlobalToLocal(point)))
		{
			vst::RackMove move{_model.OwnerScope, _model.OwnerId, row.Id, row.Ordinal, row.Ordinal, _model.Revision};
			_slotMove = SlotMove{std::move(move), row.Ordinal, action.Index, action.Touch};
			_UpdateSlotMove(action.Position);
			return true;
		}
	return false;
}

void GuiVstRack::_UpdateSlotMove(Position2d point)
{
	if (!_slotMove) return;
	_slotMove->Target.reset();
	point = point + GlobalPosition();
	const auto clip = _scroll->EffectiveContentRect({0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height)});
	for (const auto& row : _rows)
	{
		const auto p = row.Buttons[0]->GlobalPosition();
		const auto size = row.Buttons[0]->GetSize();
		const bool target = !row.Missing && Rect2d{p.X, p.Y, p.X + static_cast<int>(size.Width) + 18, p.Y + static_cast<int>(_RowHeight)}
			.Intersected(clip).Contains(point);
		row.Handle->ApplyHoverState(target);
		if (target) _slotMove->Target = row.Ordinal;
	}
	SetFeedback(_slotMove->Target ? "Move to slot " + std::to_string(*_slotMove->Target + 1u) : "Outside rack: cancel move");
}

void GuiVstRack::_FinishSlotMove()
{
	auto move = std::exchange(_slotMove, std::nullopt);
	for (const auto& row : _rows) row.Handle->ApplyHoverState(false);
	_feedback.clear();
	if (!move || !move->Target || move->Move.Revision != _model.Revision || !_model.Editable
		|| *move->Target == move->Move.Source) return;
	move->Move.Target = *move->Target;
	Command command; command.Operation = Command::Kind::Move; command.Revision = move->Move.Revision;
	command.Identity = move->Move.PluginId; command.Ordinal = move->Move.Source; command.Move = std::move(move->Move);
	if (_submit) _submit(std::move(command));
}
