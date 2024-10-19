#include "ShaderResource.h"
#include "glm/gtc/type_ptr.hpp"

using namespace resources;
using namespace graphics;

ShaderResource::ShaderResource(std::string name,
	GLuint shaderProgram,
	std::vector<std::string> uniforms) :
	Resource(name),
	_shaderProgram(shaderProgram)
{
	glUseProgram(shaderProgram);
	InitUniforms(uniforms);
	glUseProgram(0);
}

ShaderResource::~ShaderResource()
{
	Release();
}

std::optional<GLuint> ShaderResource::Load(const std::string& vertFilePath, const std::string& fragFilePath)
{
	auto res = true;
	auto shaderProgram = glCreateProgram();

	res &= AddStageFromFile(shaderProgram, vertFilePath, GL_VERTEX_SHADER);
	res &= AddStageFromFile(shaderProgram, fragFilePath, GL_FRAGMENT_SHADER);

	if (!res)
	{
		glDeleteProgram(shaderProgram);
		return std::nullopt;
	}

	glLinkProgram(shaderProgram);

	GLint glResult;
	glGetProgramiv(shaderProgram, GL_LINK_STATUS, &glResult);

	if (GL_FALSE == glResult)
	{
		glDeleteProgram(shaderProgram);
		return std::nullopt;
	}

	return shaderProgram;
}

void ShaderResource::Release()
{
	glDeleteProgram(_shaderProgram);
	_shaderProgram = 0;
}

void ShaderResource::SetUniforms(GlDrawContext& ctx)
{
	// Find any matching uniforms
	for (auto uniform : _uniforms)
	{
		auto uniformOpt = ctx.GetUniform(uniform.first);

		if (uniformOpt.has_value())
		{
			auto val = uniformOpt.value();

			if (val.type() == typeid(int))
				glUniform1i(uniform.second, std::any_cast<int&>(val));
			else if (val.type() == typeid(unsigned int))
				glUniform1i(uniform.second, std::any_cast<unsigned int&>(val));
			else if (val.type() == typeid(float))
				glUniform1f(uniform.second, std::any_cast<float&>(val));
			else if (val.type() == typeid(glm::mat4))
				glUniformMatrix4fv(uniform.second, 1, GL_FALSE, glm::value_ptr(std::any_cast<glm::mat4&>(val)));
			else if (val.type() == typeid(std::vector<glm::mat4>))
			{
				auto stack = std::any_cast<std::vector<glm::mat4>>(val);
				auto collapsed = glm::mat4(1.0);
				for (auto m : stack)
				{
					collapsed *= m;
				}
				glUniformMatrix4fv(uniform.second, 1, GL_FALSE, glm::value_ptr(collapsed));
			}
		}
	}

	//GlUtils::CheckError("ShaderResource::SetUniforms");
}

void ShaderResource::InitUniforms(std::vector<std::string> uniforms)
{
	_uniforms.clear();

	for (auto uniform : uniforms) {
		auto uniformLoc = glGetUniformLocation(_shaderProgram, uniform.c_str());

		if (uniformLoc >= 0)
			_uniforms[uniform] = uniformLoc;
	}
}

bool ShaderResource::AddStageFromFile(GLuint shaderProgram, const std::string& filePath, GLenum shaderType)
{
	std::ifstream f;
	f.open(filePath.c_str(), std::ios::in | std::ios::binary);

	if (!f.is_open())
	{
		std::cout << "Failed to open file: " << filePath << std::endl;
	}

	std::stringstream buffer;
	buffer << f.rdbuf();

	auto shader = glCreateShader(shaderType);
	if (0 == shader)
	{
		std::cout << "glCreateShader failed" << std::endl;
		return false;
	}

	auto shaderStr = buffer.str();
	const char* code = shaderStr.c_str();
	glShaderSource(shader, 1, (const GLchar**)&code, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

	if (GL_FALSE == status)
	{
		std::string msg("Compile failure in shader:\n");

		GLint infoLogLength;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
		auto strInfoLog = new char[infoLogLength + 1];
		glGetShaderInfoLog(shader, infoLogLength, NULL, strInfoLog);
		msg += strInfoLog;
		delete[] strInfoLog;

		std::cout << msg << std::endl;

		glDeleteShader(shader);
		return false;
	}

	utils::GlUtils::CheckError("ShaderResource::AddStageFromFile");

	glAttachShader(shaderProgram, shader);
	glDeleteShader(shader);

	return true;
}