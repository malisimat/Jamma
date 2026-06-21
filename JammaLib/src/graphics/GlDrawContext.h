#pragma once

#include <iostream>
#include <map>
#include <vector>
#include <any>
#include <optional>
#include <glm/glm.hpp>
#include "../utils/CommonTypes.h"
#include "../utils/VecUtils.h"
#include "DrawContext.h"
#include <gl/glew.h>
#include <gl/gl.h>
#include "glm/glm.hpp"
#include "glm/ext.hpp"

namespace graphics
{
	class GlDrawContext :
		public base::DrawContext
	{
	public:
		GlDrawContext(utils::Size2d size, ContextTarget target);
		~GlDrawContext();

	public:
		auto GetContextType() -> base::DrawContext::ContextType
		{
			return ContextType::OPENGL;
		}

		void Initialise() override;
		void Bind() override;
		unsigned int GetTexture() const;
		unsigned int GetPixel(utils::Position2d pos) override;
		const std::vector<unsigned char> GetPixels() const;

		std::optional<std::any> GetUniform(std::string name);
		void SetUniform(const std::string& name, std::any val);

		void PushMvp(const glm::mat4 mat) noexcept;
		void PopMvp() noexcept;
		void ClearMvp() noexcept;
		void PushScissorRect(utils::Position2d pos, utils::Size2d size) override;
		void PopScissorRect() override;
		utils::Position2d ProjectScreen(utils::Position3d pos);

	protected:
		static unsigned int _CreateFrameBuffer(utils::Size2d size, ContextTarget target);
		void _ApplyScissorState() noexcept;

		struct ScissorRect
		{
			int X;
			int Y;
			int Width;
			int Height;
		};

		static ScissorRect _IntersectScissorRect(const ScissorRect& a, const ScissorRect& b) noexcept;

	private:
		const std::string _MvpUniformName = "MVP";

		std::map<std::string, std::any> _uniforms;
		std::vector<glm::mat4> _mvp;
		std::vector<ScissorRect> _scissorStack;
		unsigned int _frameBuffer;
		unsigned int _texture;
	};
}
