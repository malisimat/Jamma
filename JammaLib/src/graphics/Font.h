#pragma once

///////////////////////////////////////////////////////////////
//
// Font header file
// ================
//
// Contains definition of font character widths
//
///////////////////////////////////////////////////////////////

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <gl/glew.h>
#include <gl/gl.h>
#include "../resources/TextureResource.h"
#include "../resources/ShaderResource.h"
#include "../graphics/GlDrawContext.h"
#include "../utils/FunctionUtils.h"

namespace graphics
{
	namespace FontOptions
	{
		enum FontSize
		{
			FONT_TINY,
			FONT_SMALL,
			FONT_MEDIUM,
			FONT_LARGE
		};

		static const FontOptions::FontSize FontSizes[] = { FontOptions::FONT_LARGE, FontOptions::FONT_MEDIUM, FontOptions::FONT_SMALL, FontOptions::FONT_TINY };

		enum TextAlign
		{
			TEXTALIGN_LEFT,
			TEXTALIGN_RIGHT,
			TEXTALIGN_CENTRE
		};

		struct Colour
		{
		public:
			float R;
			float G;
			float B;
		};

		static const Colour White{ 1.0f, 1.0f, 1.0f };

		struct FontParams
		{
			unsigned int NumWidth;
			unsigned int NumHeight;
			unsigned int GridSize;
			float CharHeight;
			unsigned int SpaceChar;
			unsigned int DegreeChar;
			FontSize Size;
		};
	}

	class Font
	{
	public:
		Font();
		Font(FontOptions::FontParams params,
			std::vector<float> charWidths,
			std::weak_ptr<resources::TextureResource> texture,
			std::weak_ptr<resources::ShaderResource> shader);

		// Copy
		Font(const Font&) = delete;
		Font& operator=(const Font&) = delete;

		// Move
		Font(Font&& other) :
			_params(other._params),
			_charWidths(std::move(other._charWidths)),
			_texture(other._texture),
			_shader(other._shader)
		{
			other._params = {};
			other._charWidths = {};
			other._texture = std::weak_ptr<resources::TextureResource>();
			other._shader = std::weak_ptr<resources::ShaderResource>();
		}

		Font& operator=(Font&& other)
		{
			if (this != &other)
			{
				std::swap(_params, other._params);
				std::swap(_charWidths, other._charWidths);
				_texture.swap(other._texture);
				_shader.swap(other._shader);
			}

			return *this;
		}

		GLuint InitVertexArray(const std::string& str, GLenum usage, GLuint* outPosBuffer = nullptr, GLuint* outUvBuffer = nullptr);
		void Draw(GlDrawContext& ctx, GLuint vertexArray, unsigned int numChars);
		float MeasureString(const std::string& str) const;
		float GetHeight() const;

		static std::optional<std::unique_ptr<Font>> Load(FontOptions::FontSize size,
			std::weak_ptr<resources::TextureResource> texture,
			std::weak_ptr<resources::ShaderResource> shader);
		static std::string GetFontName(FontOptions::FontSize size);

	private:
		int GetCharNum(char c) const;
		float FillPosUv(std::vector<GLfloat>& pos,
			std::vector<GLfloat>& uv,
			unsigned int index,
			float xOffset,
			char c,
			float du,
			float dv) const;

		static std::string GetFontFilename(FontOptions::FontSize size);

	public:
		static const unsigned int MaxChars = 256;
		static const unsigned int MaxVerts = 1536;
		static const unsigned int StartChar = 33;

	private:
		FontOptions::FontParams _params;
		std::vector<float> _charWidths;
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
