#pragma once

#include <string>
#include <iostream>
#include <optional>
#include <gl/glew.h>
#include <gl/gl.h>
#include "Resource.h"
#include "../utils/ImageUtils.h"

namespace resources
{
	class TextureResource : public Resource
	{
	public:
		TextureResource(std::string name,
			GLuint texture,
			unsigned int width,
			unsigned int height,
			bool isNinePatch,
			unsigned int borderX,
			unsigned int borderY);
		~TextureResource();

		// Delete the copy constructor/assignment
		TextureResource(const TextureResource&) = delete;
		TextureResource& operator=(const TextureResource&) = delete;

		TextureResource(TextureResource&& other) :
			Resource(other._name),
			_width(other._width),
			_height(other._height),
			_texture(other._texture),
			_isNinePatch(other._isNinePatch),
			_borderX(other._borderX),
			_borderY(other._borderY)
		{
			std::cout << "Moving TextureResource" << std::endl;

			other._name = "";
			other._width = 0;
			other._height = 0;
			other._texture = 0;
			other._isNinePatch = false;
			other._borderX = 0;
			other._borderY = 0;
		}

		TextureResource& operator=(TextureResource&& other)
		{
			if (this != &other)
			{
				std::cout << "Swapping TextureResource" << std::endl;

				Release();
				std::swap(_name, other._name);
				std::swap(_width, other._width);
				std::swap(_height, other._height);
				std::swap(_texture, other._texture);
				std::swap(_isNinePatch, other._isNinePatch);
				std::swap(_borderX, other._borderX);
				std::swap(_borderY, other._borderY);
			}

			return *this;
		}

		virtual Type GetType() const override { return TEXTURE; }
		virtual GLuint GetId() const override { return _texture; }
		virtual void Release() override;

		unsigned int Width() const { return _width; }
		unsigned int Height() const { return _height; }
		bool IsNinePatch() const { return _isNinePatch; }
		unsigned int BorderX() const { return _borderX; }
		unsigned int BorderY() const { return _borderY; }

		static std::optional<std::tuple<GLuint, unsigned int, unsigned int>> Load(std::string fileName);

	private:
		unsigned int _width;
		unsigned int _height;
		GLuint _texture;
		bool _isNinePatch;
		unsigned int _borderX;
		unsigned int _borderY;
	};
}
