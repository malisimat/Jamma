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
		GuiStackPanelParams hParams = GuiStackPanelParams::PanelHorizontalRow(
			_SectionContentWidth,
			_StackRowHeight);
		auto hStack = std::make_shared<GuiStackPanel>(hParams);

		for (int i = 0; i < 3; ++i)
			hStack->AddChild(std::make_shared<GuiButton>(GuiButtonParams::PanelButton()));

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
		GuiTextBoxParams tp = GuiTextBoxParams::PanelInput(_SectionContentWidth);
		tp.Text = "edit me";
		section->AddChild(std::make_shared<GuiTextBox>(tp));
	}

	{
		GuiNumericInputParams np = GuiNumericInputParams::PanelInput(_SectionContentWidth);
		np.Min = 0.0;
		np.Max = 100.0;
		np.Step = 0.5;
		np.InitValue = 42.0;
		np.Decimals = 1;
		section->AddChild(std::make_shared<GuiNumericInput>(np));
	}

	{
		GuiDropDownParams dp = GuiDropDownParams::PanelInput(_SectionContentWidth);
		dp.Items = { "Sine", "Square", "Saw", "Triangle", "Noise" };
		dp.InitIndex = 0u;
		auto dd = std::make_shared<GuiDropDown>(dp);
		if (_popupHost != nullptr)
			dd->SetPopupHost(_popupHost);
		section->AddChild(dd);
	}

	{
		GuiStackPanelParams cParams = GuiStackPanelParams::PanelContentStack(
			_SectionContentWidth,
			_ScrollPanelHeight * 3);
		auto content = std::make_shared<GuiStackPanel>(cParams);
		for (int i = 0; i < 12; ++i)
		{
			content->AddChild(std::make_shared<GuiLabel>(
				GuiLabelParams::PanelScrollRow("Scroll row " + std::to_string(i + 1))));
		}

		GuiScrollPanelParams spParams = GuiScrollPanelParams::PanelScroll(
			_SectionContentWidth,
			_ScrollPanelHeight);
		auto scroll = std::make_shared<GuiScrollPanel>(spParams);
		scroll->SetContent(content);
		section->AddChild(scroll);
	}
}

std::shared_ptr<GuiLabel> GuiMainPanel::_MakeHeaderLabel(const std::string& text, unsigned int width)
{
	return std::make_shared<GuiLabel>(GuiLabelParams::PanelHeader(text, width));
}

std::shared_ptr<GuiGrid> GuiMainPanel::_CreateToggleGrid()
{
	GuiGridParams gp = GuiGridParams::PanelToggleGrid(_SectionContentWidth, _GridHeight);
	gp.Texture = "";

	auto grid = std::make_shared<GuiGrid>(gp);
	const std::array<std::function<std::shared_ptr<GuiElement>()>, 4> cellCreators = {
		[]() {
			return std::make_shared<GuiToggle>(GuiToggleParams::PanelPrimary());
		},
		[]() {
			return std::make_shared<GuiToggle>(GuiToggleParams::PanelSecondary());
		},
		[]() {
			return std::make_shared<GuiToggle>(GuiToggleParams::PanelSecondary());
		},
		[]() {
			return std::make_shared<GuiToggle>(GuiToggleParams::PanelSecondary());
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
	wParams.MinSize = { 60u, GuiSliderParams::DefaultHeight };
	auto wStack = std::make_shared<GuiStackPanel>(wParams);

	const std::array<std::string, 4> sliderDragTextures = {
		"blue",
		"green",
		"red",
		"purple"
	};

	for (std::size_t i = 0; i < sliderDragTextures.size(); ++i)
	{
		const bool isWide = (i + 1u) == sliderDragTextures.size();
		const unsigned int width = isWide ? GuiSliderParams::WideWidth : GuiSliderParams::DefaultWidth;
		wStack->AddChild(std::make_shared<GuiSlider>(
			GuiSliderParams::PanelHorizontal(sliderDragTextures[i], width)));
	}

	return wStack;
}
