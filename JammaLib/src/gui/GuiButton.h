#pragma once

#include <memory>
#include "GuiElement.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiButtonParams :
		public base::GuiElementParams
	{
	public:
		GuiButtonParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{})
		{
			GuiPassThrough = false;
		}

		GuiButtonParams(base::GuiElementParams guiParams) :
			base::GuiElementParams(guiParams)
		{
			GuiPassThrough = false;
		}
	};

	class GuiButton :
		public base::GuiElement
	{
	public:
		GuiButton(GuiButtonParams guiParams);

	private:
		GuiButtonParams _buttonParams;
	};
}
