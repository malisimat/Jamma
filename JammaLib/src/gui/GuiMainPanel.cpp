#include "GuiMainPanel.h"

#include <array>
#include <functional>
#include <string>

using namespace base;
using namespace gui;
using namespace utils;
using namespace resources;

GuiMainPanel::GuiMainPanel(GuiMainPanelParams params) :
	GuiStackPanel(params),
	_popupHost(params.PopupHost)
{
	_guiParams.Texture = "rounded_but";
	SetDirection(StackDirection::Vertical);
	SetSpacing(_PanelSpacing);
	SetPadding(_PanelPadding, _PanelPadding);
	SetPosition({ _PanelPosX, _PanelPosY });
	SetSize({ _PanelWidth, _PanelHeight });
	SetMinSize({ _PanelMinWidth, _PanelMinHeight });

	_BuildUpperSection();
	_BuildLowerSection();
}

void GuiMainPanel::_BuildUpperSection()
{
	const unsigned int sectionWidth = _SectionWidth;

	auto section = std::make_shared<GuiStackPanel>(GuiStackPanelParams{});
	section->SetDirection(StackDirection::Vertical);
	section->SetSpacing(_SectionSpacing);
	section->SetPadding(_SectionPadding, _SectionPadding);
	section->SetSize({ sectionWidth, _UpperSectionHeight });
	section->SetMinSize({ sectionWidth, _UpperSectionHeight });
	AddChild(section);

	section->AddChild(_MakeHeaderLabel("-- Upper --", sectionWidth));

	{
		GuiStackPanelParams hParams;
		hParams.Direction = StackDirection::Horizontal;
		hParams.Spacing = 6u;
		hParams.Size = { _SectionContentWidth, _StackRowHeight };
		hParams.MinSize = { 80u, _StackRowHeight };
		auto hStack = std::make_shared<GuiStackPanel>(hParams);

		{
			GuiButtonParams bp;
			bp.Texture = "rounded_but";
			bp.OverTexture = "rounded_but_over";
			bp.DownTexture = "rounded_but_down";
			bp.OutTexture = "rounded_but_on";
			bp.Size = { 102u, _ControlHeight };
			bp.MinSize = { 36u, _ControlHeight };
			hStack->AddChild(std::make_shared<GuiButton>(bp));
		}

		{
			GuiButtonParams bp;
			bp.Texture = "rounded_but";
			bp.OverTexture = "rounded_but_over";
			bp.DownTexture = "rounded_but_down";
			bp.OutTexture = "rounded_but_on";
			bp.Size = { 102u, _ControlHeight };
			bp.MinSize = { 36u, _ControlHeight };
			hStack->AddChild(std::make_shared<GuiButton>(bp));
		}

		{
			GuiButtonParams bp;
			bp.Texture = "rounded_but";
			bp.OverTexture = "rounded_but_over";
			bp.DownTexture = "rounded_but_down";
			bp.OutTexture = "rounded_but_on";
			bp.Size = { 102u, _ControlHeight };
			bp.MinSize = { 36u, _ControlHeight };
			hStack->AddChild(std::make_shared<GuiButton>(bp));
		}

		section->AddChild(hStack);
	}

	section->AddChild(_CreateToggleGrid());
	section->AddChild(_CreateSliderStack());
}

