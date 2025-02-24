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
