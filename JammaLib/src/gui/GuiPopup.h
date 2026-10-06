#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiButton.h"
#include "GuiLabel.h"

namespace gui
{
	struct GuiPopupAction
	{
		std::string Text;
		unsigned int Index = 0u;
	};

	struct GuiPopupButtonConfig
	{
		std::vector<GuiPopupAction> Actions;
	};

	struct GuiPopupParams : public base::GuiElementParams
	{
		GuiPopupParams() = default;

		static GuiPopupParams PanelDefault();
	};

	class GuiPopup : public GuiPanel
	{
	public:
		explicit GuiPopup(const GuiPopupParams& params = GuiPopupParams::PanelDefault());

		void SetTitle(const std::string& text);
		void SetBodyLines(const std::vector<std::string>& lines);
		void ConfigureButtons(const GuiPopupButtonConfig& config);
		void SetButtonReceiver(std::shared_ptr<base::ActionReceiver> receiver);
		void SetSize(utils::Size2d size) override;
		void FitToViewport(utils::Size2d viewport);
		void Draw(base::DrawContext& context) override;

	private:
		void _LayoutButtons();
		void _LayoutContents();

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
		std::array<std::shared_ptr<GuiLabel>, 3> _lineLabels;
		std::vector<std::shared_ptr<GuiButton>> _buttons;
		std::shared_ptr<base::ActionReceiver> _buttonReceiver;
		utils::Size2d _preferredSize;
	};
}
