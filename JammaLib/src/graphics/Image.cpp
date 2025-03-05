#include "Image.h"
#include <gl/glew.h>
#include <gl/gl.h>
#include "gl/glext.h"
#include "gl/wglext.h"
#include "glm/glm.hpp"

using namespace base;
using namespace graphics;
using namespace utils;
using namespace resources;

Image::Image(ImageParams params) :
	Drawable(params),
	Sizeable(params),
	_vertexArray(0),
	_vertexBuffer{ 0,0 },
	_shaderName(params.Shader),
	_texture(std::weak_ptr<TextureResource>()),
	_shader(std::weak_ptr<ShaderResource>())
{
}

void Image::SetSize(Size2d size)
{
	Sizeable::SetSize(size);
}

void Image::Draw(DrawContext& ctx)
{
	auto texture = _texture.lock();
	auto shader = _shader.lock();

	if (!texture || !shader)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(_sizeParams.Size.Width, _sizeParams.Size.Height, 1.f)));

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glBindTexture(GL_TEXTURE_2D, texture->GetId());

	glDrawArrays(GL_TRIANGLES, 0, VertexCount);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
}

void Image::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
}

void Image::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	if (validated)
		validated = _InitTexture(resourceLib);
	if (validated)
		validated = _InitShader(resourceLib);
	if (validated)
		validated = _InitVertexArray();

	_isDrawInitialised = validated;

	GlUtils::CheckError("Image::_InitResources()");
}

void Image::_ReleaseResources()
{
	glDeleteBuffers(2, _vertexBuffer);
	_vertexBuffer[0] = 0;
	_vertexBuffer[1] = 0;

	glDeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;

	_isDrawInitialised = false;
}

bool Image::_InitTexture(ResourceLib& resourceLib)
{
	auto textureOpt = resourceLib.GetResource(_drawParams.Texture);
	
	if (!textureOpt.has_value())
		return false;

	auto resource = textureOpt.value().lock();

	if (!resource)
		return false;

	if (TEXTURE != resource->GetType())
		return false;
	
	_texture = std::dynamic_pointer_cast<TextureResource>(resource);

	return true;
}

bool Image::_InitShader(ResourceLib& resourceLib)
{
	auto shaderOpt = resourceLib.GetResource(_shaderName);

	if (!shaderOpt.has_value())
		return false;

	auto resource = shaderOpt.value().lock();

	if (!resource)
		return false;

	if (SHADER != resource->GetType())
		return false;

	_shader = std::dynamic_pointer_cast<ShaderResource>(resource);

	return true;
}

bool Image::_InitVertexArray()
{
	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(2, _vertexBuffer);

	GLfloat verts[] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f,  1.0f, 0.0f,
		0.0f,  1.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		1.0f, 1.0f, 0.0f,
	};

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
	glBufferData(GL_ARRAY_BUFFER, 18 * sizeof(GLfloat), verts, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	static const GLfloat uvs[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,
		0.0f, 1.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
	};

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[1]);
	glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLfloat), uvs, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(1);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("Image::InitVertexArray");

	return true;
}
