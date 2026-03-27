#pragma once

#include <memory>
#include "DrawContext.h"
#include "ResourceUser.h"
#include "../resources/ResourceLib.h"
#include "../resources/TextureResource.h"

namespace base
{
	enum DrawPass
	{
		PASS_SCENE,
		PASS_PICKER,
		PASS_HIGHLIGHT
	};

	class DrawableParams
	{
	public:
		std::string Texture;
	};

	class Drawable : public ResourceUser
	{
	public:
		Drawable(DrawableParams params) :
			ResourceUser(),
			_isDrawInitialised(false),
			_drawParams(params),
			_texture(std::weak_ptr<resources::TextureResource>())
		{
		};

	public:
		virtual void Draw(DrawContext& ctx) = 0;
		virtual void Draw3d(DrawContext& ctx, unsigned int numInstances, DrawPass pass) = 0;

		bool IsDrawInitialised() const { return _isDrawInitialised;	};

	protected:
		bool _isDrawInitialised;
		DrawableParams _drawParams;
		std::weak_ptr<resources::TextureResource> _texture;
	};
}