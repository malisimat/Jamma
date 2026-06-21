#include "GlDrawContext.h"
#include "GlDeleteQueue.h"
#include <algorithm>

using namespace graphics;
using namespace utils;

GlDrawContext::GlDrawContext(Size2d size,
	ContextTarget target) :
	DrawContext(size, target),
	_frameBuffer(0u),
	_texture(0u)
{
}

GlDrawContext::~GlDrawContext()
{
	if (_texture)
	{
		GlDeleteQueue::DeleteTextures(1, &_texture);
		_texture = 0u;
	}

	if (SCREEN != _target)
	{
		GlDeleteQueue::DeleteFramebuffers(1, &_frameBuffer);
		_frameBuffer = 0u;
	}
}

void GlDrawContext::Initialise()
{
	if (_texture)
	{
		GlDeleteQueue::DeleteTextures(1, &_texture);
		_texture = 0u;
	}

	if (_frameBuffer)
	{
		GlDeleteQueue::DeleteFramebuffers(1, &_frameBuffer);
		_frameBuffer = 0u;
	}

	_frameBuffer = _CreateFrameBuffer(_size, _target);

	if (SCREEN == _target)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);

	GLint texMode = PICKING == _target ? GL_RGB : GL_RGBA;

	glGenTextures(1, &_texture);
	glBindTexture(GL_TEXTURE_2D, _texture);
	glTexImage2D(GL_TEXTURE_2D, 0, texMode, _size.Width, _size.Height, 0, texMode, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		std::cerr << "Framebuffer is not complete: ";
		switch (status) {
		case GL_FRAMEBUFFER_UNDEFINED:
			std::cerr << "GL_FRAMEBUFFER_UNDEFINED";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER";
			break;
		case GL_FRAMEBUFFER_UNSUPPORTED:
			std::cerr << "GL_FRAMEBUFFER_UNSUPPORTED";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
			std::cerr << "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS";
			break;
		default:
			std::cerr << "Unknown error";
		}
		std::cerr << std::endl;
	}
	else {
		std::cout << "Framebuffer is complete!" << std::endl;
	}
}

void GlDrawContext::Bind()
{
	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);
	glViewport(0, 0, static_cast<GLsizei>(_size.Width), static_cast<GLsizei>(_size.Height));
	_scissorStack.clear();
	glDisable(GL_SCISSOR_TEST);
}

unsigned int GlDrawContext::GetTexture() const
{
	return _texture;
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
		glReadPixels(pos.X, pos.Y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixels);

		red = pixels[0];
		green = pixels[1];
		blue = pixels[2];
		alpha = pixels[3];

		objectId = utils::VecToId({ red, green, blue, alpha });
	}

	return objectId;
}

const std::vector<unsigned char> GlDrawContext::GetPixels() const
{
	unsigned int objectId = 0;
	std::vector<unsigned char> pixels(4 * _size.Width * _size.Height);

	glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);

	glReadPixels(0, 0, _size.Width, _size.Height, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[0]);

	return pixels;
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

void GlDrawContext::PushScissorRect(utils::Position2d pos, utils::Size2d size)
{
	ScissorRect rect{
		pos.X,
		pos.Y,
		(int)size.Width,
		(int)size.Height
	};

	if (!_scissorStack.empty())
		rect = _IntersectScissorRect(_scissorStack.back(), rect);

	_scissorStack.push_back(rect);
	_ApplyScissorState();
}

void GlDrawContext::PopScissorRect()
{
	if (!_scissorStack.empty())
		_scissorStack.pop_back();

	_ApplyScissorState();
}

void GlDrawContext::_ApplyScissorState() noexcept
{
	if (_scissorStack.empty())
	{
		glDisable(GL_SCISSOR_TEST);
		return;
	}

	auto rect = _scissorStack.back();
	rect = _IntersectScissorRect(rect, { 0, 0, (int)_size.Width, (int)_size.Height });

	glEnable(GL_SCISSOR_TEST);
	glScissor(rect.X, rect.Y, std::max(0, rect.Width), std::max(0, rect.Height));
}

GlDrawContext::ScissorRect GlDrawContext::_IntersectScissorRect(const ScissorRect& a, const ScissorRect& b) noexcept
{
	const int x1 = std::max(a.X, b.X);
	const int y1 = std::max(a.Y, b.Y);
	const int x2 = std::min(a.X + a.Width, b.X + b.Width);
	const int y2 = std::min(a.Y + a.Height, b.Y + b.Height);

	return {
		x1,
		y1,
		std::max(0, x2 - x1),
		std::max(0, y2 - y1)
	};
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
	/*glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	unsigned int rb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_FRAMEBUFFER, GL_DEPTH_COMPONENT16, size.Width, size.Height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);*/

	return fbo;
}