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
		static constexpr unsigned int DefaultWidth = 102u;
		static constexpr unsigned int DefaultHeight = 34u;
		static constexpr unsigned int DefaultMinWidth = 36u;

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
			params.Texture = "rounded_but";
			params.OverTexture = "rounded_but_over";
			params.DownTexture = "rounded_but_down";
			params.Size = { width, DefaultHeight };
			params.MinSize = { DefaultMinWidth, DefaultHeight };
			return params;
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
