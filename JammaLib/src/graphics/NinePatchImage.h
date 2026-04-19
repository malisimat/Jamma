#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <gl/glew.h>
#include "Drawable.h"
#include "Sizeable.h"
#include "../resources/ResourceLib.h"
#include "GlUtils.h"

namespace graphics
{
	class NinePatchImageParams :
		public base::DrawableParams,
		public base::SizeableParams
	{
	public:
		NinePatchImageParams(base::DrawableParams drawParams,
			base::SizeableParams sizeParams,
			std::string shader) :
			base::DrawableParams(drawParams),
			base::SizeableParams(sizeParams),
			Shader(shader)
		{}

	public:
		std::string Shader;
	};

	class NinePatchImage :
		public base::Drawable,
		public base::Sizeable
	{
	public:
		struct BorderInfo
		{
			unsigned int borderX;
			unsigned int borderY;
		};

		static std::optional<BorderInfo> DetectBorder(
			std::vector<unsigned char>& pixels,
			unsigned int width,
			unsigned int height);

		static std::array<GLfloat, 162> BuildPositions(
			unsigned int borderX,
			unsigned int borderY,
			utils::Size2d size);

		NinePatchImage(NinePatchImageParams params);
		~NinePatchImage() { ReleaseResources(); }

		// Copy
		NinePatchImage(const NinePatchImage&) = delete;
		NinePatchImage& operator=(const NinePatchImage&) = delete;

		// Move
		NinePatchImage(NinePatchImage&& other) :
			base::Drawable(other._drawParams),
			base::Sizeable(other._sizeParams),
			_vertexArray(other._vertexArray),
			_vertexBuffer{ other._vertexBuffer[0], other._vertexBuffer[1] },
			_borderX(other._borderX),
			_borderY(other._borderY),
			_texWidth(other._texWidth),
			_texHeight(other._texHeight),
			_ninePatchParams(std::move(other._ninePatchParams)),
			_texture(std::move(other._texture)),
			_shader(std::move(other._shader))
		{
			other._vertexArray = 0;
			other._vertexBuffer[0] = 0;
			other._vertexBuffer[1] = 0;
			other._borderX = 0;
			other._borderY = 0;
			other._texWidth = 0;
			other._texHeight = 0;
			other._texture = std::weak_ptr<resources::TextureResource>();
			other._shader = std::weak_ptr<resources::ShaderResource>();
		}

		NinePatchImage& operator=(NinePatchImage&& other)
		{
			if (this != &other)
			{
				ReleaseResources();
				std::swap(_vertexArray, other._vertexArray);
				std::swap(_vertexBuffer, other._vertexBuffer);
				std::swap(_borderX, other._borderX);
				std::swap(_borderY, other._borderY);
				std::swap(_texWidth, other._texWidth);
				std::swap(_texHeight, other._texHeight);
				std::swap(_ninePatchParams, other._ninePatchParams);
				_texture.swap(other._texture);
				_shader.swap(other._shader);
			}

			return *this;
		}

	public:
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		bool _InitTexture(resources::ResourceLib& resourceLib);
		bool _InitShader(resources::ResourceLib& resourceLib);
		bool _InitVertexArray();

		std::array<GLfloat, 162> _BuildPositions(utils::Size2d size) const;

	protected:
		static constexpr int VertexCount = 54;

		GLuint _vertexArray;
		GLuint _vertexBuffer[2];
		unsigned int _borderX;
		unsigned int _borderY;
		unsigned int _texWidth;
		unsigned int _texHeight;
		NinePatchImageParams _ninePatchParams;
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
