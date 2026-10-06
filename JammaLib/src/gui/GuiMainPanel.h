#pragma once

#include <array>
#include <functional>
#include "GuiPanel.h"
#include "GuiStackPanel.h"
#include "GuiScrollPanel.h"
#include "GuiRadio.h"
#include "GuiPopupManager.h"

namespace gui
{
	enum class SettingsPage : unsigned int { Midi, Timing, Audio, Session, Groups, Selection, Count };
	struct GuiSettingEntry
	{
		SettingsPage Page = SettingsPage::Timing;
		std::string Label;
		std::shared_ptr<base::GuiElement> Control;
		unsigned int Command = 0;
	};
	struct GuiMainPanelParams : public base::GuiElementParams
	{
		GuiPopupManager* PopupManager = nullptr;
		bool SelectionOnly = false;
		std::vector<GuiSettingEntry> Settings;
		std::function<void(const std::shared_ptr<base::GuiElement>&)> BeforeHide;
	};

	// UI-owned overlay composition. Scene supplies controls and command identity;
	// pages retain their widgets, while only the active page enters traversal.
	class GuiMainPanel : public GuiPanel
	{
	public:
		explicit GuiMainPanel(GuiMainPanelParams params);
		void SetCommandOwner(std::weak_ptr<base::ActionReceiver> owner);
		void SetViewportSize(utils::Size2d viewport);
		void SetPage(SettingsPage page);
		SettingsPage Page() const { return _page; }
		void SetExpanded(bool expanded);
		bool IsExpanded() const { return _expanded; }
		bool AdvanceAnimation(float elapsedSeconds);
		float TransitionValue() const { return _transition; }
		float PresentedOpacity() const;
		void Draw(base::DrawContext& context) override;
		bool RouteHitTest(utils::Position2d position) override;
		using GuiPanel::OnAction;
		actions::ActionResult OnAction(actions::GuiAction action) override;
	protected:
		void _InitReceivers() override;
		void _InitResources(resources::ResourceLib& resources, bool force) override;
		void _ReleaseResources() override;
	private:
		void _Layout();
		void _PrepareHide(const std::shared_ptr<base::GuiElement>& subtree);
		void _UpdatePresentation();
		static constexpr unsigned int _PageCommand = 1u;
		static constexpr unsigned int _ExpandCommand = 2u;
		bool _selectionOnly;
		bool _expanded = true;
		float _transition = 1.0f;
		SettingsPage _page;
		utils::Size2d _viewport{};
		GuiPopupManager* _popups;
		std::function<void(const std::shared_ptr<base::GuiElement>&)> _beforeHide;
		std::shared_ptr<GuiPanel> _frame;
		std::shared_ptr<GuiPanel> _edge;
		std::shared_ptr<GuiToggle> _handle;
		std::shared_ptr<GuiRadio> _tabs;
		std::shared_ptr<GuiScrollPanel> _tabScroll;
		std::shared_ptr<GuiScrollPanel> _pageScroll;
		std::array<std::shared_ptr<GuiStackPanel>, static_cast<size_t>(SettingsPage::Count)> _pages;
		std::array<int, static_cast<size_t>(SettingsPage::Count)> _offsets{};
		std::vector<std::shared_ptr<GuiCommandReceiver>> _bindings;
		std::vector<std::pair<std::shared_ptr<base::GuiElement>, utils::Size2d>> _controlSizes;
		std::shared_ptr<GuiCommandReceiver> _pageBinding;
		std::shared_ptr<GuiCommandReceiver> _expandBinding;
	};
}
