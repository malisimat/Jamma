#include "ImageFullscreen.h"
#include <gl/glew.h>
#include <gl/gl.h>
#include "gl/glext.h"
#include "gl/wglext.h"
#include "glm/glm.hpp"

using namespace base;
using namespace graphics;
using namespace utils;
using namespace resources;

ImageFullscreen::ImageFullscreen(ImageFullscreenParams params) :
	Drawable(params),
	_texture(0u),
	_vertexArray(0),
	_shaderName(params.Shader),
	_shader(std::weak_ptr<ShaderResource>())
{
}

void ImageFullscreen::Draw(DrawContext& ctx)
{
}

void ImageFullscreen::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	auto shader = _shader.lock();

	if ((0u == _texture) || !shader)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _texture);

	glDrawArrays(GL_TRIANGLES, 0, VertexCount);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

void ImageFullscreen::SetTexture(unsigned int texture)
{
	_texture = texture;
}

void ImageFullscreen::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	if (validated)
		validated = _InitShader(resourceLib);
	if (validated)
		validated = _InitVertexArray();

	GlUtils::CheckError("ImageFullscreen::_InitResources()");
}

void ImageFullscreen::_ReleaseResources()
{
	glDeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

bool ImageFullscreen::_InitShader(ResourceLib& resourceLib)
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

bool ImageFullscreen::_InitVertexArray()
{
	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glBindVertexArray(0);

	GlUtils::CheckError("ImageFullscreen::InitVertexArray");

	return true;
}
