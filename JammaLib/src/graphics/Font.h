#pragma once

#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <algorithm>
#include <cmath>
#include <gl/glew.h>
#include <gl/gl.h>
#include "../resources/ShaderResource.h"
#include "../graphics/GlDrawContext.h"
#include "../utils/FunctionUtils.h"
#include "../../lib/stb/stb_truetype.h"

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
		static constexpr unsigned int BasePixelHeight = 16u;

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
		static constexpr int kGlyphPadding = 2;
		static constexpr int kGlyphSize = 32;
		static constexpr int kAtlasWidth = 512;
		static constexpr int kAtlasHeight = 512;
		static constexpr unsigned int kGlyphStartCodepoint = 32;
		static constexpr unsigned int kGlyphEndCodepoint = 126;

		Font();
		Font(FontOptions::FontParams params,
			std::vector<float> charWidths,
			std::weak_ptr<resources::ShaderResource> shader);
		~Font();

		Font(const Font&) = delete;
		Font& operator=(const Font&) = delete;

		Font(Font&& other) :
			_params(other._params),
			_charWidths(std::move(other._charWidths)),
			_fontData(std::move(other._fontData)),
			_glyphs(std::move(other._glyphs)),
			_fontInfo(other._fontInfo),
			_fontScale(other._fontScale),
			_fontAscent(other._fontAscent),
			_fontDescent(other._fontDescent),
			_fontLineGap(other._fontLineGap),
			_atlasWidth(other._atlasWidth),
			_atlasHeight(other._atlasHeight),
			_atlasTexture(other._atlasTexture),
			_shader(other._shader)
		{
			other._params = {};
			other._charWidths = {};
			other._fontData.clear();
			other._glyphs.clear();
			other._fontInfo = {};
			other._fontScale = 0.0f;
			other._fontAscent = 0.0f;
			other._fontDescent = 0.0f;
			other._fontLineGap = 0.0f;
			other._atlasWidth = 0;
			other._atlasHeight = 0;
			other._atlasTexture = 0;
			other._shader = std::weak_ptr<resources::ShaderResource>();
		}

		Font& operator=(Font&& other)
		{
			if (this != &other)
			{
				std::swap(_params, other._params);
				std::swap(_charWidths, other._charWidths);
				_fontData.swap(other._fontData);
				_glyphs.swap(other._glyphs);
				std::swap(_fontInfo, other._fontInfo);
				std::swap(_fontScale, other._fontScale);
				std::swap(_fontAscent, other._fontAscent);
				std::swap(_fontDescent, other._fontDescent);
				std::swap(_fontLineGap, other._fontLineGap);
				std::swap(_atlasWidth, other._atlasWidth);
				std::swap(_atlasHeight, other._atlasHeight);
				std::swap(_atlasTexture, other._atlasTexture);
				_shader.swap(other._shader);
			}

			return *this;
		}

		GLuint InitVertexArray(const std::string& str, GLenum usage, GLuint* outPosBuffer = nullptr, GLuint* outUvBuffer = nullptr);
		void Draw(GlDrawContext& ctx, GLuint vertexArray, unsigned int numChars);
		float MeasureString(const std::string& str) const;
		float GetHeight() const;
		
		static std::optional<std::unique_ptr<Font>> Load(FontOptions::FontSize size,
			std::weak_ptr<resources::ShaderResource> shader);
		static std::string GetFontName(FontOptions::FontSize size);
		static std::string GetFontFilename();
		static unsigned int GetPixelHeightForSize(FontOptions::FontSize size);
		static FontOptions::FontSize GetClosestSizeForPixelHeight(unsigned int desiredPixelHeight);

	private:
		struct GlyphMetrics
		{
			unsigned int Codepoint = 0;
			float Advance = 0.0f;
			float XOffset = 0.0f;
			float YOffset = 0.0f;
			float Width = 0.0f;
			float Height = 0.0f;
			float AtlasU0 = 0.0f;
			float AtlasV0 = 0.0f;
			float AtlasU1 = 1.0f;
			float AtlasV1 = 1.0f;
		};

		int GetCharNum(char c) const;
		bool _LoadGlyphs(FontOptions::FontSize size);
		bool _CreateAtlasTexture(const std::vector<unsigned char>& bitmap, int atlasWidth, int atlasHeight);
		GlyphMetrics _GetGlyph(unsigned int codepoint) const;
		float _MeasureCodepoint(unsigned int codepoint, unsigned int prevCodepoint) const;
		
	public:
		static const unsigned int MaxChars = 256;
		static const unsigned int MaxVerts = 1536;
		static const unsigned int StartChar = 33;

	private:
		FontOptions::FontParams _params;
		std::vector<float> _charWidths;
		std::vector<unsigned char> _fontData;
		std::vector<GlyphMetrics> _glyphs;
		stbtt_fontinfo _fontInfo{};
		float _fontScale = 0.0f;
		float _fontAscent = 0.0f;
		float _fontDescent = 0.0f;
		float _fontLineGap = 0.0f;
		unsigned int _atlasWidth = 0;
		unsigned int _atlasHeight = 0;
		GLuint _atlasTexture = 0;
		std::weak_ptr<resources::ShaderResource> _shader;
			bool _hasLoggedMissingShader = false;
			bool _hasLoggedMissingAtlas = false;
	};
}
