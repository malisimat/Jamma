#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiLabel.h"
#include "GuiToggle.h"

namespace gui
{
	struct GuiPopupButtonConfig
	{
		bool ShowYes = true;
		bool ShowNo = false;
		bool ShowCancel = true;
		bool ShowOk = false;

		unsigned int YesIndex = 0u;
		unsigned int NoIndex = 0u;
		unsigned int CancelIndex = 0u;
		unsigned int OkIndex = 0u;

		std::string YesText = "Yes";
		std::string NoText = "No";
		std::string CancelText = "Cancel";
		std::string OkText = "Ok";
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
		void ResetButtonStates();

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
		static constexpr int ButtonSpacing = 20;

		std::shared_ptr<GuiLabel> _titleLabel;
		std::array<std::shared_ptr<GuiLabel>, 3> _lineLabels;
		std::shared_ptr<GuiToggle> _yesButton;
		std::shared_ptr<GuiToggle> _noButton;
		std::shared_ptr<GuiToggle> _cancelButton;
		std::shared_ptr<GuiToggle> _okButton;
	};
}
