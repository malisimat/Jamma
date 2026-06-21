#include "Image.h"
#include "GlDeleteQueue.h"
#include <gl/glew.h>
#include <gl/gl.h>
#include "gl/glext.h"
#include "gl/wglext.h"
#include "glm/glm.hpp"

using namespace base;
using namespace graphics;
using namespace utils;
using namespace resources;

Image::Image(ImageParams params) :
	Drawable(params),
	Sizeable(params),
	_vertexArray(0),
	_vertexBuffer{ 0,0 },
	_imageParams(params),
	_vertexCount(VertexCountQuad),
	_isNinePatch(false),
	_borderX(0),
	_borderY(0),
	_texWidth(0),
	_texHeight(0),
	_vertexBufferDirty(false),
	_pendingWidth(params.Size.Width),
	_pendingHeight(params.Size.Height),
	_texture(std::weak_ptr<TextureResource>()),
	_shader(std::weak_ptr<ShaderResource>())
{
}

void Image::SetSize(Size2d size)
{
	Sizeable::SetSize(size);

	if (_isNinePatch)
	{
		_pendingWidth.store(size.Width, std::memory_order_release);
		_pendingHeight.store(size.Height, std::memory_order_release);
		_vertexBufferDirty.store(true, std::memory_order_release);
	}
}

void Image::Draw(DrawContext& ctx)
{
	auto texture = _texture.lock();
	auto shader = _shader.lock();

	if (!texture || !shader)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	if (_isNinePatch)
		_SyncVertexBuffer();
	else
		glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(_sizeParams.Size.Width, _sizeParams.Size.Height, 1.f)));

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture->GetId());

	glDrawArrays(GL_TRIANGLES, 0, _vertexCount);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	if (!_isNinePatch)
		glCtx.PopMvp();
}

void Image::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
}

void Image::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	if (validated)
		validated = _InitTexture(resourceLib);
	if (validated)
		validated = _InitShader(resourceLib);
	if (validated)
		validated = _InitVertexArray();

	_isDrawInitialised = validated;

	GlUtils::CheckError("Image::_InitResources()");
}

void Image::_ReleaseResources()
{
	GlDeleteQueue::DeleteBuffers(2, _vertexBuffer);
	_vertexBuffer[0] = 0;
	_vertexBuffer[1] = 0;

	GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;

	_isDrawInitialised = false;
}

bool Image::_InitTexture(ResourceLib& resourceLib)
{
	auto textureOpt = resourceLib.GetResource(_drawParams.Texture);
	
	if (!textureOpt.has_value())
		return false;

	auto resource = textureOpt.value().lock();

	if (!resource)
		return false;

	if (TEXTURE != resource->GetType())
		return false;
	
	_texture = std::dynamic_pointer_cast<TextureResource>(resource);

	auto texture = _texture.lock();
	if (!texture)
		return false;

	_isNinePatch = texture->IsNinePatch();
	_borderX = texture->BorderX();
	_borderY = texture->BorderY();
	_texWidth = texture->Width();
	_texHeight = texture->Height();

	if (_isNinePatch)
	{
		const auto borderWidth = static_cast<unsigned long long>(_borderX) * 2ull;
		const auto borderHeight = static_cast<unsigned long long>(_borderY) * 2ull;
		if ((borderWidth > _texWidth) || (borderHeight > _texHeight))
		{
			_borderX = 0;
			_borderY = 0;
		}

		_pendingWidth.store(_sizeParams.Size.Width, std::memory_order_release);
		_pendingHeight.store(_sizeParams.Size.Height, std::memory_order_release);
		_vertexBufferDirty.store(true, std::memory_order_release);
	}
	else
	{
		_vertexBufferDirty.store(false, std::memory_order_release);
	}

	return true;
}

bool Image::_InitShader(ResourceLib& resourceLib)
{
	auto shaderOpt = resourceLib.GetResource(_imageParams.Shader);

	if (!shaderOpt.has_value())
		return false;

	auto resource = shaderOpt.value().lock();

	if (!resource)
		return false;

	if (SHADER != resource->GetType())
		return false;

	_shader = std::dynamic_pointer_cast<ShaderResource>(resource);

	return true;
}

