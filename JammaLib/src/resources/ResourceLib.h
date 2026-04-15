#pragma once

#include <vector>
#include <map>
#include <optional>
#include <memory>
#include <gl/glew.h>
#include <gl/gl.h>
#include "Resource.h"
#include "TextureResource.h"
#include "ShaderResource.h"
#include "WavResource.h"
#include "CubemapResource.h"
#include "../graphics/Font.h"

namespace resources
{
	class ResourceLib
	{
	public:
		ResourceLib();

	public:
		int NumResources() const;
		void ClearResources();
		bool LoadResource(Type type, std::string name, std::vector<std::string> args);
		std::optional<std::weak_ptr<Resource>> GetResource(const std::string& name);
		bool LoadFonts();
		std::optional<std::weak_ptr<graphics::Font>> GetFont(graphics::FontOptions::FontSize size);

	private:

		std::map<std::string, std::shared_ptr<Resource>> _resources;
		std::map<graphics::FontOptions::FontSize, std::shared_ptr<graphics::Font>> _fonts;
	};
}
