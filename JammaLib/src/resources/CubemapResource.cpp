#include "CubemapResource.h"

using namespace resources;

CubemapResource::CubemapResource(std::string name, GLuint texture) :
	Resource(name),
	_texture(texture)
{
}

CubemapResource::~CubemapResource()
{
	Release();
}

void CubemapResource::Release()
{
	glDeleteTextures(1, &_texture);
	_texture = 0;
}

std::optional<GLuint> CubemapResource::Load(const std::string& baseName)
{
	// Face suffixes in GL cubemap order
	static const std::array<const char*, 6> faces = {
		"posx", "negx", "posy", "negy", "posz", "negz"
	};

	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_CUBE_MAP, texture);

	for (int i = 0; i < 6; ++i)
	{
		auto fileName = "./resources/textures/" + baseName + "_" + faces[i] + ".tga";
		auto imageLoaded = utils::ImageUtils::LoadTga(fileName);

		if (!imageLoaded.has_value())
		{
			glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
			glDeleteTextures(1, &texture);
			return std::nullopt;
		}

		auto [pixels, width, height] = imageLoaded.value();
		glTexImage2D(
			GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
			0, GL_RGBA8, width, height, 0,
			GL_BGRA, GL_UNSIGNED_BYTE,
			pixels.data());
	}

	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

	return texture;
}
