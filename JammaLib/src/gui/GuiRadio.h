#pragma once

#include <memory>
#include "GuiToggle.h"
#include "GuiButton.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiRadioParams :
		public GuiButtonParams
	{
	public:
		GuiRadioParams() :
			GuiButtonParams(),
			InitValue(0u)
		{}

		GuiRadioParams(base::GuiElementParams guiParams,
			std::weak_ptr<base::ActionReceiver> receiver) :
			GuiButtonParams(guiParams, receiver),
			InitValue(0u)
		{}

	public:
		unsigned int InitValue;
		std::vector<GuiToggleParams> ToggleParams;
	};

	class GuiRadio :
		public GuiButton
	{
	public:
		GuiRadio(GuiRadioParams guiParams);

	public:
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;

		unsigned int CurrentValue() const;
		void SetCurrentValue(unsigned int value, bool bypassUpdates);

	protected:
		const static unsigned int _ToggleWidth;
		const static unsigned int _ToggleHeight;
		const static unsigned int _ToggleGap;

		virtual void _InitReceivers() override;
		virtual void _OnRadioValueChange(unsigned int value, bool bypassUpdates);

	private:
		unsigned int _currentValue;
		GuiRadioParams _radioParams;
	};
}
