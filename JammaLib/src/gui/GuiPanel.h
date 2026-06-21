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
		void AddChild(std::shared_ptr<base::GuiElement> child) override;
		bool RemoveChild(const std::shared_ptr<base::GuiElement>& child);
	};
}
