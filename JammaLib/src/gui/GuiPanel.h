#pragma once

#include <memory>
#include "GuiElement.h"
#include "ActionReceiver.h"

namespace gui
{
	// UI-owned binding adapter. Tree indices stay local; command identity stays
	// explicit, and the weak owner prevents a binding extending Scene lifetime.
	class GuiCommandReceiver : public base::ActionReceiver
	{
	public:
		GuiCommandReceiver(unsigned int commandIndex, std::weak_ptr<base::ActionReceiver> owner = {}) :
			_commandIndex(commandIndex), _owner(std::move(owner)) {}
		void SetOwner(std::weak_ptr<base::ActionReceiver> owner) { _owner = std::move(owner); }
		using base::ActionReceiver::OnAction;
		actions::ActionResult OnAction(actions::GuiAction action) override;
	private:
		unsigned int _commandIndex;
		std::weak_ptr<base::ActionReceiver> _owner;
	};

	class GuiPanel :
		public base::GuiElement
	{
	public:
		GuiPanel(base::GuiElementParams guiParams);

	public:
		void AddChild(std::shared_ptr<base::GuiElement> child) override;
		bool RemoveChild(const std::shared_ptr<base::GuiElement>& child);
	};
}
