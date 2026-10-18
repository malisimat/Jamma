#include "GuiMainPanel.h"
#include "GuiLabel.h"
#include "../graphics/GlDrawContext.h"
#include <cmath>

using namespace gui;
using namespace base;
using namespace actions;
using namespace utils;

GuiMainPanel::GuiMainPanel(GuiMainPanelParams params) : GuiPanel(params),
	_selectionOnly(params.SelectionOnly), _page(params.SelectionOnly ? SettingsPage::Selection : SettingsPage::Timing),
	_popups(params.PopupManager), _beforeHide(std::move(params.BeforeHide))
{
	_guiParams.GuiPassThrough = true;
	GuiElementParams frameParams;
	frameParams.GuiPassThrough = false;
	// Share the flatter fill texture with the HUD panels.
	frameParams.Texture = "panel_fill_flat";
	frameParams.TextureShader = "texture_tinted";
	frameParams.TintColor = GuiStyle::Graphite();
	frameParams.TextureOpacity = GuiStyle::PanelFillOpacity;
	_frame = std::make_shared<GuiPanel>(frameParams);
	_children.push_back(_frame);
	if (!_selectionOnly)
	{
		auto headerParams = GuiLabelParams::PanelHeader("Settings", 320u);
		headerParams.Ellipsize = true;
		_frame->AddChild(std::make_shared<GuiLabel>(headerParams));
	}
	GuiToggleParams handleParams = GuiToggleParams::PanelPrimary();
	handleParams.Text = "";
	handleParams.Texture = "arrow";
	handleParams.OverTexture = "arrow_over";
	handleParams.DownTexture = "arrow_down";
	handleParams.ToggledTexture = "arrowup2";
	handleParams.ToggledOverTexture = "arrowup2_over";
	handleParams.ToggledDownTexture = "arrowup2_down";
	handleParams.Rot90 = !_selectionOnly;
	handleParams.TextPadding = 0u;
	handleParams.Size = _selectionOnly ? Size2d{ 64u, 20u } : Size2d{ 20u, 64u };
	handleParams.MinSize = handleParams.Size;
	handleParams.InitState = GuiToggleParams::TOGGLE_ON;
	// Match rack handles: preserve the arrow textures' authored colours.
	handleParams.TextureShader = "texture";
	_handle = std::make_shared<GuiToggle>(handleParams);
	_expandBinding = std::make_shared<GuiCommandReceiver>(_ExpandCommand);
	_handle->SetReceiver(_expandBinding);
	_children.push_back(_handle);
	for (auto& page : _pages)
	{
		GuiStackPanelParams pageParams;
		pageParams.PaddingH = 4u;
		pageParams.PaddingV = 4u;
		pageParams.Spacing = 8u;
		pageParams.Size = { 300u, 8u };
		page = std::make_shared<GuiStackPanel>(pageParams);
	}
	for (const auto& setting : params.Settings)
	{
		if (!setting.Control || setting.Page >= SettingsPage::Count) continue;
		auto& page = _pages[static_cast<size_t>(setting.Page)];
		if (!setting.Label.empty())
		{
			const auto labelHeight = setting.Label.size() > 30u
				? 2u * GuiLabelParams::DefaultHeight : GuiLabelParams::DefaultHeight;
			auto label = GuiLabelParams::PanelHeader(setting.Label, 320, labelHeight);
			label.FontSize = graphics::FontOptions::FONT_SMALL;
			page->AddChild(std::make_shared<GuiLabel>(label));
		}
		page->AddChild(setting.Control);
		_controlSizes.emplace_back(setting.Control, setting.Control->GetSize());
		auto binding = std::make_shared<GuiCommandReceiver>(setting.Command);
		setting.Control->SetReceiver(binding);
		_bindings.push_back(std::move(binding));
	}
	if (!_selectionOnly)
	{
		auto save = GuiButtonParams::PanelButton(260u);
		save.Text = "Save rig as default..."; save.DispatchCommand = true;
		save.Size.Height = 40u; save.MinSize.Height = 40u;
		save.TextFontSize = graphics::FontOptions::FONT_SMALL;
		_saveRigButton = std::make_shared<GuiButton>(save);
		_pages[static_cast<size_t>(SettingsPage::Timing)]->AddChild(_saveRigButton);
		_controlSizes.emplace_back(_saveRigButton, save.Size);
		auto binding = std::make_shared<GuiCommandReceiver>(SaveRigDefaultControlIndex);
		_saveRigButton->SetReceiver(binding);
		_bindings.push_back(std::move(binding));
	}
	if (_selectionOnly)
	{
		for (std::size_t index = 0; index < _groupRows.size(); ++index)
		{
			_groupRows[index] = std::make_shared<GuiPerformanceGroupRow>(index == 1u);
			_pages[static_cast<size_t>(_page)]->AddChild(_groupRows[index]);
		}
	}
	GuiScrollPanelParams scrollParams;
	scrollParams.ScrollBarWidth = 12u;
	_pageScroll = std::make_shared<GuiScrollPanel>(scrollParams);
	_pageScroll->SetContent(_pages[static_cast<size_t>(_page)]);
	_frame->AddChild(_pageScroll);
	if (!_selectionOnly)
	{
		GuiRadioParams tabParams;
		tabParams.InitValue = static_cast<unsigned int>(SettingsPage::Timing);
		tabParams.Size = { 416u, 48u };
		for (unsigned int index = 0; index < 4; ++index)
		{
			auto toggle = GuiToggleParams::PanelPrimary();
			toggle.Text = std::array<std::string, 4>{ "Audio", "MIDI", "Timing", "File" }[index];
			toggle.TintColor = GuiStyle::Control();
			toggle.Position = { static_cast<int>(index * 104u), 0 };
			toggle.Size = { 100u, 48u };
			tabParams.ToggleParams.push_back(toggle);
		}
		_tabs = std::make_shared<GuiRadio>(tabParams);
		_pageBinding = std::make_shared<GuiCommandReceiver>(_PageCommand);
		_tabs->SetReceiver(_pageBinding);
		scrollParams.Orientation = GuiScrollOrientation::Horizontal;
		_tabScroll = std::make_shared<GuiScrollPanel>(scrollParams);
		_tabScroll->SetContent(_tabs);
		_frame->AddChild(_tabScroll);
	}
	SetViewportSize(params.Size);
}