void GuiMainPanel::_BuildLowerSection()
{
	const unsigned int sectionWidth = _SectionWidth;

	auto section = std::make_shared<GuiStackPanel>(GuiStackPanelParams{});
	section->SetDirection(StackDirection::Vertical);
	section->SetSpacing(_SectionSpacing);
	section->SetPadding(_SectionPadding, _SectionPadding);
	section->SetSize({ sectionWidth, _LowerSectionHeight });
	section->SetMinSize({ sectionWidth, _LowerSectionHeight });
	AddChild(section);

	section->AddChild(_MakeHeaderLabel("-- Lower --", sectionWidth));

	{
		GuiTextBoxParams tp;
		tp.Texture = "rounded_but";
		tp.Text = "edit me";
		tp.Size = { _SectionContentWidth, _ControlHeight+10 };
		tp.MinSize = { 60u, _ControlHeight };
		tp.Padding = _TextBoxPadding;
		section->AddChild(std::make_shared<GuiTextBox>(tp));
	}

	{
		GuiNumericInputParams np;
		np.Texture = "rounded_but";
		np.Padding = _NumericInputPadding;
		np.Min = 0.0;
		np.Max = 100.0;
		np.Step = 0.5;
		np.InitValue = 42.0;
		np.Decimals = 1;
		np.Size = { _SectionContentWidth, _ControlHeight+10 };
		np.MinSize = { 60u, _ControlHeight };
		section->AddChild(std::make_shared<GuiNumericInput>(np));
	}

	{
		GuiDropDownParams dp;
		dp.Texture = "rounded_but";
		dp.Padding = _DropDownPadding;
		dp.Items = { "Sine", "Square", "Saw", "Triangle", "Noise" };
		dp.InitIndex = 0u;
		dp.RowHeight = 22u;
		dp.Size = { _SectionContentWidth, _ControlHeight+10 };
		dp.MinSize = { 60u, _ControlHeight+10 };
		auto dd = std::make_shared<GuiDropDown>(dp);
		if (_popupHost != nullptr)
			dd->SetPopupHost(_popupHost);
		section->AddChild(dd);
	}

	{
		GuiStackPanelParams cParams;
		cParams.Direction = StackDirection::Vertical;
		cParams.Spacing = 2u;
		cParams.PaddingH = 2u;
		cParams.PaddingV = 2u;
		cParams.Size = { _SectionContentWidth, _ScrollPanelHeight*3 };
		cParams.MinSize = { 60u, 40u };
		auto content = std::make_shared<GuiStackPanel>(cParams);
		for (int i = 0; i < 12; ++i)
		{
			GuiLabelParams lp;
			lp.String = "Scroll row " + std::to_string(i + 1);
			lp.Size = { 260u, 24u };
			lp.MinSize = { 40u, 24u };
			content->AddChild(std::make_shared<GuiLabel>(lp));
		}

		GuiScrollPanelParams spParams;
		spParams.Texture = "rounded_but";
		spParams.Size = { _SectionContentWidth+10, _ScrollPanelHeight };
		spParams.MinSize = { 60u, 40u };
		spParams.ScrollBarWidth = 12u;
		auto scroll = std::make_shared<GuiScrollPanel>(spParams);
		scroll->SetContent(content);
		section->AddChild(scroll);
	}
}

std::shared_ptr<GuiLabel> GuiMainPanel::_MakeHeaderLabel(const std::string& text, unsigned int width)
{
	GuiLabelParams lp;
	lp.String = text;
	lp.Size = { width, _HeaderHeight };
	lp.MinSize = { 40u, _HeaderHeight };
	return std::make_shared<GuiLabel>(lp);
}

std::shared_ptr<GuiGrid> GuiMainPanel::_CreateToggleGrid()
{
	GuiGridParams gp;
	gp.Texture = "";
	gp.Size = { _SectionContentWidth + 4, _GridHeight };
	gp.MinSize = { 80u, 40u };
	gp.PaddingH = 2u;
	gp.PaddingV = 2u;

	GridCellDef col;
	col.sizing = GridCellDef::Sizing::Fill;
	col.spacing = 4u;
	gp.Cols = { col, col };

	GridCellDef row;
	row.sizing = GridCellDef::Sizing::Fixed;
	row.fixedSize = 44u;
	row.spacing = 4u;
	gp.Rows = { row, row };

	auto grid = std::make_shared<GuiGrid>(gp);
	const std::array<std::function<std::shared_ptr<GuiElement>()>, 4> cellCreators = {
		[]() {
			GuiToggleParams tp;
			tp.Texture = "rounded_but";
			tp.OverTexture = "rounded_but_over";
			tp.DownTexture = "rounded_but_down";
			tp.OutTexture = "";
			tp.ToggledTexture = "rounded_but_on";
			tp.ToggledOverTexture = "rounded_but_on_over";
			tp.ToggledDownTexture = "rounded_but_on_down";
			tp.Size = { 80u, _ControlHeight };
			tp.MinSize = { 20u, _ControlHeight };
			return std::make_shared<GuiToggle>(tp);
		},
		[]() {
			GuiToggleParams tp;
			tp.Texture = "rounded_but";
			tp.OverTexture = "rounded_but_over";
			tp.DownTexture = "rounded_but_down";
			tp.OutTexture = "rounded_but_on";
			tp.ToggledTexture = "rounded_but_on";
			tp.ToggledOverTexture = "rounded_but_on_over";
			tp.ToggledDownTexture = "rounded_but_on_down";
			tp.Size = { 10u, _ControlHeight };
			tp.MinSize = { 10u, _ControlHeight };
			return std::make_shared<GuiToggle>(tp);
		},
		[]() {
			GuiToggleParams tp;
			tp.Texture = "rounded_but";
			tp.OverTexture = "rounded_but_over";
			tp.DownTexture = "rounded_but_down";
			tp.OutTexture = "rounded_but_on";
			tp.ToggledTexture = "rounded_but_on";
			tp.ToggledOverTexture = "rounded_but_on_over";
			tp.ToggledDownTexture = "rounded_but_on_down";
			tp.Size = { 10u, _ControlHeight };
			tp.MinSize = { 10u, _ControlHeight };
			return std::make_shared<GuiToggle>(tp);
		},
		[]() {
			GuiToggleParams tp;
			tp.Texture = "rounded_but";
			tp.OverTexture = "rounded_but_over";
			tp.DownTexture = "rounded_but_down";
			tp.OutTexture = "rounded_but_on";
			tp.ToggledTexture = "rounded_but_on";
			tp.ToggledOverTexture = "rounded_but_on_over";
			tp.ToggledDownTexture = "rounded_but_on_down";
			tp.Size = { 10u, _ControlHeight };
			tp.MinSize = { 10u, _ControlHeight };
			return std::make_shared<GuiToggle>(tp);
		}
	};

	for (int ci = 0; ci < 4; ++ci)
	{
		GridChildPlacement placement;
		placement.row = static_cast<unsigned int>(ci / 2);
		placement.col = static_cast<unsigned int>(ci % 2);
		placement.hAlign = LayoutHAlign::Fill;
		placement.vAlign = LayoutVAlign::Fill;
		grid->AddGridChild(cellCreators[ci](), placement);
	}

	return grid;
}

