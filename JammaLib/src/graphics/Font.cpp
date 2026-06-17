#define STB_TRUETYPE_IMPLEMENTATION
#include "Font.h"
#include "GlDeleteQueue.h"
#include <iterator>

using namespace graphics;
using resources::ShaderResource;

Font::Font()
{
}

Font::Font(FontOptions::FontParams params,
	std::vector<float> charWidths,
	std::weak_ptr<ShaderResource> shader) :
	_params(params),
	_charWidths(charWidths),
	_shader(shader)
{
}

Font::~Font()
{
	if (_atlasTexture != 0)
	{
		graphics::GlDeleteQueue::DeleteTextures(1, &_atlasTexture);
		_atlasTexture = 0;
	}
}

std::optional<std::unique_ptr<Font>> Font::Load(FontOptions::FontSize size,
	std::weak_ptr<ShaderResource> shader)
{
	auto datafilename = GetFontFilename();
	std::cout << "Font::Load reading TTF from " << datafilename << " for size " << (int)size << "\n";
	std::ifstream inputFile(datafilename.c_str(), std::ios::binary);
	if (!inputFile)
	{
		std::cout << "Font::Load failed to open " << datafilename << "\n";
		return std::nullopt;
	}

	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(inputFile)), std::istreambuf_iterator<char>());
	inputFile.close();
	if (bytes.empty())
	{
		std::cout << "Font::Load loaded an empty font file " << datafilename << "\n";
		return std::nullopt;
	}

	auto font = std::make_unique<Font>();
	font->_fontData = std::move(bytes);
	font->_params = { 1, 1, 1, 16.0f, 0, 0, size };
	font->_shader = shader;
	font->_charWidths = std::vector<float>(MaxChars, 8.0f);
	font->_charWidths[font->_params.SpaceChar] = 6.0f;
	font->_charWidths[font->_params.DegreeChar] = 8.0f;
	if (!font->_LoadGlyphs(size))
	{
		std::cout << "Font::Load failed to initialise stb font data for " << datafilename << "\n";
		return std::nullopt;
	}

	std::cout << "Font::Load loaded atlas " << font->_atlasWidth << "x" << font->_atlasHeight
		<< " for size " << (int)size << "\n";

	return font;
}

std::string Font::GetFontName(FontOptions::FontSize size)
{
	switch (size)
	{
	case FontOptions::FONT_TINY:
		return "font_tiny";
	case FontOptions::FONT_SMALL:
		return "font_small";
	case FontOptions::FONT_MEDIUM:
		return "font_medium";
	case FontOptions::FONT_LARGE:
		return "font_large";
	}

	return "";
}

std::string Font::GetFontFilename()
{
	return "./resources/fonts/Inter-Regular.ttf";
}

