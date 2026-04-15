#pragma once

#include <array>
#include <optional>
#include <string>
#include <gl/glew.h>
#include <gl/gl.h>
#include "Resource.h"
#include "../utils/ImageUtils.h"

namespace resources
{
	class CubemapResource : public Resource
	{
	public:
		CubemapResource(std::string name, GLuint texture);
		~CubemapResource();

		// Delete copy
		CubemapResource(const CubemapResource&) = delete;
		CubemapResource& operator=(const CubemapResource&) = delete;

		// Move
		CubemapResource(CubemapResource&& other) :
			Resource(other._name),
			_texture(other._texture)
		{
			other._name = "";
			other._texture = 0;
		}

		CubemapResource& operator=(CubemapResource&& other)
		{
			if (this != &other)
			{
				Release();
				std::swap(_name, other._name);
				std::swap(_texture, other._texture);
			}

			return *this;
		}

		virtual Type GetType() const override { return CUBEMAP; }
		virtual GLuint GetId() const override { return _texture; }
		virtual void Release() override;

		// Loads 6 face TGA files: {baseName}_posx.tga, _negx, _posy, _negy, _posz, _negz
		// from ./resources/textures/
		static std::optional<GLuint> Load(const std::string& baseName);

	private:
		GLuint _texture;
	};
}
