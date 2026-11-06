#include "GuiPopup.h"
#include <optional>
#include <utility>

#include <algorithm>

using namespace gui;

namespace gui
{
	class GuiPopupColorRect : public base::GuiElement
	{
	public:
		explicit GuiPopupColorRect(base::GuiElementParams params) : GuiElement(params) {}
		void SetColor(glm::vec3 color) { _guiParams.TintColor = color; }
	};

	class GuiPopupFrameParams
	{
	public:
		static base::GuiElementParams Container(const GuiPopupParams& params)
		{
			auto container = static_cast<base::GuiElementParams>(params);
			// Only the inset fill and border strips draw; no opaque layer beneath them.
			container.Texture.clear();
			container.TextureShader = "texture";
			return container;
		}
	};

	class GuiPopupActionButton : public GuiButton
	{
	public:
		GuiPopupActionButton(GuiButtonParams params, unsigned int actionIndex, bool requirePress) :
			GuiButton(params),
			_requirePress(requirePress),
			_actionIndex(actionIndex)
		{
		}

		actions::ActionResult OnAction(actions::TouchAction action) override
		{
			const bool pressed = _pressed && _pressed->first == action.Touch && _pressed->second == action.Index;
			auto result = GuiButton::OnAction(action);
			if (action.State == actions::TouchAction::TOUCH_DOWN && result.IsEaten)
				_pressed = std::pair{ action.Touch, action.Index };
			else if (action.State == actions::TouchAction::TOUCH_UP && pressed) _pressed.reset();
			if (result.IsEaten && action.State == actions::TouchAction::TOUCH_UP && _receiver
				&& (!_requirePress || pressed))
			{
				actions::GuiAction buttonAction;
				buttonAction.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
				buttonAction.Index = _actionIndex;
				buttonAction.Data = actions::GuiAction::GuiInt{ 1 };
				_receiver->OnAction(buttonAction);
			}
			return result;
		}

		void ClearPointerState() override
		{
			_pressed.reset();
			GuiButton::ClearPointerState();
		}

	private:
		std::optional<std::pair<actions::TouchAction::TouchType, int>> _pressed;
		bool _requirePress;
		unsigned int _actionIndex;
	};

	class GuiPopupRowButton : public GuiButton
	{
	public:
		GuiPopupRowButton(GuiButtonParams params, std::function<void()> action) : GuiButton(params), _action(std::move(action)) {}
		actions::ActionResult OnAction(actions::TouchAction touch) override
		{
			if (touch.Touch == actions::TouchAction::TOUCH_MOUSE && touch.Index != 0) return actions::ActionResult::NoAction();
			const bool pressed = _pressed && _source == touch.Touch && _index == touch.Index;
			auto result = GuiButton::OnAction(touch);
			if (touch.State == actions::TouchAction::TOUCH_DOWN)
			{ _pressed = result.IsEaten; _source = touch.Touch; _index = touch.Index; }
			else if (touch.State == actions::TouchAction::TOUCH_UP && pressed)
			{ _pressed = false; if (result.IsEaten && HitTest(touch.Position) && _action) _action(); }
			return result;
		}
		void ClearPointerState() override { _pressed = false; GuiButton::ClearPointerState(); }
	private:
		std::function<void()> _action;
		bool _pressed = false;
		actions::TouchAction::TouchType _source = actions::TouchAction::TOUCH_MOUSE;
		int _index = 0;
	};
}

GuiPopupParams GuiPopupParams::PanelDefault(VisualMode mode)
{
	GuiPopupParams params;
	params.Mode = mode;
	params.GuiPassThrough = false;
	params.TextureShader = "colour";
	params.Size = { 460u, 210u };
	params.MinSize = { 460u, 210u };
	params.TintColor = mode == VisualMode::Learning ? glm::vec3{0.20f, 0.055f, 0.065f}
		: mode == VisualMode::LearningBlue ? glm::vec3{0.035f, 0.17f, 0.22f}
		: glm::vec3{0.075f, 0.085f, 0.10f};
	params.TextureOpacity = 0.60f;
	return params;
}

base::GuiElementParams GuiPopupParams::FlatRect(unsigned int width, unsigned int height,
	glm::vec3 color, float opacity)
{
	base::GuiElementParams params;
	params.Size = {width,height}; params.MinSize = {0u,0u};
	params.TextureShader = "colour"; params.TintColor = color; params.TextureOpacity = opacity;
	params.GuiPassThrough = true;
	return params;
}

GuiLabelParams GuiPopupParams::Header(const std::string& text, unsigned int width, unsigned int height)
{
	auto params = GuiLabelParams::PanelHeader(text, static_cast<int>(width), height);
	params.FontSize = graphics::FontOptions::FONT_SMALL;
	params.Ellipsize = true;
	return params;
}

