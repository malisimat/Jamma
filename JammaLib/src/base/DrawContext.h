#pragma once

#include <string>
#include <algorithm>
#include <cmath>
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

		// UI-thread draw state. Nested scopes multiply, then restore exactly;
		// no heap-backed stack or GL state mutation is needed to enter a scope.
		class ScopedOpacity
		{
		public:
			ScopedOpacity(DrawContext& context, float local) noexcept :
				_context(context), _previous(context._opacity)
			{
				context._opacity *= std::isfinite(local) ? (std::clamp)(local, 0.0f, 1.0f) : 1.0f;
			}
			~ScopedOpacity() { _context._opacity = _previous; }
			ScopedOpacity(const ScopedOpacity&) = delete;
			ScopedOpacity& operator=(const ScopedOpacity&) = delete;
		private:
			DrawContext& _context;
			float _previous;
		};
		[[nodiscard]] ScopedOpacity WithOpacity(float local) noexcept { return ScopedOpacity(*this, local); }
		float Opacity() const noexcept { return _opacity; }

		virtual void Initialise() { }

		virtual void Bind() { }

		virtual unsigned int GetPixel(utils::Position2d pos)
		{
			return 0;
		}

		virtual void PushScissorRect(utils::Position2d pos, utils::Size2d size)
		{
		}

		virtual void PopScissorRect()
		{
		}

	protected:
		float _opacity = 1.0f;
		utils::Size2d _size;
		ContextTarget _target;
	};
}
