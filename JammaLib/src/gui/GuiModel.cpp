#include "GuiModel.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace base;
using namespace gui;
using namespace resources;
using namespace utils;
using graphics::GlDrawContext;

GuiModel::GuiModel(GuiModelParams params) :
	GuiElement(params),
	_geometryNeedsUpdating(false),
	_modelParams(params),
	_numTris(0),
	_backVerts({}),
	_backUvs({})
{
}

void GuiModel::Draw3d(DrawContext& ctx,
	unsigned int numInstances)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = ModelPosition();
	auto scale = ModelScale();

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	auto texture = _modelTexture.lock();
	auto shader = _modelShader.lock();

	if (!texture || !shader)
		return;

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glBindTexture(GL_TEXTURE_2D, texture->GetId());
	if (numInstances > 1)
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, numInstances);
	else
		glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	for (auto& child : _children)
		child->Draw3d(ctx, 1);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void GuiModel::SetGeometry(std::vector<float> verts, std::vector<float> uvs)
{
	_backVerts = verts;
	_backUvs = uvs;

	_geometryNeedsUpdating = true;
	_resourcesNeedInitialising = true;
}

void GuiModel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	if (validated)
		validated = InitTexture(resourceLib);
	if (validated)
		validated = InitShader(resourceLib);
	if (validated)
	{
		if (_geometryNeedsUpdating)
		{
			_geometryNeedsUpdating = false;
			_modelParams.Verts = _backVerts;
			_modelParams.Uvs = _backUvs;
		}

		validated = InitVertexArray(_modelParams.Verts, _modelParams.Uvs);
	}

	GlUtils::CheckError("GuiModel::_InitResources()");
}

void GuiModel::_ReleaseResources()
{
	glDeleteBuffers(2, _vertexBuffer);
	_vertexBuffer[0] = 0;
	_vertexBuffer[1] = 0;

	glDeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

bool GuiModel::InitTexture(ResourceLib& resourceLib)
{
	auto textureOpt = resourceLib.GetResource(_modelParams.ModelTexture);

	if (!textureOpt.has_value())
		return false;

	auto resource = textureOpt.value().lock();

	if (!resource)
		return false;

	if (TEXTURE != resource->GetType())
		return false;

	_modelTexture = std::dynamic_pointer_cast<TextureResource>(resource);

	return true;
}

bool GuiModel::InitShader(ResourceLib & resourceLib)
{
	auto shaderOpt = resourceLib.GetResource(_modelParams.ModelShader);

	if (!shaderOpt.has_value())
		return false;

	auto resource = shaderOpt.value().lock();

	if (!resource)
		return false;

	if (SHADER != resource->GetType())
		return false;

	_modelShader = std::dynamic_pointer_cast<ShaderResource>(resource);

	return true;
}

bool GuiModel::InitVertexArray(std::vector<float> verts, std::vector<float> uvs)
{
	_numTris = (unsigned int)verts.size() / 9;
	auto numFaces = _numTris / 2;

	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(3, _vertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[0]);
	glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GLfloat), verts.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[1]);
	glBufferData(GL_ARRAY_BUFFER, uvs.size() * sizeof(GLfloat), uvs.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);

	std::vector<GLfloat> norms;
	for (auto tri = 0u; tri < _numTris; tri++)
	{
		auto v1 = glm::vec3(verts[(tri * 3 + 0) * 3], verts[(tri * 3 + 0) * 3 + 1], verts[(tri * 3 + 0) * 3 + 2]);
		auto v2 = glm::vec3(verts[(tri * 3 + 1) * 3], verts[(tri * 3 + 1) * 3 + 1], verts[(tri * 3 + 1) * 3 + 2]);
		auto v3 = glm::vec3(verts[(tri * 3 + 2) * 3], verts[(tri * 3 + 2) * 3 + 1], verts[(tri * 3 + 2) * 3 + 2]);

		auto v12 = glm::normalize(v2 - v1);
		auto v13 = glm::normalize(v3 - v1);
		auto norm = glm::vec3(1.0f, 0.0f, 0.0f);
		if ((glm::length(v12) < 1.1f) && (glm::length(v13) < 1.1f))
			norm = glm::normalize(glm::cross(v12, v13));

		norms.push_back(norm.x); norms.push_back(norm.y); norms.push_back(norm.z);
		norms.push_back(norm.x); norms.push_back(norm.y); norms.push_back(norm.z);
		norms.push_back(norm.x); norms.push_back(norm.y); norms.push_back(norm.z);
	}

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer[2]);
	glBufferData(GL_ARRAY_BUFFER, norms.size() * sizeof(GLfloat), norms.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("Image::InitVertexArray");

	return true;
}