base::GuiElementParams GuiPopupParams::HeaderBand(unsigned int width, unsigned int height)
{
	// A neutral 50% band over the 60% popup fill gives 80% effective opacity.
	return FlatRect(width,height,{0.18f,0.18f,0.18f},0.70f);
}

GuiPopup::GuiPopup(const GuiPopupParams& params) :
	GuiPanel(GuiPopupFrameParams::Container(params)), _preferredSize(params.Size)
{
	_fill = std::make_shared<GuiPopupColorRect>(GuiPopupParams::FlatRect(params.Size.Width - 8u,
		params.Size.Height - 8u,{0.25f,0.28f,0.31f},params.TextureOpacity));
	_fill->SetPosition({4,4}); AddChild(_fill);
	for (auto& edge : _border)
	{
		edge = std::make_shared<GuiPopupColorRect>(GuiPopupParams::FlatRect(0u,0u,params.TintColor,0.96f));
		_fill->AddChild(edge);
	}
	SetVisualMode(params.Mode);
	_headerBand = std::make_shared<base::GuiElement>(GuiPopupParams::HeaderBand(params.Size.Width - 16u, 36u));
	_headerBand->SetVisible(false);
	AddChild(_headerBand);
	GuiLabelParams titleParams = GuiPopupParams::Header("", TitleWidth, TitleHeight);
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
	if (_headerBand) _headerBand->SetVisible(!text.empty());
	if (_titleLabel)
	{
		_titleLabel->SetString(text);
		_titleLabel->SetVisible(!text.empty());
	}
}

void GuiPopup::SetVisualMode(GuiPopupParams::VisualMode mode)
{
	_guiParams.TintColor = GuiPopupParams::PanelDefault(mode).TintColor;
	for (auto& edge : _border) edge->SetColor(_guiParams.TintColor);
	_fill->SetColor(mode == GuiPopupParams::VisualMode::Learning ? glm::vec3{0.36f,0.18f,0.19f}
		: mode == GuiPopupParams::VisualMode::LearningBlue ? glm::vec3{0.16f,0.32f,0.37f}
		: glm::vec3{0.25f,0.28f,0.31f});
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
	_actions.clear();

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
		auto button = std::make_shared<GuiPopupActionButton>(params, action.Index, config.RequirePress);
		button->SetReceiver(_buttonReceiver);
		AddChild(button);
		_buttons.push_back(button);
		_actions.push_back(action);
	}

	_LayoutContents();
}

void GuiPopup::SetButtonReceiver(std::shared_ptr<base::ActionReceiver> receiver)
{
	_buttonReceiver = std::move(receiver);
	for (const auto& button : _buttons)
		button->SetReceiver(_buttonReceiver);
}

void GuiPopup::SetActionEnabled(unsigned int index, bool enabled)
{
	for (std::size_t i = 0; i < _buttons.size(); ++i)
		if (_actions[i].Index == index) _buttons[i]->SetEnabled(enabled);
}

void GuiPopup::SetRowText(const std::string& subtitle, const std::string& status)
{
	_rowSubtitle->SetString(subtitle); _rowStatus->SetString(status);
	_rowSubtitle->SetVisible(!subtitle.empty()); _rowStatus->SetVisible(!status.empty());
}

