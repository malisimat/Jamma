#include "GuiModel.h"
#include "../graphics/GlDeleteQueue.h"
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
	_instanceAttributesNeedUpdating(false),
	_usesInstanceAttributes(false),
	_modelParams(params),
	_vertexArray(0),
	_vertexBuffer{0,0,0},
	_instanceBuffers({}),
	_numTris(0),
	_backInstanceCount(0),
	_instanceCount(0),
	_backVerts({}),
	_backUvs({}),
	_backInstanceAttributes({}),
	_instanceAttributes({})
{
}

bool GuiModel::HasCurrentGlContext() noexcept
{
	return nullptr != wglGetCurrentContext();
}

void GuiModel::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (!HasCurrentGlContext())
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = ModelPosition();
	auto scale = ModelScale();

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	const auto instanceAttributesNeedUpdating = _instanceAttributesNeedUpdating.load(std::memory_order_acquire);

	if (instanceAttributesNeedUpdating)
	{
		if (!SyncInstanceAttributes())
			_resourcesNeedInitialising = true;
	}

	auto modelTexture = GetTexture();
	auto modelShader = GetShader();

	auto texture = modelTexture.lock();
	auto shader = modelShader.lock();

	if (!texture || !shader)
	{
		glCtx.PopMvp();
		glCtx.PopMvp();
		return;
	}

	if (0u == _vertexArray || 0u == _numTris)
	{
		glCtx.PopMvp();
		glCtx.PopMvp();
		return;
	}

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glBindTexture(GL_TEXTURE_2D, texture->GetId());
	const auto drawInstances = _usesInstanceAttributes ? _instanceCount : numInstances;
	if (drawInstances > 1 || _usesInstanceAttributes)
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, drawInstances);
	else
		glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	for (auto& child : _children)
		child->Draw3d(ctx, 1, pass);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void GuiModel::SetGeometry(std::vector<float> verts, std::vector<float> uvs)
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	_backVerts = std::move(verts);
	_backUvs = std::move(uvs);

	_geometryNeedsUpdating.store(true, std::memory_order_release);
	_resourcesNeedInitialising = true;
}

void GuiModel::SetInstanceAttributes(std::vector<InstanceAttribute> attributes, unsigned int instanceCount)
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	_backInstanceAttributes = std::move(attributes);
	_backInstanceCount = instanceCount;

	_instanceAttributesNeedUpdating.store(true, std::memory_order_release);
	_usesInstanceAttributes = true;
}

void GuiModel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	if (!HasCurrentGlContext())
		return;

	auto validated = true;

	if (validated)
		validated = InitTextures(resourceLib);
	if (validated)
		validated = InitShaders(resourceLib);
	if (validated)
	{
		std::lock_guard<std::mutex> lock(_modelStateMutex);

		if (_geometryNeedsUpdating.load(std::memory_order_acquire))
		{
			_geometryNeedsUpdating.store(false, std::memory_order_release);
			_modelParams.Verts = _backVerts;
			_modelParams.Uvs = _backUvs;
		}

		if (_instanceAttributesNeedUpdating.load(std::memory_order_acquire))
		{
			_instanceAttributesNeedUpdating.store(false, std::memory_order_release);
			_instanceAttributes = _backInstanceAttributes;
			_instanceCount = _backInstanceCount;
		}

		validated = InitVertexArray(_modelParams.Verts, _modelParams.Uvs);
	}

	GlUtils::CheckError("GuiModel::_InitResources()");
}

void GuiModel::_ReleaseResources()
{
	if (!HasCurrentGlContext())
		return;

	graphics::GlDeleteQueue::DeleteBuffers(3, _vertexBuffer);
	_vertexBuffer[0] = 0;
	_vertexBuffer[1] = 0;
	_vertexBuffer[2] = 0;

	if (!_instanceBuffers.empty())
	{
		graphics::GlDeleteQueue::DeleteBuffers((GLsizei)_instanceBuffers.size(), _instanceBuffers.data());
		_instanceBuffers.clear();
	}

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

// ---------------------------------------------------------------------------
// Thread-safe resource accessors
// ---------------------------------------------------------------------------

std::weak_ptr<resources::TextureResource> GuiModel::GetTexture()
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	if (!_modelTextures.empty())
		return _modelTextures.front();
	return {};
}

std::weak_ptr<resources::ShaderResource> GuiModel::GetShader()
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	if (!_modelShaders.empty())
		return _modelShaders.front();
	return {};
}

std::weak_ptr<resources::TextureResource> GuiModel::GetTextureAt(unsigned int index)
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	if (index < _modelTextures.size())
		return _modelTextures[index];
	return {};
}

std::weak_ptr<resources::ShaderResource> GuiModel::GetShaderAt(unsigned int index)
{
	std::lock_guard<std::mutex> lock(_modelStateMutex);
	if (index < _modelShaders.size())
		return _modelShaders[index];
	return {};
}

bool GuiModel::InitTextures(ResourceLib& resourceLib)
{
	bool result = true;
	std::vector<std::weak_ptr<TextureResource>> modelTextures;

	for (std::string texture : _modelParams.ModelTextures)
	{
		auto textureOpt = resourceLib.GetResource(texture);

		if (!textureOpt.has_value())
		{
			result = false;
			continue;
		}

		auto resource = textureOpt.value().lock();

		if (!resource)
		{
			result = false;
			continue;
		}

		if (TEXTURE != resource->GetType())
		{
			result = false;
			continue;
		}

		modelTextures.push_back(std::dynamic_pointer_cast<TextureResource>(resource));
	}

	{
		std::lock_guard<std::mutex> lock(_modelStateMutex);
		_modelTextures = std::move(modelTextures);
	}

	return result;
}

