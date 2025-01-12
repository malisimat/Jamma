#include "KeyAction.h"

using namespace actions;

KeyAction::KeyAction() :
	KeyActionType(KeyActionType::KEY_DOWN),
	KeyChar(0),
	IsSystem(false),
	Modifiers(Action::MODIFIER_NONE)
{
}

KeyAction::~KeyAction()
{
}
