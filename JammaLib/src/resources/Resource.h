#pragma once

#include <optional>
#include <memory>
#include <gl/glew.h>
#include <gl/gl.h>

#include <string>

namespace resources
{
	enum Type
	{
		NONE = 0,
		TEXTURE = 1,
		SHADER = 2,
		WAV = 3,
		CUBEMAP = 4
	};

	class Resource
	{
	public:
		Resource(std::string name) : _name(name) {}

	public:
		virtual Type GetType() const { return NONE; }
		virtual GLuint GetId() const { return 0; }
		virtual void Release() {}

		template<typename T>
		static std::optional<std::weak_ptr<T>> ToResource(std::optional<std::weak_ptr<Resource>> resource)
		{
			if (!resource.has_value())
				return std::nullopt;

			auto res = resource.value().lock();

			if (!res)
				return std::nullopt;

			auto texRes = std::dynamic_pointer_cast<T>(res);

			if (texRes)
				return texRes;
			else
				return std::nullopt;
		}

	protected:
		std::string _name;
	};
}