void GuiPopup::SetRows(const std::vector<GuiPopupRow>& rows, std::function<void(std::size_t)> primary,
	std::function<void(std::size_t)> remove, std::optional<std::size_t> reveal)
{
	if (!_rowScroll)
	{
		GuiScrollPanelParams scroll;
		scroll.Size = {1u,1u}; scroll.MinSize = {1u,1u}; scroll.ScrollBarWidth = 12u;
		_rowScroll = std::make_shared<GuiScrollPanel>(scroll); AddChild(_rowScroll);
		_rowSubtitle = std::make_shared<GuiLabel>(GuiPopupParams::Header("",420u)); AddChild(_rowSubtitle);
		_rowStatus = std::make_shared<GuiLabel>(GuiPopupParams::Header("",420u,38u)); AddChild(_rowStatus);
		_rowSubtitle->SetVisible(false); _rowStatus->SetVisible(false);
	}
	_rowScroll->SetVisible(true);
	_LayoutContents();
	const int offset = _rowScroll->ScrollOffset();
	auto content = std::make_shared<GuiPanel>(base::GuiElementParams{});
	const bool compact = _rowScroll->GetSize().Width < 190u;
	const unsigned int rowHeight = compact ? 86u : 62u;
	const unsigned int height = (std::max)(rowHeight, static_cast<unsigned int>(rows.size()) * rowHeight);
	const unsigned int panelWidth = _rowScroll->GetSize().Width;
	const unsigned int width = panelWidth > 12u && height > _rowScroll->ViewportHeight() ? panelWidth - 12u : panelWidth;
	content->SetSize({width,height});
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		const int y = static_cast<int>(height - (i + 1u) * rowHeight);
		const int inset = (std::min)(4, static_cast<int>(width) / 8);
		const int gap = inset;
		const int primaryWidth = compact ? (std::min)(52, static_cast<int>(width) / 3)
			: std::clamp(static_cast<int>(width / 4u), 36, 92);
		const int detailX = compact ? inset + primaryWidth + gap : 4;
		const int detailWidth = compact ? (std::max)(0, static_cast<int>(width) - detailX - inset)
			: (std::max)(40, static_cast<int>(width) - primaryWidth - 28 - 20);
		const int removeWidth = compact ? (std::min)(28, detailWidth) : 28;
		base::GuiElementParams box;
		box.Size = {static_cast<unsigned int>(detailWidth),compact ? 50u : rowHeight - 8u};
		box.MinSize = {1u,1u}; box.Texture = "rounded_but_on"; box.TextureShader = "texture_tinted";
		box.GuiPassThrough = true; box.TextureOpacity = 0.72f;
		box.Position = {detailX,y + (compact ? 32 : 4)};
		box.TintColor = rows[i].Active ? glm::vec3{0.26f,0.39f,0.50f} : rows[i].Recent ? glm::vec3{0.07f,0.35f,0.43f} : GuiStyle::Control();
		content->AddChild(std::make_shared<base::GuiElement>(box));
		// Only the row currently learning gets an additional outline.
		if (rows[i].Active)
		{
			const int edge = (std::min)(2, detailWidth);
			const auto edgeColor = glm::vec3{0.12f,0.57f,0.78f};
			const int boxHeight = compact ? 50 : static_cast<int>(rowHeight) - 8;
			for (const auto& rect : {std::array<int,4>{detailX,box.Position.Y,detailWidth,edge},
				std::array<int,4>{detailX,box.Position.Y + boxHeight - edge,detailWidth,edge},
				std::array<int,4>{detailX,box.Position.Y,edge,boxHeight},
				std::array<int,4>{detailX + detailWidth - edge,box.Position.Y,edge,boxHeight}})
			{
				auto line = GuiPopupParams::FlatRect(static_cast<unsigned int>(rect[2]),static_cast<unsigned int>(rect[3]),edgeColor,0.94f);
				line.Position = {rect[0],rect[1]}; content->AddChild(std::make_shared<base::GuiElement>(line));
			}
		}
		const int textInset = compact ? (std::min)(8, detailWidth / 2) : 16;
		const auto textWidth = static_cast<unsigned int>((std::max)(0,
			detailWidth - textInset - (compact ? textInset : 8)));
		auto detail = GuiPopupParams::Header(rows[i].Detail, textWidth, compact ? 46u : rowHeight - 12u);
		detail.MinSize.Width = 0u;
		detail.Position = {detailX + textInset,y + (compact ? 34 : 6)}; detail.ClipText = true;
		auto detailLabel = std::make_shared<GuiLabel>(detail);
		// Zero width means dynamic sizing to GuiLabel; an empty frame must stay hidden.
		detailLabel->SetVisible(textWidth > 0u);
		content->AddChild(detailLabel);
		auto buttonParams = GuiButtonParams::PanelButton(static_cast<unsigned int>(primaryWidth));
		// The primary action spans the row even when details and removal stack beside it.
		buttonParams.MinSize = {0u,1u}; buttonParams.Size = {static_cast<unsigned int>(primaryWidth),rowHeight - 8u};
		buttonParams.Position = {compact ? inset : detailWidth + 8,y + 4}; buttonParams.Text = rows[i].PrimaryAction;
		auto primaryButton = std::make_shared<GuiPopupRowButton>(buttonParams,[primary,i] { if (primary) primary(i); });
		primaryButton->SetEnabled(rows[i].ActionsEnabled); content->AddChild(primaryButton);
		buttonParams.Size = {static_cast<unsigned int>(removeWidth),static_cast<unsigned int>(removeWidth)};
		buttonParams.Position = {compact ? static_cast<int>(width) - removeWidth - inset : detailWidth + primaryWidth + 12,
			compact ? y + 4 : y + static_cast<int>(rowHeight) - removeWidth - 4};
		buttonParams.Text = "X";
		auto removeButton = std::make_shared<GuiPopupRowButton>(buttonParams,[remove,i] { if (remove) remove(i); });
		removeButton->SetEnabled(rows[i].ActionsEnabled); content->AddChild(removeButton);
	}
	_rowScroll->SetContent(content);
	_rowScroll->SetScrollOffset(offset);
	_rowScroll->SetVisible(true);
	for (auto& line : _lineLabels) line->SetVisible(false);
	_LayoutContents();
	if (reveal && *reveal < rows.size())
	{
		const int top = static_cast<int>(height - *reveal * rowHeight);
		_rowScroll->ScrollToVisible({0,top - static_cast<int>(rowHeight),static_cast<int>(_rowScroll->ViewportWidth()),top});
	}
}

