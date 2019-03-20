#include "GuiButton.h"

GuiButton::GuiButton(GuiButtonParams params) :
	_buttonParams(params),
	Touchable(params),
	GuiElement(params)
{
}

GuiButton::~GuiButton()
{
}

void GuiButton::SetReceiver(std::weak_ptr<ActionReceiver> receiver)
{
	_buttonParams.Receiver = receiver;
}

void GuiButton::Draw(DrawContext& ctx)
{
}

void GuiButton::OnTouchBegin(TouchType touchType, int num)
{

}

void GuiButton::OnTouchEnd(TouchType touchType, int num)
{
	auto receiver = _buttonParams.Receiver.lock();

	//if (receiver)
	//	receiver->OnAction();
}

void GuiButton::OnDrag(TouchType touchType, int num)
{
}