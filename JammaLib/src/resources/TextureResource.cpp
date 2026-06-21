#include "TextureResource.h"
#include "../graphics/GlDeleteQueue.h"

using namespace resources;

TextureResource::TextureResource(std::string name,
	GLuint texture,
	unsigned int width,
	unsigned int height,
	bool isNinePatch,
	unsigned int borderX,
	unsigned int borderY) :
	Resource(name),
	_width(width),
	_height(height),
	_texture(texture),
	_isNinePatch(isNinePatch),
	_borderX(borderX),
	_borderY(borderY)
{
}

TextureResource::~TextureResource()
{
	Release();
}

void TextureResource::Release()
{
	graphics::GlDeleteQueue::DeleteTextures(1, &_texture);
	_texture = 0;
}

std::optional<std::tuple<GLuint,unsigned int,unsigned int>> TextureResource::Load(std::string fileName)
{
	auto imageLoaded = utils::ImageUtils::LoadTga(fileName);

	if (!imageLoaded.has_value())
		return std::nullopt;

	auto [pixels,width,height] = imageLoaded.value();

	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

	glBindTexture(GL_TEXTURE_2D, 0);

	return std::tuple<GLuint,unsigned int,unsigned int>(texture, width, height);
}
