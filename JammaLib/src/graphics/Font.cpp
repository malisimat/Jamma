#include "Font.h"

using namespace graphics;
using resources::TextureResource;
using resources::ShaderResource;

// Font instance represents an individual font of a particular size
Font::Font()
{
}

Font::Font(FontOptions::FontParams params,
	std::vector<float> charWidths,
	std::weak_ptr<TextureResource> texture,
	std::weak_ptr<ShaderResource> shader) :
	_params(params),
	_charWidths(charWidths),
	_texture(texture),
	_shader(shader)
{
}

std::optional<std::unique_ptr<Font>> Font::Load(FontOptions::FontSize size,
	std::weak_ptr<TextureResource> texture,
	std::weak_ptr<ShaderResource> shader)
{
	auto datafilename = GetFontFilename(size);

	std::ifstream inputFile;
	inputFile.open(datafilename.c_str());

	if (!inputFile)
	{
		std::cout << "Error opening file " << datafilename << "!\n";

		inputFile.close();

		return std::nullopt;
	}

	// Declare a tempory holder for the lines to be read (of type string)
	int linenum = 0;
	unsigned int param1;
	float param2;
	unsigned int gridSize = 1;
	unsigned int spaceChar = 0;
	unsigned int degreeChar = 0;
	float charHeight = 16.f;
	std::vector<float> charWidths;

	std::string line;
	unsigned int texWidth = 0;
	unsigned int texHeight = 0;

	auto textureResource = texture.lock();
	if (textureResource)
	{
		texWidth = textureResource->Width();
		texHeight = textureResource->Height();
	}

	while (std::getline(inputFile, line))
	{
		linenum++;

		if (linenum > 4)
			break;

		// Parse the line from the file and
		// set appropriate parameters
		std::stringstream ss(line);

		switch (linenum)
		{
		case 1:
			while (ss >> param2)
			{
				charWidths.push_back(param2 + 10.0f);
			}
			break;
		case 2:
			if (ss >> param2)
				charHeight = param2;

			break;
		case 3:
			if (ss >> param1)
				gridSize = param1;

			break;
		case 4:
			if (ss >> param1)
				spaceChar = param1;

			break;
		case 5:
			if (ss >> param1)
				degreeChar = param1;

			break;
		}
	}

	auto numWidth = (unsigned int)(((double)texWidth) / ((double)gridSize));
	auto numHeight = (unsigned int)(((double)texHeight) / ((double)gridSize));

	auto numChars = charWidths.size();
	if (numChars > 0)
	{
		float totalwidth = 0.0f;

		for (auto width : charWidths)
		{
			totalwidth += width;
		}

		if (spaceChar < numChars)
			charWidths[spaceChar] = (totalwidth / ((float)numChars)) - 4.0f;

		FontOptions::FontParams fontParams = { numWidth, numHeight, gridSize, charHeight, spaceChar, degreeChar, size };
		return std::make_unique<Font>(fontParams, charWidths, texture, shader);
	}

	return std::nullopt;
}

