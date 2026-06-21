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
		struct FontSelection
		{
			graphics::FontOptions::FontSize Size = graphics::FontOptions::FONT_LARGE;
			unsigned int PixelHeight = 0u;
		};

		ResourceLib();

	public:
		int NumResources() const;
		void ClearResources();
		bool LoadResource(Type type, std::string name, std::vector<std::string> args);
		static bool ParseTextureArgs(const std::vector<std::string>& args,
			bool& isNinePatch,
			unsigned int& borderX,
			unsigned int& borderY);
		std::optional<std::weak_ptr<Resource>> GetResource(const std::string& name);
		bool LoadFonts();
		std::optional<std::weak_ptr<graphics::Font>> GetFont(graphics::FontOptions::FontSize size);
		static unsigned int ResolveTextPixelHeightFromControlBox(unsigned int controlHeight, unsigned int padding);
		std::optional<FontSelection> SelectClosestFont(unsigned int desiredPixelHeight) const;
		std::optional<FontSelection> SelectClosestFontForControlBox(unsigned int controlHeight, unsigned int padding) const;
		std::optional<std::weak_ptr<graphics::Font>> GetClosestFont(unsigned int desiredPixelHeight,
			FontSelection* selection = nullptr);
		std::optional<std::weak_ptr<graphics::Font>> GetClosestFontForControlBox(unsigned int controlHeight,
			unsigned int padding,
			FontSelection* selection = nullptr);

	private:
		std::map<std::string, std::shared_ptr<Resource>> _resources;
		std::map<graphics::FontOptions::FontSize, std::shared_ptr<graphics::Font>> _fonts;
	};
}