void GuiPopup::SetKeyAction(std::function<bool(actions::KeyAction)> action) { _keyAction = std::move(action); }

actions::ActionResult GuiPopup::OnAction(actions::KeyAction action)
{
	if (_keyAction && _keyAction(action))
		return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
	return GuiPanel::OnAction(action);
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
	if (_rowScroll && _rowScroll->IsVisible())
	{
		int right = popupWidth - 8;
		for (std::size_t i = _buttons.size(); i-- > 0;)
		{
			const int w = (std::min)(static_cast<int>(ButtonWidth), (std::max)(1, popupWidth / 4));
			const int x = _actions[i].PinLeft ? 8 : right - w;
			_buttons[i]->SetPosition({x,12});
			_buttons[i]->SetSize({static_cast<unsigned int>(w),static_cast<unsigned int>((std::min)(36,height))});
			if (!_actions[i].PinLeft) right = x - 8;
		}
		return;
	}
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
	const int fillInsetX = (std::min)(4,width / 2), fillInsetY = (std::min)(4,height / 2);
	_fill->SetPosition({fillInsetX,fillInsetY});
	_fill->SetSize({static_cast<unsigned int>(width - 2 * fillInsetX),
		static_cast<unsigned int>(height - 2 * fillInsetY)});
	// Border strips surround the fill without stacking another background under it.
	const std::array<std::array<int,4>,4> edges{{
		{0,0,width,fillInsetY}, {0,height - fillInsetY,width,fillInsetY},
		{0,fillInsetY,fillInsetX,height - 2 * fillInsetY},
		{width - fillInsetX,fillInsetY,fillInsetX,height - 2 * fillInsetY}}};
	for (std::size_t i = 0; i < edges.size(); ++i)
	{
		_border[i]->SetPosition({edges[i][0] - fillInsetX,edges[i][1] - fillInsetY});
		_border[i]->SetSize({static_cast<unsigned int>(edges[i][2]),static_cast<unsigned int>(edges[i][3])});
	}
	if (_rowScroll && _rowScroll->IsVisible())
	{
		const unsigned int inner = static_cast<unsigned int>((std::max)(1,width - 24));
		_headerBand->SetPosition({8,(std::max)(0,height - 44)});
		_headerBand->SetSize({static_cast<unsigned int>((std::max)(0,width - 16)),static_cast<unsigned int>((std::min)(36,height))});
		_titleLabel->SetPosition({12,(std::max)(0,height - 38)});
		_titleLabel->SetSize({inner,26u});
		_rowSubtitle->SetPosition({12,(std::max)(0,height - 66 - GuiLabelParams::HeaderBottomSpacing)}); _rowSubtitle->SetSize({inner,26u});
		_rowStatus->SetPosition({12,(std::max)(0,height - 106 - GuiLabelParams::HeaderBottomSpacing)}); _rowStatus->SetSize({inner,38u});
		_rowScroll->SetPosition({12,56});
		_rowScroll->SetSize({inner, static_cast<unsigned int>((std::max)(1,height - 164 - GuiLabelParams::HeaderBottomSpacing))});
		return;
	}
	const int padding = (std::min)(20, width / 2);
	const int titleHeight = (std::min)(static_cast<int>(TitleHeight), height);
	const int topInset = (std::min)(height >= static_cast<int>(_preferredSize.Height) ? 14 : 8, height - titleHeight);
	const int titleY = height - topInset - titleHeight;
	if (_headerBand)
	{
		_headerBand->SetPosition({(std::min)(8,width), (std::max)(0, titleY - 4)});
		_headerBand->SetSize({static_cast<unsigned int>((std::max)(0, width - 16)),
			static_cast<unsigned int>((std::min)(height, titleHeight + 8))});
	}
	if (_titleLabel) {
		_titleLabel->SetPosition({ padding, titleY });
		_titleLabel->SetSize({ static_cast<unsigned int>((std::min)(static_cast<int>(TitleWidth), width - padding * 2)),
			static_cast<unsigned int>(titleHeight) });
	}
	const int bodyTop = (std::max)(0, titleY - 10 - GuiLabelParams::HeaderBottomSpacing);
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
