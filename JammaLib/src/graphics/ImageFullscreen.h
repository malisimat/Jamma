#pragma once

#include <array>
#include <memory>
#include "Drawable.h"
#include "Sizeable.h"
#include "../resources/ResourceLib.h"
#include "GlUtils.h"

namespace graphics
{
	class ImageFullscreenParams : public base::DrawableParams
	{
	public:
		ImageFullscreenParams(base::DrawableParams drawParams,
			std::string shader) :
			base::DrawableParams(drawParams),
			Shader(shader)
		{}

	public:
		std::string Shader;
	};

	class ImageFullscreen :
		public base::Drawable
	{
	public:
		ImageFullscreen(ImageFullscreenParams params);
		~ImageFullscreen() { ReleaseResources(); }

		// Copy
		ImageFullscreen(const ImageFullscreen&) = delete;
		ImageFullscreen& operator=(const ImageFullscreen&) = delete;

		// Move
		ImageFullscreen(ImageFullscreen&& other) :
			base::Drawable(other._drawParams),
			_texture(std::move(other._texture)),
			_vertexArray(std::move(other._vertexArray)),
			_shaderName(std::move(other._shaderName)),
			_shader(std::move(other._shader))
		{
			other._texture = 0;
			other._vertexArray = 0;
			other._shader = std::weak_ptr<resources::ShaderResource>();
		}

		ImageFullscreen& operator=(ImageFullscreen&& other)
		{
			if (this != &other)
			{
				ReleaseResources();
				std::swap(_texture, other._texture);
				std::swap(_vertexArray, other._vertexArray);
				std::swap(_shaderName, other._shaderName),
				_shader.swap(other._shader);
			}

			return *this;
		}

	public:
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		void SetTexture(unsigned int texture);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		bool _InitShader(resources::ResourceLib& resourceLib);
		bool _InitVertexArray();

	protected:
		const int VertexCount = 3;

		unsigned int _texture;
		GLuint _vertexArray;
		std::string _shaderName;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
