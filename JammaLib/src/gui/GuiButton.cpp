#include "GuiButton.h"

using namespace base;
using namespace gui;

GuiButton::GuiButton(GuiButtonParams params) :
	_buttonParams(params),
	GuiElement(params)
{
}

void GuiButton::SetReceiver(std::weak_ptr<ActionReceiver> receiver)
{
	_buttonParams.Receiver = receiver;
}

void GuiButton::Draw(DrawContext& ctx)
{
	GuiElement::Draw(ctx);
}
