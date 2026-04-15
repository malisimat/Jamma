#include "Skybox.h"
#include <gl/glew.h>
#include <gl/gl.h>

using namespace graphics;
using namespace resources;

// Unit cube, positions only, inward-facing (CCW winding for GL_BACK face culling off)
static const float SkyboxVerts[] = {
	// +X face
	 1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,
	 1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,
	// -X face
	-1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,
	-1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,
	// +Y face
	-1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,
	 1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,
	// -Y face
	-1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,
	 1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,
	// +Z face
	-1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
	// -Z face
	 1.0f, -1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
	-1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
};

static const int SkyboxVertCount = 36;

Skybox::Skybox() :
	_initialised(false),
	_vao(0),
	_vbo(0),
	_cubemap(),
	_shader()
{
}

Skybox::~Skybox()
{
	ReleaseResources();
}

void Skybox::InitResources(ResourceLib& resourceLib, bool forceInit)
{
	if (_initialised && !forceInit)
		return;

	if (forceInit)
		ReleaseResources();

	auto valid = _InitShader(resourceLib);
	if (valid)
		valid = _InitCubemap(resourceLib);
	if (valid)
		valid = _InitVertexArray();

	_initialised = valid;
}

void Skybox::Draw(GlDrawContext& ctx)
{
	auto shader = _shader.lock();
	auto cubemap = _cubemap.lock();

	if (!shader || !cubemap)
		return;

	glDepthMask(GL_FALSE);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);

	glUseProgram(shader->GetId());
	shader->SetUniforms(ctx);

	glBindVertexArray(_vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap->GetId());

	glDrawArrays(GL_TRIANGLES, 0, SkyboxVertCount);

	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	glEnable(GL_CULL_FACE);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
}

void Skybox::ReleaseResources()
{
	if (_vao)
	{
		glDeleteVertexArrays(1, &_vao);
		_vao = 0;
	}
	if (_vbo)
	{
		glDeleteBuffers(1, &_vbo);
		_vbo = 0;
	}
	_initialised = false;
}

bool Skybox::_InitShader(ResourceLib& resourceLib)
{
	auto resOpt = resourceLib.GetResource("skybox");
	if (!resOpt.has_value())
		return false;

	auto res = resOpt.value().lock();
	if (!res || res->GetType() != SHADER)
		return false;

	_shader = std::dynamic_pointer_cast<ShaderResource>(res);
	return true;
}

bool Skybox::_InitCubemap(ResourceLib& resourceLib)
{
	auto resOpt = resourceLib.GetResource("sky");
	if (!resOpt.has_value())
		return false;

	auto res = resOpt.value().lock();
	if (!res || res->GetType() != CUBEMAP)
		return false;

	_cubemap = std::dynamic_pointer_cast<CubemapResource>(res);
	return true;
}

bool Skybox::_InitVertexArray()
{
	glGenVertexArrays(1, &_vao);
	glBindVertexArray(_vao);

	glGenBuffers(1, &_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, _vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(SkyboxVerts), SkyboxVerts, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return true;
}
