#pragma once

#include <memory>
#include "Action.h"
#include "Actionable.h"
#include "../actions/WindowAction.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../actions/KeyAction.h"
#include "../actions/DoubleAction.h"
#include "../actions/TriggerAction.h"
#include "../actions/JobAction.h"
#include "../actions/ActionResult.h"

namespace base
{
	class ActionReceiver :
		public virtual Actionable
	{
	public:
		virtual ActionDirection Direction() const override { return ACTIONDIR_RECEIVE; }
		virtual actions::ActionResult OnAction(actions::WindowAction action)	{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::TouchAction action)		{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action)	{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::KeyAction action)		{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::DoubleAction action)	{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::TriggerAction action)	{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };
		virtual actions::ActionResult OnAction(actions::JobAction action)		{ return { false, "", "", actions::ACTIONRESULT_DEFAULT }; };

		std::shared_ptr<ActionReceiver> shared_from_this()
		{
			return std::dynamic_pointer_cast<ActionReceiver>(
				Sharable::shared_from_this());
		}

	protected:
		virtual void InitReceivers() {};
	};
}
