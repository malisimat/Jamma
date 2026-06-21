#pragma once

#include <memory>
#include <vector>
#include "GuiElement.h"
#include "../actions/TouchAction.h"
#include "../actions/KeyAction.h"

namespace gui
{
	// Scene-level popup layer + input capture.
	//
	// Popups are already-initialised GuiElements positioned in global (scene
	// overlay) coordinates.  The host renders them after the normal GUI tree and
	// captures input while any popup is open:
	//   - input is routed to the topmost popup first;
	//   - a press outside the topmost popup dismisses it (outside-click dismiss);
	//   - Escape dismisses the topmost popup.
	// The host does not own resource lifetime; the opening control initialises
	// and releases its own popup content.
	class GuiPopupHost
	{
	public:
		// Push a popup onto the stack.  `owner` is informational (the control
		// that opened it) and is held weakly.
		void Open(std::shared_ptr<base::GuiElement> element,
			std::shared_ptr<base::GuiElement> owner = nullptr);

		// Close the topmost popup.  No-op when empty.
		void Close();
		void CloseAll();

		bool IsOpen() const;
		std::shared_ptr<base::GuiElement> Top() const;
		std::shared_ptr<base::GuiElement> OwnerOfTop() const;

		void Draw(base::DrawContext& ctx);

		// While a popup is open these capture input.  Returns an eaten result so
		// the scene does not fall through to controls beneath the popup.
		actions::ActionResult OnAction(actions::TouchAction action);
		actions::ActionResult OnAction(actions::TouchMoveAction action);
		actions::ActionResult OnAction(actions::KeyAction action);

	private:
		struct Popup
		{
			std::shared_ptr<base::GuiElement> Element;
			std::weak_ptr<base::GuiElement>   Owner;
		};

		std::vector<Popup> _popups;
	};
}
