#include "GuiLabel.h"
#include "../graphics/GlDeleteQueue.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

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
	_wrapWidth(guiParams.WrapWidth != 0 ? guiParams.WrapWidth : static_cast<int>(guiParams.Size.Width)),
	_fontSize(guiParams.FontSize),
	_verticalAlign(guiParams.VerticalAlign),
	_clipText(guiParams.ClipText),
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
	const bool layoutChanged = GetSize().Width != size.Width || GetSize().Height != size.Height;
	GuiElement::SetSize(size);
	if ((_ellipsize || _wrapWidth > 0) && layoutChanged)
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

std::vector<std::string> GuiLabel::WrapText(const std::string& text, float width,
	unsigned int maxLines, const std::function<float(const std::string&)>& measure)
{
	if (text.empty() || maxLines == 0u) return {};
	if (width <= 0.0f) return { text };

	std::vector<std::string> lines;
	size_t offset = 0u;
	while (offset < text.size() && lines.size() < maxLines)
	{
		while (offset < text.size() && text[offset] == ' ') ++offset;
		if (offset == text.size()) break;

		std::string line;
		size_t lastSpace = std::string::npos;
		while (offset < text.size())
		{
			const char ch = text[offset];
			if (measure(line + ch) <= width || line.empty())
			{
				line.push_back(ch);
				++offset;
				if (ch == ' ') lastSpace = line.size() - 1u;
				continue;
			}

			if (lastSpace != std::string::npos)
			{
				const auto carried = line.size() - lastSpace - 1u;
				line.resize(lastSpace);
				offset -= carried;
			}
			break;
		}

		if (line.empty()) break;
		lines.push_back(std::move(line));
	}

	if (offset < text.size() && !lines.empty())
		lines.back() = FitText(lines.back() + "...", width, measure);
	return lines;
}

std::optional<float> GuiLabel::MeasureText(const std::string& text) const
{
	if (auto font = _font.lock()) return font->MeasureString(text);
	return std::nullopt;
}

utils::Size2d GuiLabel::GetSize() const
{
	auto size = GuiElement::GetSize();
	if (size.Width != 0u || _wrapWidth > 0) return size;
	if (auto font = _font.lock())
	{
		std::string currentStr;
		{
			std::lock_guard<std::mutex> lock(_stringMutex);
			currentStr = _pendingStr;
		}
		size.Width = static_cast<unsigned int>(std::ceil(font->MeasureString(currentStr)));
	}
	return size;
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
	if (_wrapWidth > 0 && _LineCapacity(*font) > 1u)
	{
		const auto width = std::min(static_cast<float>(sz.Width), static_cast<float>(_wrapWidth));
		const auto lines = WrapText(currentStr, width, _LineCapacity(*font),
			[&font](const std::string& text) { return font->MeasureString(text); });
		measuredW = 0.0f;
		for (const auto& line : lines) measuredW = std::max(measuredW, font->MeasureString(line));
	}

	return {
		static_cast<unsigned int>(std::ceil(measuredW)),
		sz.Height
	};
}

GuiTextLineFrame GuiLabel::ResolveLineFrame(float height, Font::VerticalMetrics metrics, GuiTextVerticalAlign alignment)
{
	float baseline = 0.0f;
	if (alignment == GuiTextVerticalAlign::Bottom) baseline = -metrics.Descent;
	else if (alignment == GuiTextVerticalAlign::Center) baseline = (height - metrics.Height()) * 0.5f - metrics.Descent;
	else if (alignment == GuiTextVerticalAlign::Top) baseline = height - metrics.Ascent;
	return { baseline, baseline + metrics.Descent, baseline + metrics.Ascent };
}

