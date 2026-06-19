#pragma once

#include <memory>
#include <string>
#include "GuiStackPanel.h"
#include "GuiGrid.h"
#include "GuiScrollPanel.h"
#include "GuiDropDown.h"
#include "GuiNumericInput.h"
#include "GuiTextBox.h"
#include "GuiSlider.h"
#include "GuiButton.h"
#include "GuiToggle.h"
#include "GuiLabel.h"
#include "GuiPopupHost.h"

namespace gui
{
	struct GuiMainPanelParams : public GuiStackPanelParams
	{
		GuiPopupHost* PopupHost = nullptr;
	};

	class GuiMainPanel : public GuiStackPanel
	{
	public:
		explicit GuiMainPanel(GuiMainPanelParams params);

	private:
		static constexpr unsigned int _PanelWidth = 360u;
		static constexpr unsigned int _PanelHeight = 576u;
		static constexpr unsigned int _PanelMinWidth = 160u;
		static constexpr unsigned int _PanelMinHeight = 560u;
		static constexpr int _PanelPosX = 20;
		static constexpr int _PanelPosY = 48;
		static constexpr unsigned int _PanelPadding = 16u;
		static constexpr unsigned int _PanelSpacing = 12u;
		static constexpr unsigned int _SectionWidth = 328u;
		static constexpr unsigned int _SectionContentWidth = 320u;
		static constexpr unsigned int _SectionPadding = 4u;
		static constexpr unsigned int _SectionSpacing = 8u;
		static constexpr unsigned int _TextBoxPadding = 8u;
		static constexpr unsigned int _NumericInputPadding = 8u;
		static constexpr unsigned int _DropDownPadding = 2u;
		static constexpr unsigned int _UpperSectionHeight = 248u;
		static constexpr unsigned int _LowerSectionHeight = 264u;
		static constexpr unsigned int _HeaderHeight = 22u;
		static constexpr unsigned int _StackRowHeight = 38u;
		static constexpr unsigned int _ControlHeight = 34u;
		static constexpr unsigned int _GridHeight = 96u;
		static constexpr unsigned int _WrapStackHeight = 56u;
		static constexpr unsigned int _ScrollPanelHeight = 120u;

		void _BuildUpperSection();
		void _BuildLowerSection();
		std::shared_ptr<GuiLabel> _MakeHeaderLabel(const std::string& text, unsigned int width);
		std::shared_ptr<GuiGrid> _CreateToggleGrid();
		std::shared_ptr<GuiStackPanel> _CreateSliderStack();

		GuiPopupHost* _popupHost;
	};
}
