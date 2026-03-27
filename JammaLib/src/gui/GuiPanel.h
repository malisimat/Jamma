#pragma once

#include <memory>
#include "GuiElement.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiPanel :
		public base::GuiElement
	{
	public:
		GuiPanel(base::GuiElementParams guiParams);

	public:
		bool AddChild(const std::shared_ptr<base::GuiElement>& child);
		bool RemoveChild(const std::shared_ptr<base::GuiElement>& child);
	};
}
