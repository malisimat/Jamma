#include "GuiLabel.h"
#include "../graphics/GlDeleteQueue.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include <algorithm>
#include <cmath>

using namespace base;
using namespace gui;
using namespace graphics;
using namespace resources;

GuiLabel::GuiLabel(GuiLabelParams guiParams) :
	GuiElement(guiParams),
	_str(guiParams.String),
	_pendingStr(guiParams.String),
	_textInset{ guiParams.TextInsetX, guiParams.TextInsetY },
	_centerHorizontally(guiParams.CenterHorizontally),
	_ellipsize(guiParams.Ellipsize),
	_vertexArrayDirty(true),
	_vertexArray(0),
	_vertexBuffers{ 0, 0 },
	_font(std::weak_ptr<Font>()),
	_resourceLib(nullptr),
	_selectedFontSize(graphics::FontOptions::FONT_LARGE),
	_resolvedTextPixelHeight(0u)
{
}

void GuiLabel::SetSize(utils::Size2d size)
{
	const bool widthChanged = GetSize().Width != size.Width;
	GuiElement::SetSize(size);
	if (_ellipsize && widthChanged)
		_vertexArrayDirty.store(true, std::memory_order_release);

	if (nullptr == _resourceLib)
		return;

	if (_ResolveFont(*_resourceLib))
		_vertexArrayDirty.store(true, std::memory_order_release);
}

void GuiLabel::SetString(const std::string& str)
{
	std::lock_guard<std::mutex> lock(_stringMutex);
	if (_pendingStr == str)
		return;

	_pendingStr = str;
	_vertexArrayDirty.store(true, std::memory_order_release);
}

std::string GuiLabel::FitText(const std::string& text, float width,
	const std::function<float(const std::string&)>& measure)
{
	if (width <= 0.0f) return {};
	if (measure(text) <= width) return text;
	std::string suffix = "...";
	while (!suffix.empty() && measure(suffix) > width) suffix.pop_back();
	size_t first = 0, last = text.size();
	while (first < last)
	{
		const auto length = first + (last - first + 1) / 2;
		if (measure(text.substr(0, length) + suffix) <= width) first = length;
		else last = length - 1;
	}
	return text.substr(0, first) + suffix;
}

std::optional<float> GuiLabel::MeasureText(const std::string& text) const
{
	if (auto font = _font.lock()) return font->MeasureString(text);
	return std::nullopt;
}

utils::Size2d GuiLabel::ContentSize() const
{
	auto font = _font.lock();
	if (!font)
		return GetSize();

	// Use _pendingStr so that ContentSize() reflects the most recent SetString()
	// call even before Draw() has synced the vertex array.
	std::string currentStr;
	{
		std::lock_guard<std::mutex> lock(_stringMutex);
		currentStr = _pendingStr;
	}

	auto sz = GetSize();
	float measuredW = font->MeasureString(currentStr);

	return {
		static_cast<unsigned int>(std::ceil(measuredW)),
		sz.Height
	};
}

void GuiLabel::Draw(DrawContext& ctx)
{
	if (!IsVisible() || GetSize().Width == 0u || GetSize().Height == 0u)
		return;
	auto font = _font.lock();

	if (!font)
		return;

	SyncVertexArray();
	if (_vertexArray == 0)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	const auto textWidth = _centerHorizontally ? font->MeasureString(_str) : 0.0f;
	const auto horizontalOffset = _centerHorizontally
		? std::max(0.0f, (static_cast<float>(GetSize().Width) - textWidth) / 2.0f)
		: 0.0f;
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X + _textInset.X + horizontalOffset, pos.Y + _textInset.Y, 0.f)));
	
	font->Draw(glCtx, _vertexArray, (unsigned int)_str.size());
	glCtx.PopMvp();
}

bool GuiLabel::_ResolveFont(ResourceLib& resourceLib)
{
	ResourceLib::FontSelection selection;
	const unsigned int desired = ResourceLib::ResolveTextPixelHeightFromControlBox(GetSize().Height, 0u);

	auto fontOpt = resourceLib.GetClosestFont(desired, &selection);
	if (!fontOpt.has_value())
	{
		std::cout << "GuiLabel::_ResolveFont failed for desired text height " << desired << std::endl;
		return false;
	}

	auto previousFont = _font.lock();
	const auto previousFontSize = _selectedFontSize;
	const auto previousResolvedPixelHeight = _resolvedTextPixelHeight;
	auto resolvedFont = fontOpt.value().lock();

	_font = fontOpt.value();
	_selectedFontSize = selection.Size;
	_resolvedTextPixelHeight = selection.PixelHeight;

	return (resolvedFont != previousFont)
		|| (previousFontSize != _selectedFontSize)
		|| (previousResolvedPixelHeight != _resolvedTextPixelHeight);
}

void GuiLabel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_resourceLib = &resourceLib;
	_ResolveFont(resourceLib);

	if (_font.expired())
	{
		std::cout << "GuiLabel::_InitResources missing resolved font resource" << std::endl;
		return;
	}

	_vertexArrayDirty.store(true, std::memory_order_release);

	auto validated = InitVertexArray();
	if (!validated)
		std::cout << "GuiLabel::_InitResources failed to build initial vertex array for label text: '" << _str << "'" << std::endl;

	utils::GlUtils::CheckError("GuiLabel::_InitResources()");
}

void GuiLabel::_ReleaseResources()
{
	_resourceLib = nullptr;
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
	if (_ellipsize)
		next = FitText(next, std::max(0.0f, static_cast<float>(GetSize().Width) - _textInset.X),
			[&font](const std::string& text) { return font->MeasureString(text); });

	graphics::GlDeleteQueue::DeleteBuffers(2, _vertexBuffers);
	_vertexBuffers[0] = 0;
	_vertexBuffers[1] = 0;
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = font->InitVertexArray(next, GL_STATIC_DRAW, &_vertexBuffers[0], &_vertexBuffers[1]);
	_str = next;
}
