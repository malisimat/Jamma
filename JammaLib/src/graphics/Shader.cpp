#include "Shader.h"
#include "glm/gtc/type_ptr.hpp"

using namespace graphics;
using utils::GlUtils;

Shader::Shader() :
	_state(EMPTY),
	_shader(0)
{
}

Shader::Shader(GLuint shader) :
	_state(COMPILED),
	_shader(shader)
{
}

Shader::~Shader()
{
}

bool Shader::Init(GLuint program, std::vector<std::string> uniforms)
{
	_uniforms.clear();

	for (auto uniform : uniforms) {
		auto uniformLoc = glGetUniformLocation(program, uniform.c_str());

		if (uniformLoc >= 0)
			_uniforms[uniform] = uniformLoc;
	}

	GlUtils::CheckError("Shader::Init");

	_state = READY;

	return true;
}

GLuint Shader::Name() const
{
	return _shader;
}

void Shader::Activate(GlDrawContext& ctx)
{
	if (_state == READY)
	{
		// Find any matching uniforms
		for (auto uniform : _uniforms)
		{
			auto valOpt = ctx.GetUniform(uniform.first);

			if (valOpt.has_value())
			{
				std::any val = valOpt.value();

				if (val.type() == typeid(int))
					glUniform1i(uniform.second, std::any_cast<int&>(val));
				else if (val.type() == typeid(float))
					glUniform1f(uniform.second, std::any_cast<float&>(val));
				else if (val.type() == typeid(glm::mat4))
					glUniformMatrix4fv(uniform.second, 1, GL_FALSE, glm::value_ptr(std::any_cast<glm::mat4&>(val)));
			}
		}

		//GlUtils::CheckError("Shader::Activate");
	}
}

void Shader::Release()
{
	glDeleteShader(_shader);

	_state = EMPTY;
}

std::optional<Shader> Shader::FromFile(const std::string& filePath, GLenum shaderType)
{
	std::ifstream f;
	f.open(filePath.c_str(), std::ios::in | std::ios::binary);

	if (!f.is_open()) {
		throw std::runtime_error(std::string("Failed to open file: ") + filePath);
	}

	std::stringstream buffer;
	buffer << f.rdbuf();

	GLuint shader = glCreateShader(shaderType);
	if (shader == 0)
	{
		std::cout << "[LOAD] glCreateShader failed" << std::endl;
		//throw std::runtime_error("glCreateShader failed");
		return std::nullopt;
	}

	auto shaderStr = buffer.str();
	const char* code = shaderStr.c_str();
	glShaderSource(shader, 1, (const GLchar**)&code, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		std::string msg("Compile failure in shader:\n");

		GLint infoLogLength;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
		auto strInfoLog = new char[infoLogLength + 1];
		glGetShaderInfoLog(shader, infoLogLength, NULL, strInfoLog);
		msg += strInfoLog;
		delete[] strInfoLog;

		glDeleteShader(shader); shader = 0;

		std::cout << "[LOAD] " << msg << std::endl;

		return std::nullopt;
	}

	GlUtils::CheckError("Shader::FromFile");

	return std::optional<Shader>(Shader(shader));
}