bool Font::_LoadGlyphs(FontOptions::FontSize size)
{
	if (_fontData.empty())
	{
		std::cout << "Font::_LoadGlyphs called with empty font data\n";
		return false;
	}

	if (stbtt_InitFont(&_fontInfo, _fontData.data(), 0))
	{
		_fontScale = stbtt_ScaleForPixelHeight(&_fontInfo, 16.0f + static_cast<int>(size));
		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&_fontInfo, &ascent, &descent, &lineGap);
		_fontAscent = ascent * _fontScale;
		_fontDescent = descent * _fontScale;
		_fontLineGap = lineGap * _fontScale;
		_params.CharHeight = std::max(1.0f, _fontAscent - _fontDescent);
		_params.SpaceChar = 0;
		_params.DegreeChar = 176 - 32;
		_glyphs.clear();
		_glyphs.reserve(kGlyphEndCodepoint - kGlyphStartCodepoint + 1);

		std::cout << "Font::_LoadGlyphs scale=" << _fontScale
			<< " ascent=" << _fontAscent << " descent=" << _fontDescent
			<< " charHeight=" << _params.CharHeight << " size=" << (int)size << "\n";

		std::vector<unsigned char> atlas(kAtlasWidth * kAtlasHeight, 0);
		int x = kGlyphPadding;
		int y = kGlyphPadding;
		int rowHeight = 0;
		int renderedGlyphs = 0;
		for (unsigned int codepoint = kGlyphStartCodepoint; codepoint <= kGlyphEndCodepoint; ++codepoint)
		{
			int glyphIndex = stbtt_FindGlyphIndex(&_fontInfo, codepoint);
			int x0 = 0;
			int y0 = 0;
			int x1 = 0;
			int y1 = 0;
			int advance = 0;
			int leftSideBearing = 0;
			stbtt_GetGlyphBitmapBoxSubpixel(&_fontInfo,
				glyphIndex,
				_fontScale,
				_fontScale,
				0.0f,
				0.0f,
				&x0,
				&y0,
				&x1,
				&y1);
			stbtt_GetGlyphHMetrics(&_fontInfo, glyphIndex, &advance, &leftSideBearing);
			const int width = x1 - x0;
			const int height = y1 - y0;
			if (width <= 0 || height <= 0)
			{
				_glyphs.push_back({ codepoint, static_cast<float>(advance) * _fontScale, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
				continue;
			}
			if (x + width + kGlyphPadding > kAtlasWidth)
			{
				x = kGlyphPadding;
				y += rowHeight + kGlyphPadding;
				rowHeight = 0;
			}
			if (y + height + kGlyphPadding > kAtlasHeight)
			{
				std::cout << "Font::_LoadGlyphs atlas overflow at codepoint " << codepoint
					<< " for size " << (int)size << "\n";
				return false;
			}
			stbtt_MakeGlyphBitmap(&_fontInfo, atlas.data() + y * kAtlasWidth + x, width, height, kAtlasWidth, _fontScale, _fontScale, glyphIndex);
			const float atlasU0 = static_cast<float>(x) / kAtlasWidth;
			const float atlasU1 = static_cast<float>(x + width) / kAtlasWidth;
			const float atlasVTop = static_cast<float>(y) / kAtlasHeight;
			const float atlasVBottom = static_cast<float>(y + height) / kAtlasHeight;
			_glyphs.push_back({ codepoint,
				static_cast<float>(advance) * _fontScale,
				static_cast<float>(x0),
				static_cast<float>(y0),
				static_cast<float>(width),
				static_cast<float>(height),
				atlasU0,
				atlasVTop,
				atlasU1,
				atlasVBottom });
			x += width + kGlyphPadding;
			rowHeight = std::max(rowHeight, height);
			++renderedGlyphs;
		}

		int nonZeroPixels = 0;
		for (auto b : atlas)
			if (b) ++nonZeroPixels;
		std::cout << "Font::_LoadGlyphs rendered " << renderedGlyphs << " glyphs, "
			<< nonZeroPixels << " non-zero atlas pixels (of " << atlas.size() << ") for size " << (int)size << "\n";

		_atlasWidth = kAtlasWidth;
		_atlasHeight = kAtlasHeight;
		_params.NumWidth = 16;
		_params.NumHeight = 16;
		_params.GridSize = 16;
		_charWidths.clear();
		_charWidths.resize(MaxChars, 8.0f);
		if (_atlasTexture != 0)
			glDeleteTextures(1, &_atlasTexture);
		glGenTextures(1, &_atlasTexture);
		glBindTexture(GL_TEXTURE_2D, _atlasTexture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasWidth, kAtlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		utils::GlUtils::CheckError("Font::_LoadGlyphs");
		std::cout << "Font::_LoadGlyphs atlas texture id=" << _atlasTexture << " for size " << (int)size << "\n";
		return true;
	}

	std::cout << "Font::_LoadGlyphs stbtt_InitFont failed\n";
	return false;
}

GLuint Font::InitVertexArray(const std::string& str, GLenum usage, GLuint* outPosBuffer, GLuint* outUvBuffer)
{
	if (str.empty())
		return 0;

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint vbo[2];
	glGenBuffers(2, vbo);

	auto numChars = str.size();
	auto numVerts = numChars * 6;
	std::vector<GLfloat> pos(numVerts * 3);
	std::vector<GLfloat> uv(numVerts * 2);

	float xOffset = 0.0f;
	unsigned int previousCodepoint = 0;
	for (unsigned int i = 0; i < numChars; ++i)
	{
		auto codepoint = static_cast<unsigned char>(str[i]);
		auto glyph = _GetGlyph(codepoint);
		auto advance = _MeasureCodepoint(codepoint, previousCodepoint);
		previousCodepoint = codepoint;
		if (glyph.Width <= 0.0f || glyph.Height <= 0.0f)
		{
			xOffset += advance;
			continue;
		}

		int posStartIndex = i * 3 * 6;
		int uvStartIndex = i * 2 * 6;
		const float x0 = xOffset + glyph.XOffset;
		const float y0 = -glyph.YOffset - glyph.Height;
		const float y1 = -glyph.YOffset;
		const float x1 = x0 + glyph.Width;
		pos[posStartIndex + 0] = x0; pos[posStartIndex + 1] = y0; pos[posStartIndex + 2] = 0.0f;
		pos[posStartIndex + 3] = x0; pos[posStartIndex + 4] = y1; pos[posStartIndex + 5] = 0.0f;
		pos[posStartIndex + 6] = x1; pos[posStartIndex + 7] = y0; pos[posStartIndex + 8] = 0.0f;
		pos[posStartIndex + 9] = x1; pos[posStartIndex + 10] = y0; pos[posStartIndex + 11] = 0.0f;
		pos[posStartIndex + 12] = x0; pos[posStartIndex + 13] = y1; pos[posStartIndex + 14] = 0.0f;
		pos[posStartIndex + 15] = x1; pos[posStartIndex + 16] = y1; pos[posStartIndex + 17] = 0.0f;
		uv[uvStartIndex + 0] = glyph.AtlasU0; uv[uvStartIndex + 1] = glyph.AtlasV1;
		uv[uvStartIndex + 2] = glyph.AtlasU0; uv[uvStartIndex + 3] = glyph.AtlasV0;
		uv[uvStartIndex + 4] = glyph.AtlasU1; uv[uvStartIndex + 5] = glyph.AtlasV1;
		uv[uvStartIndex + 6] = glyph.AtlasU1; uv[uvStartIndex + 7] = glyph.AtlasV1;
		uv[uvStartIndex + 8] = glyph.AtlasU0; uv[uvStartIndex + 9] = glyph.AtlasV0;
		uv[uvStartIndex + 10] = glyph.AtlasU1; uv[uvStartIndex + 11] = glyph.AtlasV0;
		xOffset += advance;
	}

	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, pos.size() * sizeof(GLfloat), pos.data(), usage);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	glBufferData(GL_ARRAY_BUFFER, uv.size() * sizeof(GLfloat), uv.data(), usage);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(1);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	if (outPosBuffer)
		*outPosBuffer = vbo[0];
	if (outUvBuffer)
		*outUvBuffer = vbo[1];

	utils::GlUtils::CheckError("Font::InitVertexArray");
	return vao;
}

void Font::Draw(GlDrawContext& ctx, GLuint vertexArray, unsigned int numChars)
{
	auto shader = _shader.lock();
	if (!shader)
	{
		if (!_hasLoggedMissingShader)
		{
			std::cout << "Font::Draw skipped: missing shader resource\n";
			_hasLoggedMissingShader = true;
		}
		return;
	}

	if (_atlasTexture == 0)
	{
		if (!_hasLoggedMissingAtlas)
		{
			std::cout << "Font::Draw skipped: atlas texture not initialised\n";
			_hasLoggedMissingAtlas = true;
		}
		return;
	}

	ctx.SetUniform("TextureSampler", 0u);
	glUseProgram(shader->GetId());
	shader->SetUniforms(ctx);
	glBindVertexArray(vertexArray);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _atlasTexture);
	glDrawArrays(GL_TRIANGLES, 0, numChars * 6);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

float Font::MeasureString(const std::string& str) const
{
	float total = 0.0f;
	unsigned int previousCodepoint = 0;
	for (unsigned char ch : str)
	{
		total += _MeasureCodepoint(ch, previousCodepoint);
		previousCodepoint = ch;
	}
	return total;
}

float Font::GetHeight() const
{
	return _params.CharHeight;
}

int Font::GetCharNum(char c) const
{
	return static_cast<int>(c) - 32;
}

Font::GlyphMetrics Font::_GetGlyph(unsigned int codepoint) const
{
	for (const auto& glyph : _glyphs)
	{
		if (glyph.Codepoint == codepoint)
			return glyph;
	}
	return {};
}

float Font::_MeasureCodepoint(unsigned int codepoint, unsigned int prevCodepoint) const
{
	if (_glyphs.empty())
		return 8.0f;
	if (codepoint == ' ')
		return 6.0f;
	int glyphIndex = stbtt_FindGlyphIndex(const_cast<stbtt_fontinfo*>(&_fontInfo), codepoint);
	int prevIndex = prevCodepoint ? stbtt_FindGlyphIndex(const_cast<stbtt_fontinfo*>(&_fontInfo), prevCodepoint) : 0;
	int kern = prevCodepoint ? stbtt_GetCodepointKernAdvance(const_cast<stbtt_fontinfo*>(&_fontInfo), prevIndex, glyphIndex) : 0;
	for (const auto& glyph : _glyphs)
	{
		if (glyph.Codepoint == codepoint)
			return glyph.Advance + (float)kern * _fontScale;
	}
	return 8.0f;
}

