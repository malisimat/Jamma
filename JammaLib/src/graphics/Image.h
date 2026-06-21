#pragma once

#include <array>
#include <atomic>
#include <memory>
#include "Drawable.h"
#include "Sizeable.h"
#include "../resources/ResourceLib.h"
#include "GlUtils.h"
#include "NinePatchImage.h"

namespace graphics
{
	class ImageParams :
		public base::DrawableParams,
		public base::SizeableParams
	{
	public:
		ImageParams(base::DrawableParams drawParams,
			base::SizeableParams sizeParams,
			std::string shader,
			bool rot90, bool flipH, bool flipV) :
			base::DrawableParams(drawParams),
			base::SizeableParams(sizeParams),
			Rot90(rot90),
			FlipH(flipH),
			FlipV(flipV),
			Shader(shader)
		{}

	public:
		bool Rot90;
		bool FlipH;
		bool FlipV;
		std::string Shader;
	};

	class Image :
		public base::Drawable,
		public base::Sizeable
	{
	public:
		Image(ImageParams params);
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
			_imageParams(std::move(other._imageParams)),
			_vertexCount(other._vertexCount),
			_isNinePatch(other._isNinePatch),
			_borderX(other._borderX),
			_borderY(other._borderY),
			_texWidth(other._texWidth),
			_texHeight(other._texHeight),
			_vertexBufferDirty(other._vertexBufferDirty.load(std::memory_order_acquire)),
			_pendingWidth(other._pendingWidth.load(std::memory_order_acquire)),
			_pendingHeight(other._pendingHeight.load(std::memory_order_acquire)),
			_texture(std::move(other._texture)),
			_shader(std::move(other._shader))
		{
			other._vertexArray = 0;
			other._vertexBuffer[0] = 0;
			other._vertexBuffer[1] = 0;
			other._vertexCount = VertexCountQuad;
			other._isNinePatch = false;
			other._borderX = 0;
			other._borderY = 0;
			other._texWidth = 0;
			other._texHeight = 0;
			other._vertexBufferDirty.store(false, std::memory_order_release);
			other._pendingWidth.store(0, std::memory_order_release);
			other._pendingHeight.store(0, std::memory_order_release);
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
				std::swap(_imageParams, other._imageParams);
				std::swap(_vertexCount, other._vertexCount);
				std::swap(_isNinePatch, other._isNinePatch);
				std::swap(_borderX, other._borderX);
				std::swap(_borderY, other._borderY);
				std::swap(_texWidth, other._texWidth);
				std::swap(_texHeight, other._texHeight);

				auto dirty = _vertexBufferDirty.load(std::memory_order_acquire);
				_vertexBufferDirty.store(other._vertexBufferDirty.load(std::memory_order_acquire), std::memory_order_release);
				other._vertexBufferDirty.store(dirty, std::memory_order_release);

				auto pendingWidth = _pendingWidth.load(std::memory_order_acquire);
				_pendingWidth.store(other._pendingWidth.load(std::memory_order_acquire), std::memory_order_release);
				other._pendingWidth.store(pendingWidth, std::memory_order_release);

				auto pendingHeight = _pendingHeight.load(std::memory_order_acquire);
				_pendingHeight.store(other._pendingHeight.load(std::memory_order_acquire), std::memory_order_release);
				other._pendingHeight.store(pendingHeight, std::memory_order_release);

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
		void _SyncVertexBuffer();

	protected:
		static constexpr int VertexCountQuad = 6;
		static constexpr int VertexCountNinePatch = 54;
		static constexpr int UvFloatsPerVertex = 2;
		static constexpr int VertsPerCell = 6;
		static constexpr int CellsPerAxis = 3;

		GLuint _vertexArray;
		GLuint _vertexBuffer[2];
		ImageParams _imageParams;
		int _vertexCount;
		bool _isNinePatch;
		unsigned int _borderX;
		unsigned int _borderY;
		unsigned int _texWidth;
		unsigned int _texHeight;
		std::atomic<bool> _vertexBufferDirty;
		std::atomic<unsigned int> _pendingWidth;
		std::atomic<unsigned int> _pendingHeight;
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
