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

	private:
		void _LayoutButtons();

		static constexpr unsigned int TitleWidth = 420u;
		static constexpr unsigned int TitleHeight = 26u;
		static constexpr unsigned int LineWidth = 420u;
		static constexpr unsigned int LineHeight = 24u;
		static constexpr unsigned int ButtonWidth = 96u;
		static constexpr unsigned int ButtonHeight = 36u;
		static constexpr unsigned int ButtonMinWidth = 60u;
		static constexpr int ButtonY = 24;
		static constexpr int ButtonRightInset = 48;
		static constexpr int ButtonSpacing = 12;

		std::shared_ptr<GuiLabel> _titleLabel;
		std::array<std::shared_ptr<GuiLabel>, 3> _lineLabels;
		std::vector<std::shared_ptr<GuiButton>> _buttons;
		std::shared_ptr<base::ActionReceiver> _buttonReceiver;
	};
}
