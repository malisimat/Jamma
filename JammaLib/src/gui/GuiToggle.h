#pragma once

#include <memory>
#include "GuiButton.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiToggleParams :
		public GuiButtonParams
	{
	public:
		enum ToggleState
		{
			TOGGLE_OFF,
			TOGGLE_ON
		};

	public:
		GuiToggleParams() :
			GuiButtonParams(),
			InitState(TOGGLE_OFF),
			ToggledTexture(""),
			ToggledOverTexture(""),
			ToggledDownTexture(""),
			ToggledOutTexture("")
		{}

		GuiToggleParams(base::GuiElementParams guiParams,
			std::weak_ptr<base::ActionReceiver> receiver) :
			GuiButtonParams(guiParams, receiver),
			InitState(TOGGLE_OFF),
			ToggledTexture(""),
			ToggledOverTexture(""),
			ToggledDownTexture(""),
			ToggledOutTexture("")
		{}

	public:
		ToggleState InitState;
		std::string ToggledTexture;
		std::string ToggledOverTexture;
		std::string ToggledDownTexture;
		std::string ToggledOutTexture;
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	class GuiToggle :
		public base::GuiElement
	{
	public:
		GuiToggle(GuiToggleParams guiParams);

	public:
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;

		virtual actions::ActionResult OnAction(actions::TouchAction action) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual void _OnToggleChange(bool bypassUpdates);

	private:
		GuiToggleParams::ToggleState _toggleState;
		graphics::Image _toggledTexture;
		graphics::Image _toggledOverTexture;
		graphics::Image _toggledDownTexture;
		graphics::Image _toggledOutTexture;
		GuiToggleParams _buttonParams;
	};
}