std::string Font::GetFontName(FontOptions::FontSize size)
{
	std::string fontname("");

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

GLuint Font::InitVertexArray(const std::string& str, GLenum usage, GLuint* outPosBuffer, GLuint* outUvBuffer)
{
	if (0 == _params.NumWidth)
		return 0;

	if (0 == _params.NumHeight)
		return 0;

	auto texWidth = 1U;
	auto texHeight = 1U;
	auto textureResource = _texture.lock();
	if (textureResource)
	{
		texWidth = textureResource->Width();
		texHeight = textureResource->Height();
	}

	auto du = (float)_params.GridSize / (float)texWidth;
	auto dv = (float)_params.GridSize / (float)texHeight;

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint vbo[2];
	glGenBuffers(2, vbo);

	auto numChars = str.size();
	auto numVerts = numChars * 6;
	std::vector<GLfloat> pos(numVerts * 5);
	std::vector<GLfloat> uv(numVerts * 5);

	auto currentPos = 0.0f;

	for (unsigned int i = 0; i < numChars; i++)
	{
		currentPos = FillPosUv(pos, uv, i, currentPos, str[i], du, dv);
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
	auto texture = _texture.lock();
	auto shader = _shader.lock();

	if (!texture || !shader)
		return;

	glUseProgram(shader->GetId());
	shader->SetUniforms(ctx);

 	glBindVertexArray(vertexArray);
	glBindTexture(GL_TEXTURE_2D, texture->GetId());

	glDrawArrays(GL_TRIANGLES, 0, numChars * 6);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

float Font::MeasureString(const std::string& str) const
{
	const char* chars = str.c_str();
	unsigned int len = (unsigned int)strlen(chars);

	float totallength = 0.0f;
	int charnum;

	for (unsigned int counter1 = 0; counter1<len; counter1++)
	{
		charnum = GetCharNum(chars[counter1]);

		if (charnum >= 0)
			totallength += (float)((int)(_charWidths[charnum] + 0.5f));
	}

	return totallength;
}

float Font::GetHeight() const
{
	return _params.CharHeight;
}

int Font::GetCharNum(char c) const
{
	if (c == 32)
		return (int)_params.SpaceChar;
	else if (c == 176)
		return (int)_params.DegreeChar;

	int charnum = ((int)c) - (int)Font::StartChar;

	if ((charnum >= 0) && (charnum < (int)(_params.NumWidth * _params.NumHeight)) && (charnum < (int)MaxChars))
		return charnum;

	return (int)_params.SpaceChar;
}

float Font::FillPosUv(std::vector<GLfloat>& pos,
	std::vector<GLfloat>& uv,
	unsigned int index,
	float xOffset,
	char c,
	float du,
	float dv) const
{
	if (0 == _params.NumWidth)
		return xOffset;

	if (0 == _params.NumHeight)
		return xOffset;

	float scal = 1.0f;
	float z = 0.0f;

	auto charNum = GetCharNum(c);
	auto charWidth = (float)_params.GridSize;
	if (charNum >= 0)
		charWidth = (float)((int)(_charWidths[charNum] + 0.5f));

	int posStartIndex = index * 3 * 6;
	int uvStartIndex = index * 2 * 6;
	int row = charNum / _params.NumWidth;
	int col = charNum % _params.NumWidth;

	// Position
	pos[posStartIndex + 0] = scal * xOffset; // x1
	pos[posStartIndex + 1] = scal * 0.0f; // y1
	pos[posStartIndex + 2] = scal * z; // z

	pos[posStartIndex + 3] = scal * xOffset; // x1
	pos[posStartIndex + 4] = scal * _params.GridSize; // y2
	pos[posStartIndex + 5] = scal * z; // z

	pos[posStartIndex + 6] = scal * (xOffset + _params.GridSize); // x2
	pos[posStartIndex + 7] = scal * 0.0f; // y1
	pos[posStartIndex + 8] = scal * z; // z

	pos[posStartIndex + 9] = scal * (xOffset + _params.GridSize); // x2
	pos[posStartIndex + 10] = scal * 0.0f; // y1
	pos[posStartIndex + 11] = scal * z; // z

	pos[posStartIndex + 12] = scal * xOffset; // x1
	pos[posStartIndex + 13] = scal * _params.GridSize; // y2
	pos[posStartIndex + 14] = scal * z; // z

	pos[posStartIndex + 15] = scal * (xOffset + _params.GridSize); // x2
	pos[posStartIndex + 16] = scal * _params.GridSize; // y2
	pos[posStartIndex + 17] = scal * z; // z

	// Texture coords
	uv[uvStartIndex + 0] = col * du; // u1
	uv[uvStartIndex + 1] = 1.0f - (row + 1) * dv; // v2
	
	uv[uvStartIndex + 2] = col * du; // u1
	uv[uvStartIndex + 3] = 1.0f - row * dv; // v1
	
	uv[uvStartIndex + 4] = (col + 1) * du; // u2
	uv[uvStartIndex + 5] = 1.0f - (row + 1) * dv; // v2
	
	uv[uvStartIndex + 6] = (col + 1) * du; // u2
	uv[uvStartIndex + 7] = 1.0f - (row + 1) * dv; // v2
	
	uv[uvStartIndex + 8] = col * du; // u1
	uv[uvStartIndex + 9] = 1.0f - row * dv; // v1

	uv[uvStartIndex + 10] = (col + 1) * du; // u2
	uv[uvStartIndex + 11] = 1.0f - row * dv; // v1

	return xOffset + charWidth;
}

std::string Font::GetFontFilename(FontOptions::FontSize size)
{
	auto fontName = GetFontName(size);

	std::stringstream datafilenamess;
	datafilenamess << "./Resources/Fonts/" << fontName << ".txt";

	return datafilenamess.str();
}
