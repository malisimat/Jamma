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
		static constexpr unsigned int DefaultHeight = GuiButtonParams::DefaultHeight;
		static constexpr unsigned int PrimaryWidth = 80u;
		static constexpr unsigned int PrimaryMinWidth = 20u;
		static constexpr unsigned int SecondaryWidth = 10u;
		static constexpr unsigned int SecondaryMinWidth = 10u;

		enum ToggleState
		{
			TOGGLE_OFF,
			TOGGLE_ON
		};

	public:

		static void ApplyPanelTextures(GuiToggleParams& params)
		{
			params.Texture = "rounded_but";
			params.OverTexture = "rounded_but_over";
			params.DownTexture = "rounded_but_down";
			params.ToggledTexture = "rounded_but_on";
			params.ToggledOverTexture = "rounded_but_on_over";
			params.ToggledDownTexture = "rounded_but_on_down";
		}

		static GuiToggleParams PanelPrimary()
		{
			GuiToggleParams params;
			ApplyPanelTextures(params);
			params.OutTexture = "";
			params.Size = { PrimaryWidth, DefaultHeight };
			params.MinSize = { PrimaryMinWidth, DefaultHeight };
			return params;
		}

		static GuiToggleParams PanelSecondary()
		{
			GuiToggleParams params;
			ApplyPanelTextures(params);
			params.OutTexture = "rounded_but_on";
			params.Size = { SecondaryWidth, DefaultHeight };
			params.MinSize = { SecondaryMinWidth, DefaultHeight };
			return params;
		}
		GuiToggleParams() :
			GuiButtonParams(),
			ToggleIndex(0u),
			InitState(TOGGLE_OFF),
			ToggledTexture(""),
			ToggledOverTexture(""),
			ToggledDownTexture(""),
			ToggledOutTexture("")
		{}

		GuiToggleParams(base::GuiElementParams guiParams,
			unsigned int toggleIndex) :
			GuiButtonParams(guiParams),
			ToggleIndex(toggleIndex),
			InitState(TOGGLE_OFF),
			ToggledTexture(""),
			ToggledOverTexture(""),
			ToggledDownTexture(""),
			ToggledOutTexture("")
		{}

	public:
		unsigned int ToggleIndex;
		ToggleState InitState;
		std::string ToggledTexture;
		std::string ToggledOverTexture;
		std::string ToggledDownTexture;
		std::string ToggledOutTexture;
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	class GuiToggle :
		public gui::GuiButton
	{
	public:
		GuiToggle(GuiToggleParams guiParams);

	public:
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;

		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

		GuiToggleParams::ToggleState Toggle();
		GuiToggleParams::ToggleState GetToggleState() const;
		void SetToggleState(GuiToggleParams::ToggleState state, bool bypassUpdates);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual void _OnToggleChange(bool bypassUpdates);

	private:
		unsigned int _toggleIndex;
		GuiToggleParams::ToggleState _toggleState;
		graphics::Image _toggledTexture;
		graphics::Image _toggledOverTexture;
		graphics::Image _toggledDownTexture;
		graphics::Image _toggledOutTexture;
		GuiToggleParams _buttonParams;
	};
}
