#pragma once

#include <memory>
#include "DrawContext.h"
#include "ResourceUser.h"
#include "../resources/ResourceLib.h"
#include "../resources/TextureResource.h"

namespace base
{
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
			_drawParams(params),
			_texture(std::weak_ptr<resources::TextureResource>())
		{
		};

	public:
		virtual void Draw(DrawContext& ctx) {};

	protected:
		DrawableParams _drawParams;
		std::weak_ptr<resources::TextureResource> _texture;
	};
}