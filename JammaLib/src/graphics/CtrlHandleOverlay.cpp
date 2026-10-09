#include "CtrlHandleOverlay.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <gl/glew.h>
#include "GlDrawContext.h"
#include "GlDeleteQueue.h"
#include "../gui/GuiLabel.h"

using namespace graphics;
using namespace resources;

CtrlHandleOverlay::CtrlHandleOverlay()
{
	for (int index = 0; index < NumButtons; ++index)
	{
		auto params = gui::GuiLabelParams::PanelHeader(index == 0 ? "SHFT" : "DIV", 72u);
		params.CenterHorizontally = true;
		_captions[index] = std::make_shared<gui::GuiLabel>(params);
	}
}

CtrlHandleOverlay::~CtrlHandleOverlay()
{
	ReleaseResources();
}

void CtrlHandleOverlay::SetAnchor(utils::Position2d screenPos, utils::Size2d sceneSize) noexcept
{
	_anchorPos = screenPos;
	_sceneSize = sceneSize;

	const auto visibleButtons = (std::max)(_visibleButtonCount, 1);
	const auto totalW = visibleButtons * ButtonW + (visibleButtons - 1) * ButtonGap;
	float panelX = static_cast<float>(_anchorPos.X) - totalW * 0.5f;
	float panelY = static_cast<float>(_anchorPos.Y) - AnchorOffsetY - ButtonH;

	const auto maxX = static_cast<float>(_sceneSize.Width) - totalW;
	const auto maxY = static_cast<float>(_sceneSize.Height) - ButtonH;
	_panelX = std::clamp(panelX, 0.0f, std::max(maxX, 0.0f));
	_panelY = std::clamp(panelY, 0.0f, std::max(maxY, 0.0f));
}

void CtrlHandleOverlay::SetVisibleButtonCount(int count) noexcept
{
	_visibleButtonCount = std::clamp(count, 0, NumButtons);
	SetAnchor(_anchorPos, _sceneSize);
}

void CtrlHandleOverlay::SetButtonScope(int index, ButtonScope scope) noexcept
{
	if ((index < 0) || (index >= NumButtons))
		return;

	_buttonScopes[static_cast<size_t>(index)] = scope;
}

int CtrlHandleOverlay::HitTestButton(utils::Position2d pos) const noexcept
{
	const float px = static_cast<float>(pos.X);
	const float py = static_cast<float>(pos.Y);

	if ((py < _panelY) || (py > (_panelY + ButtonH)))
		return -1;

	for (int i = 0; i < _visibleButtonCount; ++i)
	{
		const float x = _panelX + static_cast<float>(i) * (ButtonW + ButtonGap);
		if ((px >= x) && (px <= (x + ButtonW)))
			return i;
	}

	return -1;
}

std::optional<utils::Position2d> CtrlHandleOverlay::ButtonCenter(int index) const noexcept
{
	if (index < 0 || index >= _visibleButtonCount)
		return std::nullopt;

	const auto x = _panelX + static_cast<float>(index) * (ButtonW + ButtonGap) + (ButtonW * 0.5f);
	const auto y = _panelY + (ButtonH * 0.5f);

	return utils::Position2d{
		static_cast<int>(std::lround(x)),
		static_cast<int>(std::lround(y))
	};
}

void CtrlHandleOverlay::SetAlpha(float alpha) noexcept
{
	_alpha = std::clamp(alpha, 0.0f, 1.0f);
}

