#pragma once

#include <memory>
#include <gl/glew.h>
#include <gl/gl.h>
#include "../resources/ResourceLib.h"
#include "../resources/CubemapResource.h"
#include "../resources/ShaderResource.h"
#include "GlDrawContext.h"

namespace graphics
{
	class Skybox
	{
	public:
		Skybox();
		~Skybox();

		// Delete copy
		Skybox(const Skybox&) = delete;
		Skybox& operator=(const Skybox&) = delete;

	public:
		void InitResources(resources::ResourceLib& resourceLib, bool forceInit);
		void Draw(GlDrawContext& ctx);
		void ReleaseResources();

	private:
		bool _InitShader(resources::ResourceLib& resourceLib);
		bool _InitCubemap(resources::ResourceLib& resourceLib);
		bool _InitVertexArray();

	private:
		bool _initialised;
		GLuint _vao;
		GLuint _vbo;
		std::weak_ptr<resources::CubemapResource> _cubemap;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
