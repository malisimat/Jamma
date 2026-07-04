#include "TouchMoveAction.h"

using namespace actions;

TouchMoveAction::TouchMoveAction() :
	Touch(TouchAction::TouchType::TOUCH_MOUSE),
	Index(0),
	MouseButtonsDown(0u),
	Position({ 0, 0 }),
	Modifiers(Action::MODIFIER_NONE)
{
}

TouchMoveAction::~TouchMoveAction()
{
}
