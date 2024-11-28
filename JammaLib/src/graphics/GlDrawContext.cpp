#include "GlDrawContext.h"

using namespace graphics;
using namespace utils;

GlDrawContext::GlDrawContext(Size2d size,
	ContextTarget target) :
	DrawContext(size, target)
{
}

GlDrawContext::~GlDrawContext()
{
	if (SCREEN != _target)
		glDeleteFramebuffers(1, &_frameBuffer);
}

void GlDrawContext::Initialise()
{
	_frameBuffer = _CreateFrameBuffer(_size, _target);

	if (SCREEN == _target)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);

	unsigned int texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, _size.Width, _size.Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
}

void GlDrawContext::Bind()
{
	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);
}

unsigned int GlDrawContext::GetPixel(utils::Position2d pos)
{
	if (SCREEN == _target)
		return 0;

	unsigned int objectId = 0;
	GLuint red, green, blue, alpha;
	GLubyte pixels[4];

	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);

	if (pos.X >= 0 && ((unsigned int)pos.X) < _size.Width &&
		pos.Y >= 0 && ((unsigned int)pos.Y) < _size.Height)
	{
		glReadPixels(pos.X, pos.Y, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, &pixels);

		red = pixels[2];
		green = pixels[1];
		blue = pixels[0];
		alpha = pixels[3];

		objectId = alpha + (red << 24) + (green << 16) + (blue << 8);
	}

	return objectId;
}

std::optional<std::any> GlDrawContext::GetUniform(std::string name)
{
	if (_MvpUniformName == name)
		return (name, _mvp);

	auto it = _uniforms.find(name);

	if (it != _uniforms.end())
		return it->second;

	return std::nullopt;
}

void GlDrawContext::SetUniform(const std::string& name, std::any val)
{
	_uniforms[name] = val;
}

void GlDrawContext::PushMvp(const glm::mat4 mat) noexcept
{
	_mvp.push_back(mat);
}

void GlDrawContext::PopMvp() noexcept
{
	if (!_mvp.empty())
		_mvp.pop_back();
}

void GlDrawContext::ClearMvp() noexcept
{
	_mvp.clear();
}

Position2d GlDrawContext::ProjectScreen(utils::Position3d pos)
{
	auto p = glm::vec3(pos.X, pos.Y, pos.Z);
	auto collapsed = glm::mat4(1.0);
	for (auto m : _mvp)
	{
		collapsed *= m;
	}

	auto screenPosHom = collapsed * glm::vec4{ pos.X, pos.Y, pos.Z, 1.0 };
	auto screenPosNorm = glm::vec3{ screenPosHom.x / screenPosHom.w,
		screenPosHom.y / screenPosHom.w,
		screenPosHom.z / screenPosHom.w };

	auto screenPosPix = Position2d{
		(int)((screenPosNorm.x + 1.0) * 0.5 * _size.Width),
		(int)((screenPosNorm.y + 1.0) * 0.5 * _size.Height) };

	return screenPosPix;
}

unsigned int GlDrawContext::_CreateFrameBuffer(Size2d size, ContextTarget target)
{
	if (SCREEN == target)
		return 0;

	unsigned int fbo;
	glGenFramebuffers(1, &fbo);

	return fbo;
}