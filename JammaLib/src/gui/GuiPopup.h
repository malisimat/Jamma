#pragma once

#include <array>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiButton.h"
#include "GuiLabel.h"
#include "GuiScrollPanel.h"

namespace gui
{
	struct GuiPopupAction
	{
		std::string Text;
		unsigned int Index = 0u;
		bool PinLeft = false;
	};

	struct GuiPopupRow
	{
		std::string Detail;
		std::string PrimaryAction;
		bool Active = false;
		bool Recent = false;
		bool ActionsEnabled = true;
	};

	struct GuiPopupButtonConfig
	{
		std::vector<GuiPopupAction> Actions;
		bool RequirePress = false;
	};

	struct GuiPopupParams : public base::GuiElementParams
	{
		enum class VisualMode { Normal, Learning, LearningBlue };
		GuiPopupParams() = default;
		VisualMode Mode = VisualMode::Normal;

		static GuiPopupParams PanelDefault(VisualMode mode = VisualMode::Normal);
		static GuiLabelParams Header(const std::string& text, unsigned int width, unsigned int height = 26u);
		static base::GuiElementParams HeaderBand(unsigned int width, unsigned int height);
		static base::GuiElementParams FlatRect(unsigned int width, unsigned int height,
			glm::vec3 color, float opacity);
	};
	class GuiPopupColorRect;

	class GuiPopup : public GuiPanel
	{
	public:
		explicit GuiPopup(const GuiPopupParams& params = GuiPopupParams::PanelDefault());

		void SetTitle(const std::string& text);
		void SetVisualMode(GuiPopupParams::VisualMode mode);
		void SetBodyLines(const std::vector<std::string>& lines);
		void ConfigureButtons(const GuiPopupButtonConfig& config);
		void SetButtonReceiver(std::shared_ptr<base::ActionReceiver> receiver);
		void SetActionEnabled(unsigned int index, bool enabled);
		void SetRowText(const std::string& subtitle, const std::string& status);
		void SetRows(const std::vector<GuiPopupRow>& rows, std::function<void(std::size_t)> primary,
			std::function<void(std::size_t)> remove, std::optional<std::size_t> reveal = {});
		int RowScrollOffset() const { return _rowScroll ? _rowScroll->ScrollOffset() : 0; }
		void SetKeyAction(std::function<bool(actions::KeyAction)> action);
		using GuiPanel::OnAction;
		actions::ActionResult OnAction(actions::KeyAction action) override;
		void SetSize(utils::Size2d size) override;
		void FitToViewport(utils::Size2d viewport);
		void Draw(base::DrawContext& context) override;

	private:
		void _LayoutButtons();
		void _LayoutContents();
		void _LayoutRows();

		static constexpr unsigned int TitleWidth = 420u;
		static constexpr unsigned int TitleHeight = 26u;
		static constexpr unsigned int LineWidth = 420u;
		static constexpr unsigned int LineHeight = 24u;
		// Fits the existing "Follow server" action at the normal font/padding.
		static constexpr unsigned int ButtonWidth = 128u;
		static constexpr unsigned int ButtonHeight = 36u;
		static constexpr unsigned int ButtonMinWidth = 60u;
		static constexpr int ButtonY = 24;
		static constexpr int ButtonRightInset = 48;
		static constexpr int ButtonSpacing = 12;

		std::shared_ptr<GuiLabel> _titleLabel;
		std::shared_ptr<GuiPopupColorRect> _fill;
		std::shared_ptr<base::GuiElement> _headerBand;
		std::array<std::shared_ptr<GuiLabel>, 3> _lineLabels;
		std::vector<std::shared_ptr<GuiButton>> _buttons;
		std::vector<GuiPopupAction> _actions;
		std::shared_ptr<GuiScrollPanel> _rowScroll;
		std::shared_ptr<GuiLabel> _rowSubtitle, _rowStatus;
		std::function<bool(actions::KeyAction)> _keyAction;
		std::shared_ptr<base::ActionReceiver> _buttonReceiver;
		utils::Size2d _preferredSize;
	};
}
