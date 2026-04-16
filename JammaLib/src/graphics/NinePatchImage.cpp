#include "NinePatchImage.h"
#include <algorithm>
#include <gl/glew.h>
#include <gl/gl.h>
#include "gl/glext.h"
#include "gl/wglext.h"
#include "ImageUtils.h"
#include "glm/glm.hpp"

using namespace base;
using namespace graphics;
using namespace utils;
using namespace resources;

namespace
{
	constexpr int FloatsPerVertex = 3;
	constexpr int UvFloatsPerVertex = 2;
	constexpr int VertsPerCell = 6;
	constexpr int CellsPerAxis = 3;
	constexpr int NumChannels = 4;

	std::array<unsigned int, 4> BuildBounds(unsigned int border, unsigned int extent)
	{
		const auto left = (std::min)(border, extent);
		const auto right = extent > left ? (std::max)(left, extent - left) : left;
		return { 0u, left, right, extent };
	}
}

std::optional<BorderInfo> graphics::DetectBorder(
	const std::vector<unsigned char>& pixels,
	unsigned int width,
	unsigned int height)
{
	auto pixelsCopy = pixels;
	return DetectBorder(pixelsCopy, width, height);
}

std::optional<BorderInfo> graphics::DetectBorder(
	std::vector<unsigned char>& pixels,
	unsigned int width,
	unsigned int height)
{
	if ((width == 0) || (height == 0))
		return std::nullopt;

	const auto numPixels = width * height;
	if (pixels.size() < static_cast<size_t>(numPixels) * NumChannels)
		return std::nullopt;

	for (auto pixelIndex = 0u; pixelIndex < numPixels; ++pixelIndex)
	{
		const auto px = pixelIndex * NumChannels;
		const auto blue = pixels[px + 0];
		const auto green = pixels[px + 1];
		const auto red = pixels[px + 2];
		const auto alpha = pixels[px + 3];

		if ((blue == 255) && (green == 0) && (red == 255) && (alpha == 0))
		{
			// 1x1 textures have no adjacent pixel to copy from, so we keep the same pixel value.
			const auto replacementPixelIndex = pixelIndex + 1u < numPixels
				? pixelIndex + 1u
				: pixelIndex > 0u ? pixelIndex - 1u : pixelIndex;
			const auto replacementPx = replacementPixelIndex * NumChannels;
			for (auto i = 0; i < NumChannels; ++i)
				pixels[px + i] = pixels[replacementPx + i];

			return BorderInfo{
				pixelIndex % width,
				pixelIndex / width
			};
		}
	}

	return std::nullopt;
}

std::array<GLfloat, 162> graphics::BuildNinePatchPositions(
	unsigned int borderX,
	unsigned int borderY,
	Size2d size)
{
	const auto xBounds = BuildBounds(borderX, size.Width);
	const auto yBounds = BuildBounds(borderY, size.Height);
	std::array<GLfloat, 162> positions = {};
	auto outIndex = 0u;

	for (auto row = 0; row < CellsPerAxis; ++row)
	{
		for (auto col = 0; col < CellsPerAxis; ++col)
		{
			const auto x0 = static_cast<GLfloat>(xBounds[col]);
			const auto x1 = static_cast<GLfloat>(xBounds[col + 1]);
			const auto y0 = static_cast<GLfloat>(yBounds[row]);
			const auto y1 = static_cast<GLfloat>(yBounds[row + 1]);

			const GLfloat verts[VertsPerCell * FloatsPerVertex] = {
				x0, y0, 0.0f,
				x1, y0, 0.0f,
				x0, y1, 0.0f,
				x0, y1, 0.0f,
				x1, y0, 0.0f,
				x1, y1, 0.0f
			};

			for (auto v : verts)
				positions[outIndex++] = v;
		}
	}

	return positions;
}

NinePatchImage::NinePatchImage(NinePatchImageParams params) :
	Drawable(params),
	Sizeable(params),
	_vertexArray(0),
	_vertexBuffer{ 0,0 },
	_borderX(0),
	_borderY(0),
	_texWidth(0),
	_texHeight(0),
	_ninePatchParams(params),
	_texture(std::weak_ptr<TextureResource>()),
	_shader(std::weak_ptr<ShaderResource>())
{
}

void NinePatchImage::SetSize(Size2d size)
{
	Sizeable::SetSize(size);

	if (_vertexArray != 0)
	{
		auto pos = _BuildPositions(size);
		glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
		glBufferSubData(GL_ARRAY_BUFFER, 0, pos.size() * sizeof(GLfloat), pos.data());
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

void NinePatchImage::Draw(DrawContext& ctx)
{
	auto texture = _texture.lock();
	auto shader = _shader.lock();

	if (!texture || !shader)
		return;

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glBindTexture(GL_TEXTURE_2D, texture->GetId());

	glDrawArrays(GL_TRIANGLES, 0, VertexCount);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

void NinePatchImage::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
}

void NinePatchImage::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	const auto texturePath = std::string("./resources/textures/") + _drawParams.Texture + ".tga";
	auto imageLoaded = utils::ImageUtils::LoadTga(texturePath);
	if (!imageLoaded.has_value())
		validated = false;

	if (validated)
	{
		auto& [pixels, width, height] = imageLoaded.value();
		auto border = DetectBorder(pixels, width, height);
		if (!border.has_value())
		{
			validated = false;
		}
		else
		{
			_texWidth = width;
			_texHeight = height;
			_borderX = border->borderX;
			_borderY = border->borderY;
		}
	}

	if (validated)
		validated = _InitTexture(resourceLib);
	if (validated)
		validated = _InitShader(resourceLib);
	if (validated)
		validated = _InitVertexArray();

	_isDrawInitialised = validated;

	GlUtils::CheckError("NinePatchImage::_InitResources()");
}

void NinePatchImage::_ReleaseResources()
{
	glDeleteBuffers(2, _vertexBuffer);
	_vertexBuffer[0] = 0;
	_vertexBuffer[1] = 0;

	glDeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;

	_isDrawInitialised = false;
}

bool NinePatchImage::_InitTexture(ResourceLib& resourceLib)
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

	return true;
}

bool NinePatchImage::_InitShader(ResourceLib& resourceLib)
{
	auto shaderOpt = resourceLib.GetResource(_ninePatchParams.Shader);

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

std::array<GLfloat, 162> NinePatchImage::_BuildPositions(Size2d size) const
{
	return BuildNinePatchPositions(_borderX, _borderY, size);
}

bool NinePatchImage::_InitVertexArray()
{
	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(2, _vertexBuffer);

	const auto positions = _BuildPositions(_sizeParams.Size);
	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
	glBufferData(GL_ARRAY_BUFFER, positions.size() * sizeof(GLfloat), positions.data(), GL_DYNAMIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	const auto uBounds = BuildBounds(_borderX, _texWidth);
	const auto vBounds = BuildBounds(_borderY, _texHeight);
	const auto texWidth = _texWidth == 0 ? 1.0f : static_cast<GLfloat>(_texWidth);
	const auto texHeight = _texHeight == 0 ? 1.0f : static_cast<GLfloat>(_texHeight);

	std::array<GLfloat, VertexCount * UvFloatsPerVertex> uvs = {};
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

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("NinePatchImage::InitVertexArray");

	return true;
}
