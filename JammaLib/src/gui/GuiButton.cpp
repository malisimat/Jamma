#include "GuiButton.h"

using namespace base;
using namespace gui;
using namespace utils;

GuiLabelParams GuiButton::_MakeLabelParams(const GuiButtonParams& params)
{
	GuiLabelParams lp;
	lp.String = params.Text;

	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		params.Size.Width,
		params.Size.Height,
		params.TextPadding,
		params.TextPadding,
		true);

	lp.Position = { (int)frame.PaddingX, frame.OffsetY };
	lp.Size = { frame.ContentWidth, frame.TextHeight };
	lp.MinSize = { 1u, frame.TextHeight };
	return lp;
}

GuiButton::GuiButton(GuiButtonParams params) :
	_buttonParams(params),
	GuiElement(params),
	_label(params.Text.empty() ? nullptr : std::make_shared<GuiLabel>(_MakeLabelParams(params)))
{
	if (_label)
		_children.push_back(_label);
}

void GuiButton::SetSize(Size2d size)
{
	GuiElement::SetSize(size);

	if (!_label)
		return;

	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		size.Width,
		size.Height,
		_buttonParams.TextPadding,
		_buttonParams.TextPadding,
		true);

	_label->SetPosition({ (int)frame.PaddingX, frame.OffsetY });
	_label->SetSize({ frame.ContentWidth, frame.TextHeight });
}
