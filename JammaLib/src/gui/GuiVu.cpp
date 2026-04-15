#include "GuiVu.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace gui;
using namespace utils;
using base::DrawContext;
using graphics::GlDrawContext;
using resources::ResourceLib;
using resources::ShaderResource;

GuiVu::GuiVu() :
	ResourceUser(),
	_isVisible(true),
	_position({ 0, 0 }),
	_size({ VuWidth, 1 }),
	_value(audio::FallingValue({ 0.00005, 0.00003, 12000u })),
	_displayValue(0.0f),
	_displayHold(0.0f),
	_vertexArray(0),
	_vertexBuffer{ 0, 0 },
	_shader(std::weak_ptr<ShaderResource>())
{
}

GuiVu::~GuiVu()
{
	ReleaseResources();
}

void GuiVu::Draw(DrawContext& ctx)
{
	if (!_isVisible)
		return;

	auto shader = _shader.lock();
	if (!shader)
		return;

	if (_vertexArray == 0)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	// Snapshot atomic values written by the audio thread.
	auto displayValue = (double)_displayValue.load(std::memory_order_relaxed);
	auto displayHold  = (double)_displayHold.load(std::memory_order_relaxed);

	auto totalLeds = _CalcTotalLeds(_size.Height);
	auto numLeds = _CalcCurrentLeds(displayValue, totalLeds);
	auto holdLed = _CalcCurrentLeds(displayHold, totalLeds);
	if (holdLed > 0)
		holdLed--;

	// Position within slider-local coordinates, scale so 1 unit = LedPitch pixels tall.
	glCtx.PushMvp(glm::translate(glm::mat4(1.0),
		glm::vec3((float)_position.X, (float)_position.Y, 0.f)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0),
		glm::vec3((float)_size.Width, (float)LedPitch, 1.f)));

	// Guard: NumInstances - 1 must be > 0 to avoid a division-by-zero in the shader.
	auto shaderInstances = (totalLeds >= 2u) ? totalLeds : 2u;

	glCtx.SetUniform("DX", 0.0f);
	glCtx.SetUniform("DY", 1.0f);
	glCtx.SetUniform("NumInstances", shaderInstances);
	glCtx.SetUniform("InstanceOffset", 0u);

	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);

	// Draw current level bar.
	if (numLeds > 0)
		glDrawArraysInstanced(GL_TRIANGLES, 0, 6, numLeds);

	// Draw peak-hold LED only when the signal is not silent and the hold is above the bar.
	if (displayHold > 0.0 && holdLed >= numLeds && holdLed < totalLeds)
	{
		glCtx.SetUniform("InstanceOffset", holdLed);
		shader->SetUniforms(glCtx);
		glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
	}

	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void GuiVu::SetPeak(float peak, unsigned int numSamps)
{
	_value.SetTarget((double)peak);
	// Advance the falling value by one step per sample, matching the VU::SetValue convention.
	for (auto i = 0u; i < numSamps; i++)
		_value.Next();

	// Publish the latest display values to the render thread via atomics.
	_displayValue.store((float)_value.Current(), std::memory_order_relaxed);
	_displayHold.store((float)_value.HoldValue(), std::memory_order_relaxed);
}

void GuiVu::SetVisible(bool visible)
{
	_isVisible = visible;
}

bool GuiVu::IsVisible() const
{
	return _isVisible;
}

void GuiVu::SetPosition(utils::Position2d pos)
{
	_position = pos;
}

void GuiVu::SetSize(utils::Size2d size)
{
	_size = size;
}

void GuiVu::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	// Load shader.
	auto shaderOpt = resourceLib.GetResource("vu");
	if (shaderOpt.has_value())
	{
		auto resource = shaderOpt.value().lock();
		if (resource && resource->GetType() == resources::SHADER)
			_shader = std::dynamic_pointer_cast<ShaderResource>(resource);
	}

	if (_shader.expired())
		return;

	// Single LED quad: x in [0,1], y in [0, ledFrac] where ledFrac = LedHeight/LedPitch.
	const float ledFrac = (float)LedHeight / (float)LedPitch;

	const GLfloat verts[] = {
		0.0f, 0.0f,     0.0f,
		1.0f, 0.0f,     0.0f,
		0.0f, ledFrac,  0.0f,
		0.0f, ledFrac,  0.0f,
		1.0f, 0.0f,     0.0f,
		1.0f, ledFrac,  0.0f,
	};

	const GLfloat uvs[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,
		0.0f, 1.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
	};

	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(2, _vertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(uvs), uvs, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

void GuiVu::_ReleaseResources()
{
	if (_vertexBuffer[0] || _vertexBuffer[1])
	{
		glDeleteBuffers(2, _vertexBuffer);
		_vertexBuffer[0] = 0;
		_vertexBuffer[1] = 0;
	}

	if (_vertexArray)
	{
		glDeleteVertexArrays(1, &_vertexArray);
		_vertexArray = 0;
	}
}

unsigned int GuiVu::_CalcTotalLeds(unsigned int height)
{
	if (LedPitch <= 0 || height == 0 || (unsigned int)LedHeight > height)
		return 0u;

	// Count only LEDs whose full geometry fits within the requested height:
	// (totalLeds - 1) * LedPitch + LedHeight <= height
	return ((height - (unsigned int)LedHeight) / (unsigned int)LedPitch) + 1u;
}

unsigned int GuiVu::_CalcCurrentLeds(double value, unsigned int totalLeds)
{
	auto frac = value;
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;
	return (unsigned int)std::ceil(frac * (double)totalLeds);
}
