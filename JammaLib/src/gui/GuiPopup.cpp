#include "GuiPopup.h"

#include <algorithm>

using namespace gui;

GuiPopupParams GuiPopupParams::PanelDefault()
{
	GuiPopupParams params;
	params.GuiPassThrough = false;
	params.TextureShader = "texture_tinted";
	params.Texture = "rounded_but";
	params.OverTexture = "rounded_but";
	params.DownTexture = "rounded_but";
	params.Size = { 460u, 210u };
	params.MinSize = { 460u, 210u };
	params.TintColor = glm::vec3(0.08f, 0.10f, 0.14f);
	return params;
}

GuiPopup::GuiPopup(const GuiPopupParams& params) :
	GuiPanel(params)
{
	GuiLabelParams titleParams;
	titleParams.String = "";
	titleParams.Size = { TitleWidth, TitleHeight };
	titleParams.MinSize = { 100u, TitleHeight };
	titleParams.Position = { 20, 170 };
	_titleLabel = std::make_shared<GuiLabel>(titleParams);
	AddChild(_titleLabel);

	for (std::size_t i = 0; i < _lineLabels.size(); ++i)
	{
		GuiLabelParams lineParams;
		lineParams.String = "";
		lineParams.Size = { LineWidth, LineHeight };
		lineParams.MinSize = { 80u, LineHeight };
		lineParams.Position = { 20, 136 - static_cast<int>(i) * 28 };
		_lineLabels[i] = std::make_shared<GuiLabel>(lineParams);
		_lineLabels[i]->SetVisible(false);
		AddChild(_lineLabels[i]);
	}

	auto makeButton = [](const std::string& text, unsigned int index) {
		GuiToggleParams params = GuiToggleParams::PanelPrimary();
		params.Text = text;
		params.Position = { 0, ButtonY };
		params.Size = { ButtonWidth, ButtonHeight };
		params.MinSize = { ButtonMinWidth, ButtonHeight };
		params.ToggleIndex = index;
		return std::make_shared<GuiToggle>(params);
	};

	_yesButton = makeButton("Yes", 0u);
	_noButton = makeButton("No", 0u);
	_cancelButton = makeButton("Cancel", 0u);
	_okButton = makeButton("Ok", 0u);

	AddChild(_yesButton);
	AddChild(_noButton);
	AddChild(_cancelButton);
	AddChild(_okButton);

	ConfigureButtons(GuiPopupButtonConfig{});
}

void GuiPopup::SetTitle(const std::string& text)
{
	if (_titleLabel)
	{
		_titleLabel->SetString(text);
		_titleLabel->SetVisible(!text.empty());
	}
}

void GuiPopup::SetBodyLines(const std::vector<std::string>& lines)
{
	for (std::size_t i = 0; i < _lineLabels.size(); ++i)
	{
		if (!_lineLabels[i])
			continue;

		if (i < lines.size() && !lines[i].empty())
		{
			_lineLabels[i]->SetString(lines[i]);
			_lineLabels[i]->SetVisible(true);
		}
		else
		{
			_lineLabels[i]->SetString("");
			_lineLabels[i]->SetVisible(false);
		}
	}
}

void GuiPopup::ConfigureButtons(const GuiPopupButtonConfig& config)
{
	if (_yesButton)
	{
		_yesButton->SetVisible(config.ShowYes);
		_yesButton->SetToggleIndex(config.YesIndex);
		_yesButton->SetText(config.YesText);
	}

	if (_noButton)
	{
		_noButton->SetVisible(config.ShowNo);
		_noButton->SetToggleIndex(config.NoIndex);
		_noButton->SetText(config.NoText);
	}

	if (_cancelButton)
	{
		_cancelButton->SetVisible(config.ShowCancel);
		_cancelButton->SetToggleIndex(config.CancelIndex);
		_cancelButton->SetText(config.CancelText);
	}

	if (_okButton)
	{
		_okButton->SetVisible(config.ShowOk);
		_okButton->SetToggleIndex(config.OkIndex);
		_okButton->SetText(config.OkText);
	}

	_LayoutButtons();
	ResetButtonStates();
}

void GuiPopup::SetButtonReceiver(std::shared_ptr<base::ActionReceiver> receiver)
{
	if (_yesButton)
		_yesButton->SetReceiver(receiver);
	if (_noButton)
		_noButton->SetReceiver(receiver);
	if (_cancelButton)
		_cancelButton->SetReceiver(receiver);
	if (_okButton)
		_okButton->SetReceiver(receiver);
}

void GuiPopup::ResetButtonStates()
{
	auto reset = [](const std::shared_ptr<GuiToggle>& button) {
		if (button)
			button->SetToggleState(gui::GuiToggleParams::TOGGLE_OFF, true);
	};

	reset(_yesButton);
	reset(_noButton);
	reset(_cancelButton);
	reset(_okButton);
}

void GuiPopup::_LayoutButtons()
{
	std::vector<std::shared_ptr<GuiToggle>> visibleButtons;
	visibleButtons.reserve(4);

	if (_yesButton && _yesButton->IsVisible())
		visibleButtons.push_back(_yesButton);
	if (_noButton && _noButton->IsVisible())
		visibleButtons.push_back(_noButton);
	if (_cancelButton && _cancelButton->IsVisible())
		visibleButtons.push_back(_cancelButton);
	if (_okButton && _okButton->IsVisible())
		visibleButtons.push_back(_okButton);

	if (visibleButtons.empty())
		return;

	const int popupWidth = static_cast<int>(GetSize().Width);
	const int buttonWidth = static_cast<int>(ButtonWidth);
	const int totalWidth = static_cast<int>(visibleButtons.size()) * buttonWidth
		+ static_cast<int>(visibleButtons.size() - 1u) * ButtonSpacing;
	int x = std::max(0, (popupWidth - totalWidth) / 2);

	for (auto& button : visibleButtons)
	{
		button->SetPosition({ x, ButtonY });
		x += buttonWidth + ButtonSpacing;
	}
}
