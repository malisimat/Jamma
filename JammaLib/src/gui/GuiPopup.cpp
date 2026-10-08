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
	params.Texture = "rounded_but_on";
	params.OverTexture = "rounded_but_on";
	params.DownTexture = "rounded_but_on";
	params.Size = { 460u, 210u };
	params.MinSize = { 460u, 210u };
	params.TintColor = GuiStyle::Graphite();
	params.TextureOpacity = GuiStyle::PanelFillOpacity;
	return params;
}

GuiPopup::GuiPopup(const GuiPopupParams& params) :
	GuiPanel(params), _preferredSize(params.Size)
{
	GuiLabelParams titleParams;
	titleParams.String = "";
	titleParams.Size = { TitleWidth, TitleHeight };
	titleParams.MinSize = { 100u, TitleHeight };
	titleParams.Position = { 20, 170 };
	titleParams.CenterHorizontally = true;
	titleParams.VerticalAlign = GuiTextVerticalAlign::Center;
	titleParams.ClipText = true;
	titleParams.Ellipsize = true;
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
		lineParams.VerticalAlign = GuiTextVerticalAlign::Top;
		lineParams.ClipText = true;
		lineParams.Ellipsize = true;
		_lineLabels[i] = std::make_shared<GuiLabel>(lineParams);
		_lineLabels[i]->SetVisible(false);
		AddChild(_lineLabels[i]);
	}
	_LayoutContents();
}

void GuiPopup::SetSize(utils::Size2d size)
{
	GuiPanel::SetSize(size);
	_LayoutContents();
}

void GuiPopup::FitToViewport(utils::Size2d viewport)
{
	const utils::Size2d size{ (std::min)(viewport.Width, _preferredSize.Width),
		(std::min)(viewport.Height, _preferredSize.Height) };
	SetSize(size);
	SetPosition({ static_cast<int>((viewport.Width - size.Width) / 2),
		static_cast<int>((viewport.Height - size.Height) / 2) });
}

void GuiPopup::Draw(base::DrawContext& context)
{
	if (GetSize().Width == 0 || GetSize().Height == 0) return;
	GuiPanel::Draw(context);
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
	_LayoutContents();
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

	_LayoutContents();
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
	const int height = static_cast<int>(GetSize().Height);
	const int leftInset = (std::min)(20, popupWidth / 2);
	const int rightInset = (std::min)(popupWidth >= static_cast<int>(_preferredSize.Width) ? ButtonRightInset : leftInset,
		popupWidth - leftInset);
	const int available = popupWidth - leftInset - rightInset;
	const int count = static_cast<int>(_buttons.size());
	const int gap = count > 1 ? (std::min)(ButtonSpacing, available / (count * 2)) : 0;
	const int buttonWidth = (std::min)(static_cast<int>(ButtonWidth), (available - gap * (count - 1)) / count);
	const int buttonHeight = (std::min)(static_cast<int>(ButtonHeight), height);
	const int y = (std::min)(height >= static_cast<int>(_preferredSize.Height) ? ButtonY : 12, height - buttonHeight);
	const int totalWidth = count * buttonWidth + (count - 1) * gap;
	int x = popupWidth - rightInset - totalWidth;

	for (auto& button : _buttons)
	{
		button->SetPosition({ x, y });
		button->SetSize({ static_cast<unsigned int>(buttonWidth), static_cast<unsigned int>(buttonHeight) });
		x += buttonWidth + gap;
	}
}

void GuiPopup::_LayoutContents()
{
	_LayoutButtons();
	const int width = static_cast<int>(GetSize().Width), height = static_cast<int>(GetSize().Height);
	const int padding = (std::min)(20, width / 2);
	const int titleHeight = (std::min)(static_cast<int>(TitleHeight), height);
	const int topInset = (std::min)(height >= static_cast<int>(_preferredSize.Height) ? 14 : 8, height - titleHeight);
	const int titleY = height - topInset - titleHeight;
	if (_titleLabel) {
		_titleLabel->SetPosition({ padding, titleY });
		_titleLabel->SetSize({ static_cast<unsigned int>((std::min)(static_cast<int>(TitleWidth), width - padding * 2)),
			static_cast<unsigned int>(titleHeight) });
	}
	const int bodyTop = (std::max)(0, titleY - 10);
	const int buttonsTop = _buttons.empty() ? 0 : _buttons.front()->Position().Y + static_cast<int>(_buttons.front()->GetSize().Height);
	const int bodyBottom = (std::min)(bodyTop, buttonsTop + (height >= static_cast<int>(_preferredSize.Height) ? 12 : 8));
	const int count = static_cast<int>(std::count_if(_lineLabels.begin(), _lineLabels.end(),
		[](const auto& label) { return label && label->IsVisible(); }));
	const int gap = count > 1 ? (std::min)(4, (bodyTop - bodyBottom) / (count * 2)) : 0;
	const int lineHeight = count ? (std::min)(static_cast<int>(LineHeight), (bodyTop - bodyBottom - gap * (count - 1)) / count) : 0;
	int row = 0;
	for (auto& label : _lineLabels) {
		if (!label) continue;
		const int y = label->IsVisible() ? bodyTop - lineHeight - row++ * (lineHeight + gap) : bodyBottom;
		label->SetPosition({ padding, y });
		label->SetSize({ static_cast<unsigned int>((std::min)(static_cast<int>(LineWidth), width - padding * 2)),
			static_cast<unsigned int>(label->IsVisible() ? lineHeight : 0) });
	}
}