void GuiMainPanel::SetPerformanceGroupHandler(std::function<void(bool, engine::GroupCommand, std::size_t)> handler)
{
	if (!_selectionOnly) return;
	for (std::size_t i = 0u; i < _groupRows.size(); ++i)
		_groupRows[i]->SetHandler([handler, i](engine::GroupCommand command, std::size_t slot) {
			if (handler) handler(i == 1u, command, slot);
		});
}

void GuiMainPanel::RefreshPerformanceGroups(const engine::PerformanceGroups& groups)
{
	if (!_selectionOnly) return;
	const auto refresh = [](const auto& model, const auto& widget) {
		std::array<bool, 9> occupied{};
		for (std::size_t slot = 1u; slot <= occupied.size(); ++slot) occupied[slot - 1u] = model.Get(slot) != nullptr;
		widget->SetSlots(occupied, model.ActiveSlot());
	};
	refresh(groups.Mute, _groupRows[0]);
	refresh(groups.Selection, _groupRows[1]);
}

void GuiMainPanel::SetCommandOwner(std::weak_ptr<ActionReceiver> owner)
{
	_commandOwner = owner;
	for (auto& binding : _bindings) binding->SetOwner(owner);
	for (auto& page : _pageBindings) for (auto& binding : page) binding->SetOwner(owner);
}