bool GuiModel::InitShaders(ResourceLib & resourceLib)
{
	bool result = true;
	std::vector<std::weak_ptr<ShaderResource>> modelShaders;

	for (std::string shader : _modelParams.ModelShaders)
	{
		auto shaderOpt = resourceLib.GetResource(shader);

		if (!shaderOpt.has_value())
		{
			result = false;
			continue;
		}

		auto resource = shaderOpt.value().lock();

		if (!resource)
		{
			result = false;
			continue;
		}

		if (SHADER != resource->GetType())
		{
			result = false;
			continue;
		}

		modelShaders.push_back(std::dynamic_pointer_cast<ShaderResource>(resource));
	}

	{
		std::lock_guard<std::mutex> lock(_modelStateMutex);
		_modelShaders = std::move(modelShaders);
	}

	return result;
}

bool GuiModel::InitVertexArray(std::vector<float> verts, std::vector<float> uvs)
{
	if (!HasCurrentGlContext())
		return false;

	_ReleaseResources();

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

	if (!InitInstanceAttributes())
		return false;

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("GuiModel::InitVertexArray");

	return true;
}

bool GuiModel::InitInstanceAttributes()
{
	if (!HasCurrentGlContext())
		return false;

	if (!_usesInstanceAttributes || _instanceAttributes.empty())
		return true;

	_instanceBuffers.resize(_instanceAttributes.size(), 0);
	glGenBuffers((GLsizei)_instanceBuffers.size(), _instanceBuffers.data());

	for (auto i = 0u; i < _instanceAttributes.size(); ++i)
	{
		const auto& attribute = _instanceAttributes[i];
		if (0u == attribute.ComponentCount)
			return false;

		glBindBuffer(GL_ARRAY_BUFFER, _instanceBuffers[i]);
		glBufferData(GL_ARRAY_BUFFER,
			attribute.Data.size() * sizeof(GLfloat),
			attribute.Data.data(),
			GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray(attribute.AttributeIndex);
		glVertexAttribPointer(attribute.AttributeIndex,
			attribute.ComponentCount,
			GL_FLOAT,
			GL_FALSE,
			0,
			0);
		glVertexAttribDivisor(attribute.AttributeIndex, 1);
	}

	return true;
}

bool GuiModel::HasSameInstanceAttributeLayout(const std::vector<InstanceAttribute>& lhs,
	const std::vector<InstanceAttribute>& rhs)
{
	if (lhs.size() != rhs.size())
		return false;

	for (auto i = 0u; i < lhs.size(); ++i)
	{
		if ((lhs[i].AttributeIndex != rhs[i].AttributeIndex) ||
			(lhs[i].ComponentCount != rhs[i].ComponentCount))
			return false;
	}

	return true;
}

bool GuiModel::SyncInstanceAttributes()
{
	if (!HasCurrentGlContext())
		return false;

	if (!_instanceAttributesNeedUpdating.exchange(false, std::memory_order_acq_rel))
		return true;

	auto layoutChanged = false;
	std::vector<InstanceAttribute> nextAttributes;
	unsigned int nextInstanceCount = 0u;
	{
		std::lock_guard<std::mutex> lock(_modelStateMutex);
		nextAttributes = _backInstanceAttributes;
		nextInstanceCount = _backInstanceCount;
		layoutChanged = !HasSameInstanceAttributeLayout(_instanceAttributes, nextAttributes);
	}

	_instanceAttributes = nextAttributes;
	_instanceCount = nextInstanceCount;

	if (0u == _vertexArray)
		return false;

	if (!_usesInstanceAttributes || _instanceAttributes.empty())
	{
		if (!_instanceBuffers.empty())
		{
			graphics::GlDeleteQueue::DeleteBuffers((GLsizei)_instanceBuffers.size(), _instanceBuffers.data());
			_instanceBuffers.clear();
		}

		return true;
	}

	glBindVertexArray(_vertexArray);

	if (layoutChanged || (_instanceBuffers.size() != _instanceAttributes.size()))
	{
		if (!_instanceBuffers.empty())
		{
			graphics::GlDeleteQueue::DeleteBuffers((GLsizei)_instanceBuffers.size(), _instanceBuffers.data());
			_instanceBuffers.clear();
		}

		_instanceBuffers.resize(_instanceAttributes.size(), 0u);
		glGenBuffers((GLsizei)_instanceBuffers.size(), _instanceBuffers.data());

		for (auto i = 0u; i < _instanceAttributes.size(); ++i)
		{
			const auto& attribute = _instanceAttributes[i];
			if (0u == attribute.ComponentCount)
			{
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindVertexArray(0);
				return false;
			}

			glBindBuffer(GL_ARRAY_BUFFER, _instanceBuffers[i]);
			glBufferData(GL_ARRAY_BUFFER,
				attribute.Data.size() * sizeof(GLfloat),
				attribute.Data.data(),
				GL_DYNAMIC_DRAW);
			glEnableVertexAttribArray(attribute.AttributeIndex);
			glVertexAttribPointer(attribute.AttributeIndex,
				attribute.ComponentCount,
				GL_FLOAT,
				GL_FALSE,
				0,
				0);
			glVertexAttribDivisor(attribute.AttributeIndex, 1);
		}
	}
	else
	{
		for (auto i = 0u; i < _instanceAttributes.size(); ++i)
		{
			const auto& attribute = _instanceAttributes[i];
			if (0u == attribute.ComponentCount)
			{
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindVertexArray(0);
				return false;
			}

			glBindBuffer(GL_ARRAY_BUFFER, _instanceBuffers[i]);
			glBufferData(GL_ARRAY_BUFFER,
				attribute.Data.size() * sizeof(GLfloat),
				attribute.Data.data(),
				GL_DYNAMIC_DRAW);
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("GuiModel::SyncInstanceAttributes");

	return true;
}
