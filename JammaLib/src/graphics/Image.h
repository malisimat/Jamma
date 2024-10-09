#pragma once

#include <array>
#include <memory>
#include "Drawable.h"
#include "Sizeable.h"
#include "../resources/ResourceLib.h"
#include "GlUtils.h"

namespace graphics
{
	class ImageParams : public base::DrawableParams, public base::SizeableParams
	{
	public:
		ImageParams(base::DrawableParams drawParams,
			base::SizeableParams sizeParams,
			std::string shader) :
			base::DrawableParams(drawParams),
			base::SizeableParams(sizeParams),
			Shader(shader)
		{}

	public:
		std::string Shader;
	};

	class Image :
		public base::Drawable,
		public base::Sizeable
	{
	public:
		Image(ImageParams drawParams);
		~Image() { ReleaseResources(); }

		// Copy
		Image(const Image&) = delete;
		Image& operator=(const Image&) = delete;

		// Move
		Image(Image&& other) :
			base::Drawable(other._drawParams),
			base::Sizeable(other._sizeParams),
			_vertexArray(other._vertexArray),
			_vertexBuffer{ other._vertexBuffer[0], other._vertexBuffer[1] },
			_texture(std::move(other._texture)),
			_shader(std::move(other._shader))
		{
			other._vertexArray = 0;
			other._vertexBuffer[0] = 0;
			other._vertexBuffer[1] = 0;
			other._texture = std::weak_ptr<resources::TextureResource>();
			other._shader = std::weak_ptr<resources::ShaderResource>();
		}

		Image& operator=(Image&& other)
		{
			if (this != &other)
			{
				ReleaseResources();
				std::swap(_vertexArray, other._vertexArray);
				std::swap(_vertexBuffer, other._vertexBuffer);
				_texture.swap(other._texture);
				_shader.swap(other._shader);
			}

			return *this;
		}

	public:
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		bool InitTexture(resources::ResourceLib& resourceLib);
		bool InitShader(resources::ResourceLib& resourceLib);
		bool InitVertexArray();

	protected:
		const int VertexCount = 6;

		GLuint _vertexArray;
		GLuint _vertexBuffer[2];
		std::string _shaderName;
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
