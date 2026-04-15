#include "ResourceLib.h"

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
	return _resources.clear();
}

bool ResourceLib::LoadResource(Type type, std::string name, std::vector<std::string> args)
{
	switch (type)
	{
		case TEXTURE:
		{
			auto texFile = "./resources/textures/" + name + ".tga";
			auto texOpt = TextureResource::Load(texFile);

			if (texOpt.has_value())
			{
				auto[tex, width, height] = texOpt.value();
				_resources.emplace(name, std::make_shared<TextureResource>(name, tex, width, height));

				auto t2 = _resources[name]->GetType();

				return true;
			}
		}
		case SHADER:
		{
			auto vertFile = "./resources/shaders/" + name + ".vert";
			auto fragFile = "./resources/shaders/" + name + ".frag";
			auto shader = ShaderResource::Load(vertFile, fragFile);

			if (shader.has_value())
				_resources.emplace(name, std::make_shared<ShaderResource>(name, shader.value(), args));

			return true;
		}
		case WAV:
		{
			auto wavFile = "./resources/wav/" + name + ".wav";
			auto wavOpt = WavResource::Load(wavFile);

			if (wavOpt.has_value())
			{ 
				auto[wav, numSamps, sampleRate] = wavOpt.value();
				_resources.emplace(name, std::make_shared<WavResource>(name, wav, numSamps, sampleRate));
			}

			return true;
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
	// Load font specific textures, if not already loaded
	for (auto size : FontOptions::FontSizes)
	{
		LoadResource(TEXTURE, Font::GetFontName(size), {});
	}

	if (_fonts.size() >= (sizeof(FontOptions::FontSizes) / sizeof(int)))
		return true;

	_fonts.clear();

	std::shared_ptr<ShaderResource> shader;
	auto shaderOpt = GetResource("texture");

	if (shaderOpt.has_value())
	{
		auto shaderResource = shaderOpt.value().lock();
		if (shaderResource)
		{
			if (SHADER == shaderResource->GetType())
				shader = std::dynamic_pointer_cast<ShaderResource>(shaderResource);
		}
	}

	bool res = false;
	for (auto size : FontOptions::FontSizes)
	{
		auto fontName = Font::GetFontName(size);
		
		std::shared_ptr<TextureResource> texture;
		auto textureOpt = GetResource(fontName);

		if (textureOpt.has_value())
		{
			auto textureResource = textureOpt.value().lock();
			if (textureResource)
			{
				if (TEXTURE == textureResource->GetType())
					texture = std::dynamic_pointer_cast<TextureResource>(textureResource);
			}
		}

		auto font = Font::Load(size, texture, shader);

		if (font.has_value())
			_fonts[size] = std::move(font.value());
		else
			res = false;
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