bool Image::_InitVertexArray()
{
	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(2, _vertexBuffer);

	if (_isNinePatch)
	{
		const Size2d size = {
			_pendingWidth.load(std::memory_order_acquire),
			_pendingHeight.load(std::memory_order_acquire)
		};
		const auto positions = NinePatchImage::BuildPositions(_borderX, _borderY, size);
		glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
		glBufferData(GL_ARRAY_BUFFER, positions.size() * sizeof(GLfloat), positions.data(), GL_DYNAMIC_DRAW);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(0);

		const auto uBounds = NinePatchImage::BuildBounds(_borderX, _texWidth);
		const auto vBounds = NinePatchImage::BuildBounds(_borderY, _texHeight);
		const auto texWidth = _texWidth == 0 ? 1.0f : static_cast<GLfloat>(_texWidth);
		const auto texHeight = _texHeight == 0 ? 1.0f : static_cast<GLfloat>(_texHeight);

		std::array<GLfloat, VertexCountNinePatch * UvFloatsPerVertex> uvs = {};
		auto uvIndex = 0u;
		for (auto row = 0; row < CellsPerAxis; ++row)
		{
			for (auto col = 0; col < CellsPerAxis; ++col)
			{
				const auto u0 = static_cast<GLfloat>(uBounds[col]) / texWidth;
				const auto u1 = static_cast<GLfloat>(uBounds[col + 1]) / texWidth;
				const auto v0 = static_cast<GLfloat>(vBounds[row]) / texHeight;
				const auto v1 = static_cast<GLfloat>(vBounds[row + 1]) / texHeight;

				const GLfloat cellUvs[VertsPerCell * UvFloatsPerVertex] = {
					u0, v0,
					u1, v0,
					u0, v1,
					u0, v1,
					u1, v0,
					u1, v1
				};

				for (auto uv : cellUvs)
					uvs[uvIndex++] = uv;
			}
		}

		glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[1]);
		glBufferData(GL_ARRAY_BUFFER, uvs.size() * sizeof(GLfloat), uvs.data(), GL_STATIC_DRAW);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(1);

		_vertexCount = VertexCountNinePatch;
		_vertexBufferDirty.store(false, std::memory_order_release);
	}
	else
	{
		GLfloat verts[] = {
			0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			0.0f,  1.0f, 0.0f,
			0.0f,  1.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			1.0f, 1.0f, 0.0f,
		};

		glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
		glBufferData(GL_ARRAY_BUFFER, 18 * sizeof(GLfloat), verts, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(0);

		float left = _imageParams.FlipH ? 1.0f : 0.0f;
		float right = _imageParams.FlipH ? 0.0f : 1.0f;
		float bottom = _imageParams.FlipV ? 1.0f : 0.0f;
		float top = _imageParams.FlipV ? 0.0f : 1.0f;

		const GLfloat uvs[] = {
			left, bottom,
			right, bottom,
			left, top,
			left, top,
			right, bottom,
			right, top
		};

		const GLfloat uvsRotated[] = {
			left, top,
			left, bottom,
			right, top,
			right, top,
			left, bottom,
			right, bottom
		};

		glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[1]);
		glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLfloat), _imageParams.Rot90 ? uvsRotated : uvs, GL_STATIC_DRAW);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(1);

		_vertexCount = VertexCountQuad;
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("Image::InitVertexArray");

	return true;
}

void Image::_SyncVertexBuffer()
{
	if (!_isNinePatch)
		return;

	if (!_vertexBufferDirty.exchange(false, std::memory_order_acq_rel))
		return;

	if (_vertexBuffer[0] == 0)
		return;

	const Size2d size = {
		_pendingWidth.load(std::memory_order_acquire),
		_pendingHeight.load(std::memory_order_acquire)
	};
	auto positions = NinePatchImage::BuildPositions(_borderX, _borderY, size);

	// Upload resized geometry only while the render thread owns the GL context.
	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
	glBufferSubData(GL_ARRAY_BUFFER, 0, positions.size() * sizeof(GLfloat), positions.data());
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}
