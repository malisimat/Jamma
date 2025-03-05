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
				{}),
			Receiver(std::weak_ptr<base::ActionReceiver>())
		{
		}

		GuiButtonParams(base::GuiElementParams guiParams,
			std::weak_ptr<base::ActionReceiver> receiver) :
			base::GuiElementParams(guiParams),
			Receiver(receiver)
		{}

	public:
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	class GuiButton :
		public base::GuiElement
	{
	public:
		GuiButton(GuiButtonParams guiParams);

	public:
		void SetReceiver(std::weak_ptr<base::ActionReceiver> receiver);

		virtual void Draw(base::DrawContext& ctx) override;

	private:
		GuiButtonParams _buttonParams;
	};
}
