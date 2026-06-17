#include "ResourceLib.h"
#include <array>
#include <iostream>

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
					_resources.emplace(name, std::make_shared<TextureResource>(name, tex, width, height));
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