std::optional<std::pair<bool, std::size_t>> GuiMainPanel::HoveredPerformanceGroup() const
{
	if (!_selectionOnly || !_expanded || !IsVisible() || !IsEnabled() || !_frame->IsVisible())
		return std::nullopt;
	for (std::size_t i = 0u; i < _groupRows.size(); ++i)
		if (const auto slot = _groupRows[i]->HoveredSlot()) return std::pair(i == 1u, *slot);
	return std::nullopt;
}

void GuiMainPanel::ClearPerformanceGroupPointerState()
{
	for (const auto& row : _groupRows)
		if (row) row->ClearPointerState();
}

void GuiMainPanel::_InitReceivers()
{
	if (_expandBinding) _expandBinding->SetOwner(ActionReceiver::shared_from_this());
	if (_pageBinding) _pageBinding->SetOwner(ActionReceiver::shared_from_this());
}

void GuiMainPanel::_PrepareHide(const std::shared_ptr<GuiElement>& subtree)
{
	subtree->FinalizeEdits();
	subtree->ClearPointerState();
	if (_popups) _popups->CloseOwnedBy(subtree);
	if (_beforeHide) _beforeHide(subtree);
}

void GuiMainPanel::SetPage(SettingsPage page)
{
	if (_selectionOnly || (page > SettingsPage::Session) || page == _page) return;
	_PrepareHide(_pages[static_cast<size_t>(_page)]);
	_offsets[static_cast<size_t>(_page)] = _pageScroll->ScrollOffset();
	_page = page;
	_pageScroll->SetContent(_pages[static_cast<size_t>(_page)]);
	_tabs->SetCurrentValue(static_cast<unsigned int>(page), true);
	_Layout();
	_pageScroll->SetScrollOffset(_offsets[static_cast<size_t>(_page)]);
}

void GuiMainPanel::SetExpanded(bool expanded)
{
	if (_expanded == expanded) return;
	if (!expanded) _PrepareHide(_frame);
	_expanded = expanded;
	_handle->SetToggleState(expanded ? GuiToggleParams::TOGGLE_ON : GuiToggleParams::TOGGLE_OFF, true);
	_pageScroll->SetEnabled(expanded);
	for (const auto& row : _groupRows) if (row) row->SetEnabled(expanded);
	if (_tabScroll) _tabScroll->SetEnabled(expanded);
	_UpdatePresentation();
}

float GuiMainPanel::PresentedOpacity() const
{
	return _transition * _transition * (3.0f - 2.0f * _transition);
}

bool GuiMainPanel::AdvanceAnimation(float elapsedSeconds)
{
	if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f) return false;
	const float target = _expanded ? 1.0f : 0.0f;
	bool changed = false;
	for (const auto& row : _groupRows) if (row) changed = row->AdvanceAnimation(elapsedSeconds) || changed;
	if (_transition == target) return changed;
	const float step = std::min(elapsedSeconds, 0.05f) / GuiStyle::PanelTransitionSeconds;
	_transition = _expanded ? std::min(target, _transition + step) : std::max(target, _transition - step);
	_UpdatePresentation();
	return true;
}

void GuiMainPanel::_UpdatePresentation()
{
	const int margin = std::min(8, static_cast<int>(_viewport.Width) / 2);
	const auto size = _frame->GetSize();
	const float hidden = 1.0f - PresentedOpacity();
	_frame->SetPosition(_selectionOnly
		? Position2d{ margin, static_cast<int>(_viewport.Height) - margin - static_cast<int>(size.Height) + static_cast<int>(std::lround(hidden * (size.Height + _handle->GetSize().Height))) }
		: Position2d{ margin - static_cast<int>(std::lround(hidden * (size.Width + margin))), std::min(GuiStyle::StatusBarHeight + 8, static_cast<int>(_viewport.Height) / 2) });
	_frame->SetVisible(_transition > 0.0f && size.Width > 0u && size.Height > 0u);
}

