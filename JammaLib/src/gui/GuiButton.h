#pragma once

#include <memory>
#include <optional>
#include "GuiElement.h"
#include "GuiLabel.h"
#include "GuiPanel.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiButtonParams :
		public base::GuiElementParams
	{
	public:
		static constexpr unsigned int DefaultWidth = 102u;
		static constexpr unsigned int DefaultHeight = GuiStyle::ControlHeight;
		static constexpr unsigned int DefaultMinWidth = 36u;
		static constexpr unsigned int DefaultTextPadding = GuiStyle::TextPadding;

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

		static GuiButtonParams PanelButton(unsigned int width = DefaultWidth)
		{
			GuiButtonParams params;
			params.TextureShader = "texture_tinted";
			params.TintColor = GuiStyle::Control();
			params.Texture = "rounded_but";
			params.OverTexture = "rounded_but_over";
			params.DownTexture = "rounded_but_down";
			params.Size = { width, DefaultHeight };
			params.MinSize = { DefaultMinWidth, DefaultHeight };
			return params;
		}

	public:
		std::string Text;
		glm::vec3 TextColor{1.0f};
		// Unset state colours fall back to TextColor; pressed falls back to hover.
		std::optional<glm::vec3> OverTextColor;
		std::optional<glm::vec3> DownTextColor;
		// Existing button subclasses dispatch their own actions. Plain command
		// buttons opt in so one activation cannot submit two different commands.
		bool DispatchCommand = false;
		// Destructive modifier gestures must retain the intent at pointer press.
		bool CapturePressModifiers = false;
		unsigned int TextPadding = DefaultTextPadding;
		std::optional<graphics::FontOptions::FontSize> TextFontSize;
		bool TextEllipsize = false;
	};

	class GuiButton :
		public base::GuiElement
	{
	public:
		GuiButton(GuiButtonParams guiParams);
		void Draw(base::DrawContext& context) override;
		using base::GuiElement::OnAction;
		virtual void SetSize(utils::Size2d size) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		actions::ActionResult OnAction(actions::KeyAction action) override;
		void ClearPointerState() override;
		void SetText(const std::string& text);
		const std::string& Text() const noexcept { return _buttonParams.Text; }

	private:
		static GuiLabelParams _MakeLabelParams(const GuiButtonParams& params);

		bool _pressed = false;
		base::Action::Modifiers _pressModifiers = base::Action::MODIFIER_NONE;
		GuiButtonParams _buttonParams;
		std::shared_ptr<GuiLabel> _label;
	};
}
