#pragma once

#include <memory>
#include "GuiElement.h"

namespace gui
{
	// Scene-level single-owner focus authority for retained GUI controls.
	//
	// The manager coordinates which element currently owns the keyboard.  It
	// stores the focused element as a weak_ptr so it never extends an element's
	// lifetime, and it drives the per-element focus flag (GuiElement::RequestFocus
	// / ClearFocus) so exactly one element is focused at a time.
	class GuiFocusManager
	{
	public:
		// Move focus to `element`.  Clears focus on the previous owner first.
		// Passing nullptr is equivalent to ClearFocus().  Returns true if the
		// element accepted focus.
		bool RequestFocus(const std::shared_ptr<base::GuiElement>& element);

		// Clear focus from the current owner, if any.
		void ClearFocus();

		// True if `element` is the current focus owner.
		bool HasFocus(const std::shared_ptr<base::GuiElement>& element) const;

		// The current focus owner, or nullptr.
		std::shared_ptr<base::GuiElement> CurrentFocus() const;

		// True while the focused element is editing text and global keyboard
		// shortcuts should be suppressed.
		bool IsEditingText() const;

	private:
		std::weak_ptr<base::GuiElement> _focus;
	};
}