void GuiMainPanel::Draw(DrawContext& context)
{
	if (!IsVisible() || _viewport.Width == 0u || _viewport.Height == 0u) return;
	auto& glContext = dynamic_cast<graphics::GlDrawContext&>(context);
	const auto position = Position();
	glContext.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(position.X, position.Y, 0.0f)));
	glContext.PushScissorRect(GlobalPosition(), _viewport);
	{
		auto opacity = context.WithOpacity(PresentedOpacity());
		_frame->Draw(context);
	}
	if (_selectionOnly && _expanded)
	{
		auto opacity = context.WithOpacity(PresentedOpacity());
		for (const auto& row : _groupRows)
			if (!_pageScroll->EffectiveContentRect({ 0, 0, static_cast<int>(_viewport.Width), static_cast<int>(_viewport.Height) }).Intersected(
				{ row->GlobalPosition().X, row->GlobalPosition().Y, row->GlobalPosition().X + static_cast<int>(row->GetSize().Width), row->GlobalPosition().Y + static_cast<int>(row->GetSize().Height) }).IsEmpty())
				row->DrawTooltip(context, GlobalPosition());
	}
	// The persistent edge handle stays opaque and above the sliding body.
	_handle->Draw(context);
	glContext.PopScissorRect();
	glContext.PopMvp();
}

