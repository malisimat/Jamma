#include "TouchAction.h"

using namespace actions;

TouchAction::TouchAction() :
	State(TouchState::TOUCH_DOWN),
	Touch(TouchType::TOUCH_MOUSE),
	Index(0),
	Value(0),
	Position({0, 0}),
	Modifiers(Action::MODIFIER_NONE)
{
}

TouchAction::~TouchAction()
{
}
