#pragma once

#include <string>
#include "../utils/CommonTypes.h"

namespace base
{
	class DrawContext
	{
	public:
		enum ContextTarget { SCREEN, TEXTURE, PICKING };
		enum ContextType { DEFAULT, OPENGL };

	public:
		DrawContext(utils::Size2d size, ContextTarget target) :
			_size(size),
			_target(target) { }

		virtual auto GetContextType() -> ContextType
		{
			return DEFAULT;
		}

		ContextTarget GetContextTarget() const
		{
			return _target;
		}

		virtual void Initialise() { }

		virtual void Bind() { }

		virtual unsigned int GetPixel(utils::Position2d pos)
		{
			return 0;
		}

	protected:
		utils::Size2d _size;
		ContextTarget _target;
	};
}
