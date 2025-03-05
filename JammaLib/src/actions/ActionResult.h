#pragma once

#include <memory>
#include "../base/ActionUndo.h"

namespace base { class GuiElement; };

namespace actions
{
	enum ActionResultType
	{
		ACTIONRESULT_DEFAULT,
		ACTIONRESULT_ID,
		ACTIONRESULT_ACTIVEELEMENT,
		ACTIONRESULT_TOGGLE,
		ACTIONRESULT_ACTIVATE,
		ACTIONRESULT_DITCH,
		ACTIONRESULT_INITSELECT,
		ACTIONRESULT_SELECT,
		ACTIONRESULT_CLEARSELECT,
		ACTIONRESULT_MUTE,
		ACTIONRESULT_UNMUTE,
		ACTIONRESULT_ROUTER,
		ACTIONRESULT_ROUTERINPUT,
		ACTIONRESULT_ROUTEROUTPUT
	};

	struct ActionResult
	{
		bool IsEaten;
		std::string SourceId;
		std::string TargetId;
		ActionResultType ResultType;
		std::shared_ptr<base::ActionUndo> Undo;
		std::weak_ptr<base::GuiElement> ActiveElement;

		static ActionResult NoAction() {
			return { false, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
		}
	};
};
