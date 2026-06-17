#include "GuiLabel.h"
#include "../graphics/GlDeleteQueue.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace base;
using namespace gui;
using namespace graphics;
using namespace resources;

GuiLabel::GuiLabel(GuiLabelParams guiParams) :
	GuiElement(guiParams),
	_str(guiParams.String),
	_pendingStr(guiParams.String),
	_vertexArrayDirty(true),
	_vertexArray(0),
	_vertexBuffers{ 0, 0 },
	_texture(std::weak_ptr<TextureResource>()),
	_shader(std::weak_ptr<ShaderResource>()),
	_font(std::weak_ptr<Font>())
{
}

void GuiLabel::SetString(const std::string& str)
{
	std::lock_guard<std::mutex> lock(_stringMutex);
	if (_pendingStr == str)
		return;

	_pendingStr = str;
	_vertexArrayDirty.store(true, std::memory_order_release);
}

void GuiLabel::Draw(DrawContext& ctx)
{
	auto font = _font.lock();

	if (!font)
		return;

	SyncVertexArray();
	if (_vertexArray == 0)
		return;

	auto glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto fontHeight = font->GetHeight();
	auto pos = Position();
	auto size = GetSize();
	auto scale = size.Height / fontHeight;

	glCtx.PushMvp(glm::scale(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)), glm::vec3(scale, scale, 1.0f)));
	
	font->Draw(glCtx, _vertexArray, (unsigned int)_str.size());
	glCtx.PopMvp();
}

void GuiLabel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto fontOpt = resourceLib.GetFont(graphics::FontOptions::FONT_LARGE);

	if (!fontOpt.has_value())
	{
		std::cout << "GuiLabel::_InitResources missing FONT_LARGE resource" << std::endl;
		return;
	}

	_font = fontOpt.value();
	_vertexArrayDirty.store(true, std::memory_order_release);

	auto validated = InitVertexArray();
	if (!validated)
		std::cout << "GuiLabel::_InitResources failed to build initial vertex array for label text: '" << _str << "'" << std::endl;

	utils::GlUtils::CheckError("GuiLabel::_InitResources()");
}

void GuiLabel::_ReleaseResources()
{
	graphics::GlDeleteQueue::DeleteBuffers(2, _vertexBuffers);
	_vertexBuffers[0] = 0;
	_vertexBuffers[1] = 0;
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

bool GuiLabel::InitVertexArray()
{
	auto font = _font.lock();

	if (!font)
		return false;
		
	SyncVertexArray();
	return true;
}

void GuiLabel::SyncVertexArray()
{
	if (!_vertexArrayDirty.exchange(false, std::memory_order_acq_rel))
		return;

	std::string next;
	{
		std::lock_guard<std::mutex> lock(_stringMutex);
		next = _pendingStr;
	}

	auto font = _font.lock();
	if (!font)
		return;

	graphics::GlDeleteQueue::DeleteBuffers(2, _vertexBuffers);
	_vertexBuffers[0] = 0;
	_vertexBuffers[1] = 0;
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = font->InitVertexArray(next, GL_STATIC_DRAW, &_vertexBuffers[0], &_vertexBuffers[1]);
	_str = next;
}
