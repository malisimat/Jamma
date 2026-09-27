#include "GuiPopup.h"

#include <algorithm>

using namespace gui;

namespace gui
{
	class GuiPopupActionButton : public GuiButton
	{
	public:
		GuiPopupActionButton(GuiButtonParams params, unsigned int actionIndex) :
			GuiButton(params),
			_actionIndex(actionIndex)
		{
		}

		actions::ActionResult OnAction(actions::TouchAction action) override
		{
			auto result = GuiButton::OnAction(action);
			if (result.IsEaten && action.State == actions::TouchAction::TOUCH_UP && _receiver)
			{
				actions::GuiAction buttonAction;
				buttonAction.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
				buttonAction.Index = _actionIndex;
				buttonAction.Data = actions::GuiAction::GuiInt{ 1 };
				_receiver->OnAction(buttonAction);
			}
			return result;
		}

	private:
		unsigned int _actionIndex;
	};
}

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
	titleParams.CenterHorizontally = true;
	_titleLabel = std::make_shared<GuiLabel>(titleParams);
	AddChild(_titleLabel);

	for (std::size_t i = 0; i < _lineLabels.size(); ++i)
	{
		GuiLabelParams lineParams;
		lineParams.String = "";
		lineParams.Size = { LineWidth, LineHeight };
		lineParams.MinSize = { 80u, LineHeight };
		lineParams.Position = { 20, 136 - static_cast<int>(i) * 28 };
		lineParams.CenterHorizontally = true;
		_lineLabels[i] = std::make_shared<GuiLabel>(lineParams);
		_lineLabels[i]->SetVisible(false);
		AddChild(_lineLabels[i]);
	}

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
	for (const auto& button : _buttons)
		RemoveChild(button);
	_buttons.clear();

	for (const auto& action : config.Actions)
	{
		if (_buttons.size() == 3u)
			break;
		if (action.Text.empty())
			continue;

		GuiButtonParams params = GuiButtonParams::PanelButton(ButtonWidth);
		params.Text = action.Text;
		params.Position = { 0, ButtonY };
		params.Size = { ButtonWidth, ButtonHeight };
		params.MinSize = { ButtonMinWidth, ButtonHeight };
		params.Index = action.Index;
		auto button = std::make_shared<GuiPopupActionButton>(params, action.Index);
		button->SetReceiver(_buttonReceiver);
		AddChild(button);
		_buttons.push_back(button);
	}

	_LayoutButtons();
}

void GuiPopup::SetButtonReceiver(std::shared_ptr<base::ActionReceiver> receiver)
{
	_buttonReceiver = std::move(receiver);
	for (const auto& button : _buttons)
		button->SetReceiver(_buttonReceiver);
}

void GuiPopup::_LayoutButtons()
{
	if (_buttons.empty())
		return;

	const int popupWidth = static_cast<int>(GetSize().Width);
	const int buttonWidth = static_cast<int>(ButtonWidth);
	const int totalWidth = static_cast<int>(_buttons.size()) * buttonWidth
		+ static_cast<int>(_buttons.size() - 1u) * ButtonSpacing;
	int x = std::max(0, popupWidth - ButtonRightInset - totalWidth);

	for (auto& button : _buttons)
	{
		button->SetPosition({ x, ButtonY });
		x += buttonWidth + ButtonSpacing;
	}
}