ActionResult GuiMainPanel::OnAction(GuiAction action)
{
	if (const auto value = std::get_if<GuiAction::GuiInt>(&action.Data))
	{
		if (action.Index == _PageCommand) SetPage(static_cast<SettingsPage>(value->Value));
		else if (action.Index == _ExpandCommand) SetExpanded(value->Value == GuiToggleParams::TOGGLE_ON);
		else return ActionResult::NoAction();
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	return ActionResult::NoAction();
}

void GuiMainPanel::SetViewportSize(Size2d viewport)
{
	_viewport = viewport;
	GuiElement::SetSize(viewport);
	_Layout();
}

void GuiMainPanel::SetOccupiedTopStripBounds(Rect2d bounds)
{
	if (_occupiedTopStrip.Left == bounds.Left && _occupiedTopStrip.Right == bounds.Right &&
		_occupiedTopStrip.Bottom == bounds.Bottom && _occupiedTopStrip.Top == bounds.Top) return;
	_occupiedTopStrip = bounds;
	_Layout();
}

void GuiMainPanel::_Layout()
{
	const int width = static_cast<int>(_viewport.Width), height = static_cast<int>(_viewport.Height);
	const int margin = std::min(StandardPanelPadding, width / 2);
	const int panelWidth = std::min(_selectionOnly ? 404 : _SettingsPanelWidth(), std::max(0, width - 2 * margin));
	const int handleWidth = std::min(_selectionOnly ? 64 : 20, std::max(0, width - (_selectionOnly ? margin : 0)));
	const int handleHeight = std::min(_selectionOnly ? SelectionHandleHeight : 64, std::max(0, height - (_selectionOnly ? 0 : std::min(StandardPanelPadding, height / 2))));
	const int selectionPanelHeight = std::min(_SelectionPanelHeight(),
		std::max(0, height - 2 * std::min(StandardPanelPadding, height / 2)));
	const int mainPanelBottom = std::min(GuiStyle::StatusBarHeight + StandardPanelPadding, height / 2);
	const int selectionPanelBottom = height - margin - selectionPanelHeight;
	const int settingsAvailableHeight = std::max(0, selectionPanelBottom - MainPanelTopGap - mainPanelBottom);
	// Preserve usable settings chrome only when the viewport cannot fit both
	// overlays. Otherwise scrolling keeps settings entirely below selection.
	constexpr int settingsChromeHeight = 2 * 12 + 22 + 48 + 8;
	const int settingsHeight = settingsAvailableHeight >= settingsChromeHeight
		? settingsAvailableHeight : MainPanelMinimumHeight;
	const int unconstrainedPanelHeight = _selectionOnly
		? selectionPanelHeight
		: std::min(std::max(0, height - mainPanelBottom),
			settingsHeight);
	const int panelHeight = !_selectionOnly && !_occupiedTopStrip.IsEmpty()
		? std::min(unconstrainedPanelHeight, std::max(0, _occupiedTopStrip.Bottom - StandardPanelPadding - mainPanelBottom))
		: unconstrainedPanelHeight;
	_handle->SetPosition(_selectionOnly ? Position2d{ margin, height - handleHeight } : Position2d{ 0, std::min(GuiStyle::StatusBarHeight + 8, height / 2) });
	_handle->SetSize({ static_cast<unsigned int>(handleWidth), static_cast<unsigned int>(handleHeight) });
	_handle->SetVisible(handleWidth > 0 && handleHeight > 0);
	_frame->SetSize({ static_cast<unsigned int>(panelWidth), static_cast<unsigned int>(panelHeight) });
	_UpdatePresentation();
	if (_selectionOnly)
		_LayoutSelection(panelWidth, panelHeight);
	else
		_LayoutSettings(panelWidth, panelHeight);
}

int GuiMainPanel::_SettingsPanelWidth() const
{
	// Keep the frame aligned to the four settings tabs. Page content wraps to
	// the width available inside this fixed frame.
	return static_cast<int>(_tabs->GetSize().Width + 28u);
}

int GuiMainPanel::_SelectionPanelHeight() const
{
	unsigned int radioHeight = 64u;
	if (_selectionOnly)
		for (const auto& entry : _controlSizes)
			if (std::dynamic_pointer_cast<GuiRadio>(entry.first)) { radioHeight = entry.second.Height; break; }
	return static_cast<int>(radioHeight + 4u + 2u * GuiPerformanceGroupRow::Height) +
		2 * SelectionRowGap + 2 * StandardPanelPadding + SelectionHandleHeight;
}

void GuiMainPanel::_LayoutSelection(int panelWidth, int panelHeight)
{
	const int padding = std::min(StandardPanelPadding, std::min(panelWidth, panelHeight) / 2);
	const int innerWidth = std::max(0, panelWidth - 2 * padding);
	const int innerHeight = std::max(0, panelHeight - 2 * padding - SelectionHandleHeight);
	auto& page = _pages[static_cast<size_t>(_page)];
	page->SetPadding(2u, 2u);
	page->SetSpacing(SelectionRowGap);
	// A short viewport scrolls the complete stack; leave the scrollbar's width
	// available before distributing the radios so every row shares its clip.
	const int contentHeight = _SelectionPanelHeight() - 2 * StandardPanelPadding - SelectionHandleHeight;
	const int barWidth = contentHeight > innerHeight ? 12 : 0;
	page->SetSize({ static_cast<unsigned int>(std::max(0, innerWidth - barWidth)), static_cast<unsigned int>(contentHeight) });
	for (const auto& row : _groupRows) row->SetSize({ static_cast<unsigned int>(std::max(0, innerWidth - 4 - barWidth)), GuiPerformanceGroupRow::Height });
	for (const auto& entry : _controlSizes)
	{
		auto radio = std::dynamic_pointer_cast<GuiRadio>(entry.first);
		if (!radio) continue;
		unsigned int count = 0;
		while (count < 255 && radio->TryGetChild(static_cast<unsigned char>(count))) ++count;
		const auto available = static_cast<unsigned int>(std::max(0, innerWidth - 4 - barWidth));
		const auto radioWidth = std::min(available, entry.second.Width);
		for (unsigned int index = 0; index < count; ++index)
		{
			auto toggle = radio->TryGetChild(static_cast<unsigned char>(index));
			const auto left = radioWidth * index / count;
			const auto right = radioWidth * (index + 1u) / count;
			toggle->SetPosition({ static_cast<int>(left), 0 });
			toggle->SetSize({ right - left, entry.second.Height });
		}
		radio->SetSize({ radioWidth, entry.second.Height });
		for (const auto& row : _groupRows) row->SetSize({ radio->GetSize().Width, GuiPerformanceGroupRow::Height });
		page->SetSize({ radio->GetSize().Width + 4u, static_cast<unsigned int>(contentHeight) });
	}
	page->ComputeLayout();
	_pageScroll->SetPosition({ padding, padding });
	_pageScroll->SetSize({ static_cast<unsigned int>(innerWidth), static_cast<unsigned int>(innerHeight) });
}

void GuiMainPanel::_LayoutSettings(int panelWidth, int panelHeight)
{
	const int padding = std::min(12, std::min(panelWidth, panelHeight) / 2);
	const int inner = std::max(0, panelWidth - 2 * padding);
	const int titleHeight = std::min(22, std::max(0, panelHeight - 2 * padding));
	auto title = _frame->TryGetChild(0);
	title->SetPosition({ padding, std::max(padding, panelHeight - padding - titleHeight) });
	title->SetSize({ static_cast<unsigned int>(inner), static_cast<unsigned int>(titleHeight) });
	const int tabHeight = std::min(48, std::max(0, panelHeight - 2 * padding - titleHeight));
	if (_tabScroll)
	{
		_tabScroll->SetPosition({ padding, std::max(padding, panelHeight - padding - titleHeight - tabHeight) });
		_tabScroll->SetSize({ static_cast<unsigned int>(inner), static_cast<unsigned int>(tabHeight) });
	}
	const int viewportHeight = std::max(0, panelHeight - 2 * padding - titleHeight - tabHeight - 8);
	_pageScroll->SetPosition({ padding, padding });
	_pageScroll->SetSize({ static_cast<unsigned int>(inner), static_cast<unsigned int>(viewportHeight) });
	auto& page = _pages[static_cast<size_t>(_page)];
	unsigned int contentHeight = 8u, contentWidth = inner > 8 ? inner - 8u : 0u;
	for (unsigned int index = 0; index < 256; ++index)
	{
		auto child = page->TryGetChild(static_cast<unsigned char>(index));
		if (!child) break;
		const auto childWidth = static_cast<unsigned int>(std::max(0, inner - 24));
		const auto original = std::find_if(_controlSizes.begin(), _controlSizes.end(),
			[&child](const auto& entry) { return entry.first == child; });
		if (auto label = std::dynamic_pointer_cast<GuiLabel>(child))
			label->SetSize({ childWidth, label->GetSize().Height });
		else if (auto radio = std::dynamic_pointer_cast<GuiRadio>(child))
		{
			unsigned int count = 0;
			while (count < 255 && radio->TryGetChild(static_cast<unsigned char>(count))) ++count;
			const auto natural = original != _controlSizes.end() ? original->second : radio->GetSize();
			const auto naturalCellWidth = count ? std::max(1u, (natural.Width + count - 1u) / count) : 1u;
			unsigned int columns = count ? std::max(1u, std::min(count, childWidth / std::max(44u, naturalCellWidth))) : 1u;
			if (count == 9u) columns = 5u;
			else if (count == 16u) columns = 8u;
			const auto rows = (count + columns - 1u) / columns;
			for (unsigned int toggleIndex = 0; toggleIndex < count; ++toggleIndex)
			{
				auto toggle = radio->TryGetChild(static_cast<unsigned char>(toggleIndex));
				const auto row = toggleIndex / columns;
				const auto column = toggleIndex % columns;
				const auto rowItems = std::min(columns, count - row * columns);
				const auto rowWidth = childWidth * rowItems / columns;
				const auto rowStart = (childWidth - rowWidth) / 2u;
				const auto left = rowStart + childWidth * column / columns;
				const auto right = rowStart + childWidth * (column + 1u) / columns;
				toggle->SetPosition({ static_cast<int>(left),
					static_cast<int>((rows - 1u - row) * natural.Height) });
				toggle->SetSize({ right - left, natural.Height });
			}
			radio->SetSize({ childWidth, rows * natural.Height });
		}
		else
		{
			if (original != _controlSizes.end())
				child->SetSize({ std::dynamic_pointer_cast<GuiButton>(child)
					? childWidth : std::min(childWidth, original->second.Width), original->second.Height });
			if (auto stack = std::dynamic_pointer_cast<GuiStackPanel>(child))
			{
				stack->InvalidateLayout();
				stack->ComputeLayout();
			}
		}
		contentHeight += child->GetSize().Height + 8u;
		contentWidth = std::max(contentWidth, child->GetSize().Width);
	}
	page->SetSize({ contentWidth + 8u, contentHeight });
	page->ComputeLayout();
	_pageScroll->SetSize(_pageScroll->GetSize());
}

bool GuiMainPanel::RouteHitTest(Position2d position)
{
	if (!IsVisible() || !IsEnabled()) return false;
	if (position.X < 0 || position.Y < 0 || position.X >= static_cast<int>(_viewport.Width) || position.Y >= static_cast<int>(_viewport.Height)) return false;
	return _handle->RouteHitTest(_handle->ParentToLocal(position)) ||
		(_frame->IsVisible() && _frame->RouteHitTest(_frame->ParentToLocal(position)));
}

void GuiMainPanel::InitResources(resources::ResourceLib& resources, bool force)
{
	GuiPanel::InitResources(resources, force);
	_Layout(); // Measure headers only after every page has resolved its font.
}

void GuiMainPanel::_InitResources(resources::ResourceLib& resources, bool force)
{
	_pageResources = &resources;
	GuiPanel::_InitResources(resources, force);
	for (auto& page : _pages)
		if (page != _pageScroll->Content()) page->InitResources(resources, force);
}

void GuiMainPanel::_ReleaseResources()
{
	_pageResources = nullptr;
	GuiPanel::_ReleaseResources();
	for (auto& page : _pages)
		if (page && page != _pageScroll->Content()) page->ReleaseResources();
}

void GuiMainPanel::ReplacePageSettings(SettingsPage pageId, const std::vector<GuiSettingEntry>& settings)
{
	if (pageId >= SettingsPage::Count) return;
	const auto index = static_cast<size_t>(pageId);
	_PrepareHide(_pages[index]);
	auto oldPage = _pages[index];
	const auto previousSizes = _controlSizes;
	_controlSizes.erase(std::remove_if(_controlSizes.begin(), _controlSizes.end(),
		[&oldPage](const auto& item) { return item.first->Parent() == oldPage; }), _controlSizes.end());
	_pageBindings[index].clear();
	GuiStackPanelParams params;
	params.PaddingH = 4u; params.PaddingV = 4u; params.Spacing = 8u; params.Size = {300u, 8u};
	auto page = std::make_shared<GuiStackPanel>(params);
	for (const auto& setting : settings)
	{
		if (!setting.Control) continue;
		if (!setting.Label.empty())
		{
			const auto labelHeight = setting.Label.size() > 30u
				? 2u * GuiLabelParams::DefaultHeight : GuiLabelParams::DefaultHeight;
			auto labelParams = GuiLabelParams::PanelHeader(setting.Label, 320, labelHeight);
			labelParams.FontSize = graphics::FontOptions::FONT_SMALL;
			page->AddChild(std::make_shared<GuiLabel>(labelParams));
		}
		page->AddChild(setting.Control);
		// A retained widget may currently be wrapped for a narrow viewport.
		const auto original = std::find_if(previousSizes.begin(), previousSizes.end(),
			[&setting](const auto& entry) { return entry.first == setting.Control; });
		_controlSizes.emplace_back(setting.Control, original != previousSizes.end()
			? original->second : setting.Control->GetSize());
		auto binding = std::make_shared<GuiCommandReceiver>(setting.Command, _commandOwner);
		setting.Control->SetReceiver(binding);
		_pageBindings[index].push_back(binding);
	}
	_pages[index] = page;
	if (_page == pageId) _pageScroll->SetContent(page);
	if (_pageResources) page->InitResources(*_pageResources, true);
	_Layout();
}

void GuiMainPanel::FinalizePageSettings(SettingsPage page)
{
	if (page < SettingsPage::Count) _PrepareHide(_pages[static_cast<size_t>(page)]);
}
