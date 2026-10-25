#pragma once

#include <array>
#include <functional>
#include "GuiPanel.h"
#include "GuiStackPanel.h"
#include "GuiScrollPanel.h"
#include "GuiRadio.h"
#include "GuiPerformanceGroupRow.h"
#include "GuiPopupManager.h"

namespace gui
{
	enum class SettingsPage : unsigned int { Audio, Midi, Timing, Session, Groups, Selection, Count };
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

	// UI-owned overlay composition. The caller supplies controls and command identity;
	// pages retain their widgets, while only the active page enters traversal.
	class GuiMainPanel : public GuiPanel
	{
	public:
		static constexpr unsigned int SaveRigDefaultControlIndex = 8400u;
		// 8401-8404 belong to station confirmation controls.
		static constexpr unsigned int SaveRigControlIndex = 8410u;
		static constexpr unsigned int SaveRigAsControlIndex = 8411u;
		static constexpr unsigned int LoadRigControlIndex = 8412u;
		explicit GuiMainPanel(GuiMainPanelParams params);
		void FinalizePageSettings(SettingsPage page);
		void ReplacePageSettings(SettingsPage page, const std::vector<GuiSettingEntry>& settings);
		void SetRigSaveEnabled(bool enabled)
		{
			if (!_saveRigButton || _saveRigButton->IsEnabled() == enabled) return;
			_saveRigButton->SetEnabled(enabled);
			_saveRigButton->SetText(enabled ? "Set as Default" : "Busy");
			for (const auto& button : _additionalRigSaveButtons) button->SetEnabled(enabled);
		}
		void SetCommandOwner(std::weak_ptr<base::ActionReceiver> owner);
		void SetViewportSize(utils::Size2d viewport);
		void SetOccupiedTopStripBounds(utils::Rect2d bounds);
		void SetPerformanceGroupHandler(std::function<void(bool, engine::GroupCommand, std::size_t)> handler);
		void RefreshPerformanceGroups(const engine::PerformanceGroups& groups);
		std::optional<std::pair<bool, std::size_t>> HoveredPerformanceGroup() const;
		void ClearPerformanceGroupPointerState();
		void SetPage(SettingsPage page);
		SettingsPage Page() const { return _page; }
		void SetExpanded(bool expanded);
		bool IsExpanded() const { return _expanded; }
		bool AdvanceAnimation(float elapsedSeconds);
		float TransitionValue() const { return _transition; }
		float PresentedOpacity() const;
		void Draw(base::DrawContext& context) override;
		void InitResources(resources::ResourceLib& resources, bool force) override;
		bool RouteHitTest(utils::Position2d position) override;
		using GuiPanel::OnAction;
		actions::ActionResult OnAction(actions::GuiAction action) override;
	protected:
		void _InitReceivers() override;
		void _InitResources(resources::ResourceLib& resources, bool force) override;
		void _ReleaseResources() override;
	private:
		friend class GuiMainPanelTestAccess;
		static constexpr int StandardPanelPadding = 16;
		static constexpr int MainPanelTopGap = StandardPanelPadding * 3 + 4;
		static constexpr int MainPanelMinimumHeight = 320;
		static constexpr int SelectionRowGap = 8;
		static constexpr unsigned int PageContentPadding = 8u;
		// Match the selection stack's top inset to its combined side padding.
		static constexpr int SelectionTopPadding = StandardPanelPadding + static_cast<int>(PageContentPadding);
		static constexpr unsigned int SettingsScrollBarWidth = 12u;
		static constexpr unsigned int SettingsTabsWidth = 416u;
		int _SelectionPanelHeight() const;
		int _SettingsPanelWidth() const;
		static constexpr int SelectionHandleHeight = 20;
		void _Layout();
		void _LayoutSelection(int panelWidth, int panelHeight);
		void _LayoutSettings(int panelWidth, int panelHeight);
		void _PrepareHide(const std::shared_ptr<base::GuiElement>& subtree);
		void _UpdatePresentation();
		static constexpr unsigned int _PageCommand = 1u;
		static constexpr unsigned int _ExpandCommand = 2u;
		resources::ResourceLib* _pageResources = nullptr; // UI owner; cleared on release.
		std::weak_ptr<base::ActionReceiver> _commandOwner;
		std::array<std::vector<std::shared_ptr<GuiCommandReceiver>>, static_cast<size_t>(SettingsPage::Count)> _pageBindings;
		std::shared_ptr<GuiButton> _saveRigButton;
		std::vector<std::shared_ptr<GuiButton>> _additionalRigSaveButtons;
		std::array<std::shared_ptr<GuiPerformanceGroupRow>, 2> _groupRows;
		bool _selectionOnly;
		bool _expanded = true;
		float _transition = 1.0f;
		SettingsPage _page;
		utils::Size2d _viewport{};
		utils::Rect2d _occupiedTopStrip{};
		GuiPopupManager* _popups;
	std::function<void(const std::shared_ptr<base::GuiElement>&)> _beforeHide;
	std::shared_ptr<GuiPanel> _frame;
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
