#include "TouchMoveAction.h"

using namespace actions;

TouchMoveAction::TouchMoveAction() :
	Touch(TouchAction::TouchType::TOUCH_MOUSE),
	Index(0),
	Position({ 0, 0 }),
	Modifiers(Action::MODIFIER_NONE)
{
}

TouchMoveAction::~TouchMoveAction()
{
}
