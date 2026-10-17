#pragma once

#include <string>
#include <iostream>
#include <optional>
#include "../graphics/Shader.h"
#include "Resource.h"

namespace resources
{
	class ShaderResource : public Resource
	{
	public:
		ShaderResource(std::string name, GLuint shaderProgram, std::vector<std::string> uniforms);
		~ShaderResource();

		// Delete the copy constructor/assignment
		ShaderResource(const ShaderResource&) = delete;
		ShaderResource& operator=(const ShaderResource&) = delete;

		ShaderResource(ShaderResource&& other) :
			Resource(other._name),
			_shaderProgram(other._shaderProgram),
			_uniforms(other._uniforms)
		{
			other._name = "";
			other._shaderProgram = 0;
			other._uniforms = {};

		}

		ShaderResource& operator=(ShaderResource&& other)
		{
			if (this != &other)
			{

				Release();
				std::swap(_name, other._name);
				std::swap(_shaderProgram, other._shaderProgram);
				std::swap(_uniforms, other._uniforms);
			}

			return *this;
		}

		virtual Type GetType() const override { return SHADER; }
		virtual GLuint GetId() const override { return _shaderProgram; }
		virtual void Release() override;

		void SetUniforms(graphics::GlDrawContext& ctx);

		static std::optional<GLuint> Load(const std::string& vertFilePath, const std::string& fragFilePath);

	private:
		void InitUniforms(std::vector<std::string> uniforms);

		static bool AddStageFromFile(GLuint shaderProgram, const std::string& filePath, GLenum shaderType);

	private:
		GLuint _shaderProgram;
		std::map<std::string, GLint> _uniforms;
	};
}
