#pragma once

#include <memory>
#include "GuiElement.h"
#include "Drawable.h"
#include "../graphics/Font.h"
#include "../resources/ResourceLib.h"

namespace gui
{
	class GuiLabelParams : public base::GuiElementParams
	{
	public:
		GuiLabelParams() :
			base::GuiElementParams()
		{}

		GuiLabelParams(base::GuiElementParams params,
			std::string string) :
			base::GuiElementParams(params),
			String(string)
		{}

	public:
		std::string String;
	};

	class GuiLabel :
		public base::GuiElement
	{
	public:
		GuiLabel(GuiLabelParams guiParams);

	public:
		virtual void Draw(base::DrawContext& ctx) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		bool InitVertexArray();

	private:
		std::string _str;
		GLuint _vertexArray;
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
		std::weak_ptr<graphics::Font> _font;
	};
}