void CtrlHandleOverlay::InitResources(ResourceLib& resourceLib, bool forceInit)
{
	if (_vertexArray != 0 && !forceInit)
		return;

	auto shaderOpt = resourceLib.GetResource("ctrl_handle");
	if (!shaderOpt.has_value())
	{
		std::cout << "CtrlHandleOverlay: missing shader resource 'ctrl_handle'" << std::endl;
		return;
	}

	auto res = shaderOpt.value().lock();
	if (!res || res->GetType() != SHADER)
	{
		std::cout << "CtrlHandleOverlay: shader resource type mismatch" << std::endl;
		return;
	}

	_shader = std::dynamic_pointer_cast<ShaderResource>(res);

	for (auto& caption : _captions) caption->InitResources(resourceLib, forceInit);

	// Allocate a VBO large enough to hold all button quads (2 triangles each).
	constexpr int FloatsPerButton = 6 * 2;  // 6 vertices × 2 floats (x, y)
	constexpr int TotalFloats = (NumButtons + 1) * FloatsPerButton;

	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(1, &_vertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * TotalFloats, nullptr, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void CtrlHandleOverlay::ReleaseResources()
{
	if (_vertexBuffer != 0)
	{
		GlDeleteQueue::DeleteBuffers(1, &_vertexBuffer);
		_vertexBuffer = 0;
	}
	if (_vertexArray != 0)
	{
		GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
		_vertexArray = 0;
	}
	for (auto& caption : _captions) caption->ReleaseResources();
	_shader.reset();
}

void CtrlHandleOverlay::Draw(base::DrawContext& ctx)
{
	if (_alpha < 0.001f || _vertexArray == 0 || _visibleButtonCount == 0)
		return;

	auto shaderPtr = _shader.lock();
	if (!shaderPtr)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	// Compute panel top-left so the buttons appear just above the cursor.
	const float panelX = _panelX;
	const float panelY = _panelY;

	// Build all button quads into a local array and upload in one shot.
	constexpr int FloatsPerButton = 6 * 2;
	float verts[(NumButtons + 1) * FloatsPerButton]{};

	for (int i = 0; i < _visibleButtonCount; ++i)
	{
		const float x  = panelX + static_cast<float>(i) * (ButtonW + ButtonGap);
		const float y  = panelY;
		const float x2 = x + ButtonW;
		const float y2 = y + ButtonH;

		float* q = &verts[i * FloatsPerButton];
		// Triangle 1
		q[0]  = x;  q[1]  = y;
		q[2]  = x2; q[3]  = y;
		q[4]  = x2; q[5]  = y2;
		// Triangle 2
		q[6]  = x;  q[7]  = y;
		q[8]  = x2; q[9]  = y2;
		q[10] = x;  q[11] = y2;
	}

	const float panelRight = panelX + _visibleButtonCount * ButtonW + (_visibleButtonCount - 1) * ButtonGap;
	float* panel = &verts[NumButtons * FloatsPerButton];
	const float left = panelX - 6.0f, right = panelRight + 6.0f;
	const float bottom = panelY - 6.0f, top = panelY + ButtonH + 26.0f;
	const float panelVerts[] = {left,bottom,right,bottom,right,top,left,bottom,right,top,left,top};
	std::copy(std::begin(panelVerts), std::end(panelVerts), panel);

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glUseProgram(shaderPtr->GetId());
	shaderPtr->SetUniforms(glCtx);   // pushes MVP

	glBindVertexArray(_vertexArray);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	const auto colorLoc = glGetUniformLocation(shaderPtr->GetId(), "Color");
	const auto boundsLoc = glGetUniformLocation(shaderPtr->GetId(), "Bounds");
	const auto panelLoc = glGetUniformLocation(shaderPtr->GetId(), "Panel");
	if (_activeButton < 0)
	{
		glUniform4f(boundsLoc, left, bottom, right - left, top - bottom);
		glUniform1i(panelLoc, 1);
		glUniform4f(colorLoc, 0.23f, 0.23f, 0.23f, _alpha * 0.72f);
		glDrawArrays(GL_TRIANGLES, NumButtons * 6, 6);
	}
	glUniform1i(panelLoc, 0);

	for (int i = 0; i < _visibleButtonCount; ++i)
	{
		if (_activeButton >= 0 && i != _activeButton) continue;
		glUniform4f(boundsLoc, panelX + i * (ButtonW + ButtonGap), panelY, ButtonW, ButtonH);
		const auto& spec = ButtonSpecs[static_cast<size_t>(i)];
		const auto scope = _buttonScopes[static_cast<size_t>(i)];
		const auto hue = (scope == ButtonScope::Global)
			? spec.globalHue
			: spec.localHue;
		const auto col = HsvToRgb(hue, 0.60f, 0.90f);
		glUniform4f(colorLoc, col.x, col.y, col.z, _alpha * 0.88f);
		glDrawArrays(GL_TRIANGLES, i * 6, 6);
	}

	glBindVertexArray(0);
	glUseProgram(0);
	auto opacity = ctx.WithOpacity(_alpha);
	for (int i = 0; i < _visibleButtonCount; ++i)
	{
		if (_activeButton >= 0 && i != _activeButton) continue;
		_captions[i]->SetPosition({static_cast<int>(panelX + i * (ButtonW + ButtonGap)), static_cast<int>(panelY + ButtonH + 2.0f)});
		_captions[i]->Draw(ctx);
	}
}

glm::vec3 CtrlHandleOverlay::HsvToRgb(float h, float s, float v) noexcept
{
	h = h - std::floor(h);
	const float c = v * s;
	const float x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
	const float m = v - c;

	glm::vec3 rgb;
	if      (h < 1.0f / 6.0f) rgb = { c, x, 0.0f };
	else if (h < 2.0f / 6.0f) rgb = { x, c, 0.0f };
	else if (h < 3.0f / 6.0f) rgb = { 0.0f, c, x };
	else if (h < 4.0f / 6.0f) rgb = { 0.0f, x, c };
	else if (h < 5.0f / 6.0f) rgb = { x, 0.0f, c };
	else                       rgb = { c, 0.0f, x };

	return rgb + glm::vec3(m, m, m);
}