std::shared_ptr<GuiStackPanel> GuiMainPanel::_CreateSliderStack()
{
	GuiStackPanelParams wParams;
	wParams.Direction = StackDirection::Horizontal;
	wParams.Spacing = 4u;
	wParams.WrapContent = true;
	wParams.Size = { _SectionContentWidth, _WrapStackHeight };
	wParams.MinSize = { 60u, _ControlHeight };
	auto wStack = std::make_shared<GuiStackPanel>(wParams);

	{
		GuiSliderParams sp;
		sp.Texture = "rounded_but";
		sp.OverTexture = "rounded_but_over";
		sp.DownTexture = "rounded_but_down";
		sp.OutTexture = "rounded_but_on";
		sp.Size = { 104u, _ControlHeight };
		sp.MinSize = { 36u, _ControlHeight };
		sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
		sp.DragTexture = "blue";
		sp.DragOverTexture = "yellow";
		sp.DragControlSize = { 10u, _ControlHeight };
		sp.DragControlOffset = { 0, 0 };
		wStack->AddChild(std::make_shared<GuiSlider>(sp));
	}

	{
		GuiSliderParams sp;
		sp.Texture = "rounded_but";
		sp.OverTexture = "rounded_but_over";
		sp.DownTexture = "rounded_but_down";
		sp.OutTexture = "rounded_but_on";
		sp.Size = { 104u, _ControlHeight };
		sp.MinSize = { 36u, _ControlHeight };
		sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
		sp.DragTexture = "green";
		sp.DragOverTexture = "yellow";
		sp.DragControlSize = { 10u, _ControlHeight };
		sp.DragControlOffset = { 0, 0 };
		wStack->AddChild(std::make_shared<GuiSlider>(sp));
	}

	{
		GuiSliderParams sp;
		sp.Texture = "rounded_but";
		sp.OverTexture = "rounded_but_over";
		sp.DownTexture = "rounded_but_down";
		sp.OutTexture = "rounded_but_on";
		sp.Size = { 104u, _ControlHeight };
		sp.MinSize = { 36u, _ControlHeight };
		sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
		sp.DragTexture = "red";
		sp.DragOverTexture = "yellow";
		sp.DragControlSize = { 10u, _ControlHeight };
		sp.DragControlOffset = { 0, 0 };
		wStack->AddChild(std::make_shared<GuiSlider>(sp));
	}
	
	{
		GuiSliderParams sp;
		sp.Texture = "rounded_but";
		sp.OverTexture = "rounded_but_over";
		sp.DownTexture = "rounded_but_down";
		sp.OutTexture = "rounded_but_on";
		sp.Size = { 320u, _ControlHeight };
		sp.MinSize = { 36u, _ControlHeight };
		sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
		sp.DragTexture = "purple";
		sp.DragOverTexture = "yellow";
		sp.DragControlSize = { 10u, _ControlHeight };
		sp.DragControlOffset = { 0, 0 };
		wStack->AddChild(std::make_shared<GuiSlider>(sp));
	}

	return wStack;
}
