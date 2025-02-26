#include "GuiLabel.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace base;
using namespace gui;
using namespace graphics;
using namespace resources;

GuiLabel::GuiLabel(GuiLabelParams guiParams) :
	GuiElement(guiParams),
	_str(guiParams.String),
	_vertexArray(0),
	_texture(std::weak_ptr<TextureResource>()),
	_shader(std::weak_ptr<ShaderResource>()),
	_font(std::weak_ptr<Font>())
{
}

void GuiLabel::Draw(DrawContext& ctx)
{
	auto font = _font.lock();

	if (!font)
		return;

	auto glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto fontHeight = font->GetHeight();
	auto pos = Position();
	auto size = GetSize();
	auto scale = size.Height / fontHeight;

	glCtx.PushMvp(glm::scale(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)), glm::vec3(scale, scale, 1.0f)));
	
	font->Draw(glCtx, _vertexArray, (unsigned int)_str.size());
}

void GuiLabel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto fontOpt = resourceLib.GetFont(graphics::FontOptions::FONT_LARGE);

	if (!fontOpt.has_value())
		return;

	_font = fontOpt.value();

	auto validated = InitVertexArray();

	utils::GlUtils::CheckError("GuiLabel::_InitResources()");
}

void GuiLabel::_ReleaseResources()
{
	glDeleteVertexArrays(1, &_vertexArray);
}

bool GuiLabel::InitVertexArray()
{
	auto font = _font.lock();

	if (!font)
		return false;
		
	_vertexArray = font->InitVertexArray(_str, GL_STATIC_DRAW);
	return true;
}
