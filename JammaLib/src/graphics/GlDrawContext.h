#pragma once

#include <iostream>
#include <map>
#include <vector>
#include <any>
#include <optional>
#include <glm/glm.hpp>
#include "../utils/CommonTypes.h"
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

		void SetSize(utils::Size2d size) override;
		bool Bind() override;
		unsigned int GetPixel(utils::Position2d pos) override;

		std::optional<std::any> GetUniform(std::string name);
		void SetUniform(const std::string& name, std::any val);

		void PushMvp(const glm::mat4 mat) noexcept;
		void PopMvp() noexcept;
		void ClearMvp() noexcept;
		utils::Position2d ProjectScreen(utils::Position3d pos);

	protected:
		static unsigned int _CreateFrameBuffer(utils::Size2d size, ContextTarget target);

	private:
		const std::string _MvpUniformName = "MVP";

		std::map<std::string, std::any> _uniforms;
		std::vector<glm::mat4> _mvp;
		unsigned int _frameBuffer;
	};
}
