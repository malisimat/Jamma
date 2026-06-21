#include "ResourceLib.h"
#include "../utils/StringUtils.h"
#include <array>
#include <cctype>
#include <iostream>
#include <limits>

using namespace resources;
using namespace graphics;

ResourceLib::ResourceLib() :
	_resources({})
{
}

int ResourceLib::NumResources() const
{
	return (int)_resources.size();
}

void ResourceLib::ClearResources()
{
	_fonts.clear();
	_resources.clear();
}

bool ResourceLib::LoadResource(Type type, std::string name, std::vector<std::string> args)
{
	switch (type)
	{
		case TEXTURE:
		{
			bool isNinePatch = false;
			unsigned int borderX = 0;
			unsigned int borderY = 0;
			if (!ParseTextureArgs(args, isNinePatch, borderX, borderY))
			{
				std::cout << "ResourceLib: invalid texture args for '" << name << "'" << std::endl;
				return false;
			}

			const std::array<std::string, 2> texFiles = {
				"./resources/textures/" + name + ".tga",
				"./Jamma/resources/textures/" + name + ".tga"
			};

			for (const auto& texFile : texFiles)
			{
				auto texOpt = TextureResource::Load(texFile);

				if (texOpt.has_value())
				{
					auto[tex, width, height] = texOpt.value();
					_resources.emplace(name, std::make_shared<TextureResource>(
						name,
						tex,
						width,
						height,
						isNinePatch,
						borderX,
						borderY));
					return true;
				}
			}

			std::cout << "ResourceLib: failed to load texture '" << name
				<< "' from known texture paths" << std::endl;

			break;
		}
		case SHADER:
		{
			const std::array<std::string, 2> shaderRoots = {
				"./resources/shaders/",
				"./Jamma/resources/shaders/"
			};

			for (const auto& shaderRoot : shaderRoots)
			{
				auto vertFile = shaderRoot + name + ".vert";
				auto fragFile = shaderRoot + name + ".frag";
				auto shader = ShaderResource::Load(vertFile, fragFile);

				if (shader.has_value())
				{
					_resources.emplace(name, std::make_shared<ShaderResource>(name, shader.value(), args));
					return true;
				}
			}

			std::cout << "ResourceLib: failed to load shader '" << name
				<< "' from known shader paths" << std::endl;

			break;
		}
		case WAV:
		{
			const std::array<std::string, 2> wavFiles = {
				"./resources/wav/" + name + ".wav",
				"./Jamma/resources/wav/" + name + ".wav"
			};

			for (const auto& wavFile : wavFiles)
			{
				auto wavOpt = WavResource::Load(wavFile);

				if (wavOpt.has_value())
				{
					auto[wav, numSamps, sampleRate] = wavOpt.value();
					_resources.emplace(name, std::make_shared<WavResource>(name, wav, numSamps, sampleRate));
					return true;
				}
			}

			std::cout << "ResourceLib: failed to load wav '" << name
				<< "' from known wav paths" << std::endl;

			break;
		}
		case CUBEMAP:
		{
			if (auto cubemapOpt = CubemapResource::Load(name))
				_resources.emplace(name, std::make_shared<CubemapResource>(name, cubemapOpt.value()));

			return true;
		}
	}

	return false;
}

bool ResourceLib::ParseTextureArgs(const std::vector<std::string>& args,
	bool& isNinePatch,
	unsigned int& borderX,
	unsigned int& borderY)
{
	isNinePatch = false;
	borderX = 0;
	borderY = 0;

	if (args.empty())
		return true;

	if (args.size() != 3)
		return false;

	if (args[0] != "ninepatch")
		return false;

	if (!utils::ParseUnsigned(args[1], borderX) || !utils::ParseUnsigned(args[2], borderY))
		return false;

	isNinePatch = true;
	return true;
}

std::optional<std::weak_ptr<Resource>> ResourceLib::GetResource(const std::string& name)
{
	if (_resources.count(name) > 0)
		return _resources.at(name);

	return std::nullopt;
}

bool ResourceLib::LoadFonts()
{
	_fonts.clear();
	std::cout << "ResourceLib::LoadFonts starting font initialisation" << std::endl;

	std::shared_ptr<ShaderResource> shader;
	auto shaderOpt = GetResource("font");
	if (!shaderOpt.has_value())
		shaderOpt = GetResource("texture");

	if (shaderOpt.has_value())
	{
		auto shaderResource = shaderOpt.value().lock();
		if (shaderResource)
		{
			if (SHADER == shaderResource->GetType())
				shader = std::dynamic_pointer_cast<ShaderResource>(shaderResource);
		}
	}

	if (!shader)
		std::cout << "ResourceLib::LoadFonts missing shader resource 'font'/'texture'; font draw will be unavailable" << std::endl;

	bool res = true;
	for (auto size : FontOptions::FontSizes)
	{
		auto fontName = Font::GetFontName(size);
		auto font = Font::Load(size, shader);

		if (font.has_value())
		{
			_fonts[size] = std::move(font.value());
			std::cout << "ResourceLib::LoadFonts loaded " << fontName << std::endl;
		}
		else
		{
			std::cout << "ResourceLib: failed to load font '" << fontName << "'" << std::endl;
			res = false;
		}
	}

	return res;
}

std::optional<std::weak_ptr<Font>> ResourceLib::GetFont(FontOptions::FontSize size)
{
	if (_fonts.count(size) > 0)
	{
		auto font = _fonts.at(size);
		if (nullptr == font)
			return std::nullopt;

		return font;
	}

	return std::nullopt;
}

std::optional<ResourceLib::FontSelection> ResourceLib::SelectClosestFont(unsigned int desiredPixelHeight) const
{
	if (_fonts.empty())
		return std::nullopt;

	const unsigned int desired = std::max(1u, desiredPixelHeight);
	const auto size = Font::GetClosestSizeForPixelHeight(desired);

	if (_fonts.count(size) == 0)
		return std::nullopt;

	return FontSelection{ size, Font::GetPixelHeightForSize(size) };
}

std::optional<std::weak_ptr<Font>> ResourceLib::GetClosestFont(unsigned int desiredPixelHeight,
	FontSelection* selection)
{
	auto selected = SelectClosestFont(desiredPixelHeight);
	if (!selected.has_value())
		return std::nullopt;

	auto fontOpt = GetFont(selected->Size);
	if (!fontOpt.has_value())
		return std::nullopt;

	if (nullptr != selection)
		*selection = selected.value();

	return fontOpt;
}
