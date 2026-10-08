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
			auto label = GuiLabelParams::PanelHeader(setting.Label, 300u);
			label.Ellipsize = true;
			page->AddChild(std::make_shared<GuiLabel>(label));
		}
		page->AddChild(setting.Control);
		_controlSizes.emplace_back(setting.Control, setting.Control->GetSize());
		auto binding = std::make_shared<GuiCommandReceiver>(setting.Command);
		setting.Control->SetReceiver(binding);
		_bindings.push_back(std::move(binding));
	}
	GuiScrollPanelParams scrollParams;
	scrollParams.ScrollBarWidth = 12u;
	_pageScroll = std::make_shared<GuiScrollPanel>(scrollParams);
	_pageScroll->SetContent(_pages[static_cast<size_t>(_page)]);
	_frame->AddChild(_pageScroll);
	if (!_selectionOnly)
	{
		GuiRadioParams tabParams;
		tabParams.InitValue = 1u;
		tabParams.Size = { 208u, 32u };
		for (unsigned int index = 0; index < 2; ++index)
		{
			auto toggle = GuiToggleParams::PanelPrimary();
			toggle.Text = index == 0 ? "MIDI" : "Timing";
			toggle.TintColor = GuiStyle::Control();
			toggle.Position = { static_cast<int>(index * 104u), 0 };
			toggle.Size = { 100u, 32u };
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
	GuiElementParams edgeParams;
	edgeParams.Texture = "rounded_but";
	edgeParams.TextureShader = "texture_tinted";
	edgeParams.TintColor = GuiStyle::Edge();
	_edge = std::make_shared<GuiPanel>(edgeParams);
	_frame->AddChild(_edge);
	SetViewportSize(params.Size);
}

void GuiMainPanel::SetCommandOwner(std::weak_ptr<ActionReceiver> owner)
{
	for (auto& binding : _bindings) binding->SetOwner(owner);
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
	if (_selectionOnly || (page != SettingsPage::Midi && page != SettingsPage::Timing) || page == _page) return;
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
	if (_transition == target) return false;
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

void GuiMainPanel::_Layout()
{
	const int width = static_cast<int>(_viewport.Width), height = static_cast<int>(_viewport.Height);
	const int margin = std::min(8, width / 2);
	const int panelWidth = std::min(_selectionOnly ? 404 : 360, std::max(0, width - 2 * margin));
	const int handleWidth = std::min(_selectionOnly ? 64 : 20, std::max(0, width - (_selectionOnly ? margin : 0)));
	const int handleHeight = std::min(_selectionOnly ? 20 : 64, std::max(0, height - (_selectionOnly ? 0 : std::min(8, height / 2))));
	const int panelHeight = std::min(_selectionOnly ? 84 : 320, std::max(0, height - 2 * std::min(8, height / 2)));
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

void GuiMainPanel::_LayoutSelection(int panelWidth, int panelHeight)
{
	const int padding = std::min(8, std::min(panelWidth, panelHeight) / 2);
	const int innerWidth = std::max(0, panelWidth - 2 * padding);
	const int innerHeight = std::max(0, panelHeight - 2 * padding);
	auto& page = _pages[static_cast<size_t>(_page)];
	page->SetPadding(2u, 2u);
	page->SetSpacing(0u);
	for (const auto& entry : _controlSizes)
	{
		auto radio = std::dynamic_pointer_cast<GuiRadio>(entry.first);
		if (!radio) continue;
		unsigned int count = 0;
		while (count < 255 && radio->TryGetChild(static_cast<unsigned char>(count))) ++count;
		const auto available = static_cast<unsigned int>(std::max(0, innerWidth - 4));
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
		page->SetSize({ radioWidth + 4u, entry.second.Height + 4u });
	}
	page->ComputeLayout();
	_pageScroll->SetPosition({ padding, padding });
	_pageScroll->SetSize({ static_cast<unsigned int>(innerWidth), static_cast<unsigned int>(innerHeight) });
	_edge->SetVisible(false);
}

void GuiMainPanel::_LayoutSettings(int panelWidth, int panelHeight)
{
	const int padding = std::min(12, std::min(panelWidth, panelHeight) / 2);
	const int inner = std::max(0, panelWidth - 2 * padding);
	_edge->SetPosition({ padding, std::max(0, panelHeight - 2) });
	_edge->SetSize({ static_cast<unsigned int>(inner), static_cast<unsigned int>(std::min(1, panelHeight)) });
	const int titleHeight = std::min(22, std::max(0, panelHeight - 2 * padding));
	auto title = _frame->TryGetChild(0);
	title->SetPosition({ padding, std::max(padding, panelHeight - padding - titleHeight) });
	title->SetSize({ static_cast<unsigned int>(inner), static_cast<unsigned int>(titleHeight) });
	const int tabHeight = std::min(44, std::max(0, panelHeight - 2 * padding - titleHeight));
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
		contentHeight += child->GetSize().Height + 8u;
		const auto childWidth = static_cast<unsigned int>(std::max(0, inner - 24));
		if (auto label = std::dynamic_pointer_cast<GuiLabel>(child))
			label->SetSize({ childWidth, label->GetSize().Height });
		else if (auto radio = std::dynamic_pointer_cast<GuiRadio>(child))
		{
			unsigned int count = 0;
			while (count < 255 && radio->TryGetChild(static_cast<unsigned char>(count))) ++count;
			const auto toggleWidth = count ? childWidth / count : 0u;
			for (unsigned int toggleIndex = 0; toggleIndex < count; ++toggleIndex)
			{
				auto toggle = radio->TryGetChild(static_cast<unsigned char>(toggleIndex));
				toggle->SetPosition({ static_cast<int>(toggleIndex * toggleWidth), 0 });
				toggle->SetSize({ toggleWidth, toggle->GetSize().Height });
			}
			radio->SetSize({ childWidth, radio->GetSize().Height });
		}
		else
		{
			const auto original = std::find_if(_controlSizes.begin(), _controlSizes.end(),
				[&child](const auto& entry) { return entry.first == child; });
			if (original != _controlSizes.end())
				child->SetSize({ std::min(childWidth, original->second.Width), original->second.Height });
		}
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

void GuiMainPanel::_InitResources(resources::ResourceLib& resources, bool force)
{
	GuiPanel::_InitResources(resources, force);
	for (auto& page : _pages)
		if (page != _pageScroll->Content()) page->InitResources(resources, force);
}

void GuiMainPanel::_ReleaseResources()
{
	GuiPanel::_ReleaseResources();
	for (auto& page : _pages)
		if (page && page != _pageScroll->Content()) page->ReleaseResources();
}