std::optional<GuiTextLineFrame> GuiLabel::LineFrame() const
{
	if (auto font = _font.lock())
	{
		auto line = ResolveLineFrame(static_cast<float>(GetSize().Height), font->Metrics(), _verticalAlign);
		line.BaselineY += _textInset.Y;
		line.Bottom += _textInset.Y;
		line.Top += _textInset.Y;
		return line;
	}
	return std::nullopt;
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

	if (!_guiParams.Texture.empty()) GuiElement::Draw(ctx);
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	const auto textWidth = _centerHorizontally ? font->MeasureString(_str) : 0.0f;
	const auto horizontalOffset = _centerHorizontally
		? std::max(0.0f, (static_cast<float>(GetSize().Width) - textWidth) / 2.0f)
		: 0.0f;
	if (_clipText) glCtx.PushScissorRect(GlobalPosition(), GetSize());
	auto line = LineFrame().value();
	const auto lineCount = static_cast<unsigned int>(std::count(_str.begin(), _str.end(), '\n')) + 1u;
	if (lineCount > 1u)
	{
		const float extra = static_cast<float>(lineCount - 1u) * font->Metrics().LineStep();
		if (_verticalAlign == GuiTextVerticalAlign::Center) line.BaselineY += extra * 0.5f;
		else if (_verticalAlign == GuiTextVerticalAlign::Bottom) line.BaselineY += extra;
	}
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X + _textInset.X + horizontalOffset, pos.Y + line.BaselineY, 0.f)));
	
	font->Draw(glCtx, _vertexArray, (unsigned int)_str.size());
	glCtx.PopMvp();
	if (_clipText) glCtx.PopScissorRect();
}

bool GuiLabel::_ResolveFont(ResourceLib& resourceLib)
{
	if (_fontSize)
	{
		auto fontOpt = resourceLib.GetFont(*_fontSize);
		if (!fontOpt.has_value()) return false;
		auto previousFont = _font.lock();
		_font = fontOpt.value();
		_selectedFontSize = *_fontSize;
		_resolvedTextPixelHeight = Font::GetPixelHeightForSize(*_fontSize);
		return fontOpt.value().lock() != previousFont;
	}
	ResourceLib::FontSelection selection;
	const unsigned int desired = ResourceLib::ResolveTextPixelHeightFromControlBox(GetSize().Height, 0u);

	auto fontOpt = resourceLib.GetClosestFont(desired, &selection);
	if (!fontOpt.has_value())
	{
		std::cout << "[LOAD] GuiLabel font selection failed for desired text height " << desired << std::endl;
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
	if (!_guiParams.Texture.empty()) GuiElement::_InitResources(resourceLib, forceInit);
	_resourceLib = &resourceLib;
	_ResolveFont(resourceLib);

	if (_font.expired())
	{
		std::cout << "[LOAD] GuiLabel missing resolved font resource" << std::endl;
		return;
	}

	_vertexArrayDirty.store(true, std::memory_order_release);

	auto validated = InitVertexArray();
	if (!validated)
		std::cout << "[LOAD] GuiLabel failed to build initial vertex array for label text: '" << _str << "'" << std::endl;

	utils::GlUtils::CheckError("GuiLabel::_InitResources()");
}

void GuiLabel::_ReleaseResources()
{
	if (!_guiParams.Texture.empty()) GuiElement::_ReleaseResources();
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
	if (_wrapWidth > 0 && _LineCapacity(*font) > 1u)
	{
		const auto width = std::min(static_cast<float>(GetSize().Width), static_cast<float>(_wrapWidth));
		const auto lines = WrapText(next, width, _LineCapacity(*font),
			[&font](const std::string& text) { return font->MeasureString(text); });
		next.clear();
		for (const auto& line : lines)
		{
			if (!next.empty()) next.push_back('\n');
			next += line;
		}
	}
	else if (_ellipsize)
		next = FitText(next, std::max(0.0f, static_cast<float>(GetSize().Width) - _textInset.X),
			[&font](const std::string& text) { return font->MeasureString(text); });

	graphics::GlDeleteQueue::DeleteBuffers(2, _vertexBuffers);
	_vertexBuffers[0] = 0;
	_vertexBuffers[1] = 0;
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = font->InitVertexArray(next, GL_STATIC_DRAW, &_vertexBuffers[0], &_vertexBuffers[1]);
	_str = next;
}

unsigned int GuiLabel::_LineCapacity(const Font& font) const
{
	const auto metrics = font.Metrics();
	const float lineStep = std::max(1.0f, metrics.LineStep());
	const float height = static_cast<float>(GetSize().Height);
	if (height <= metrics.Height()) return 1u;
	return 1u + static_cast<unsigned int>(std::floor((height - metrics.Height()) / lineStep));
}
