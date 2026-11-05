#include "ImageUtils.h"
#include "StringUtils.h"

using namespace utils;

std::optional<std::tuple<std::vector<unsigned char>, unsigned int, unsigned int>> ImageUtils::LoadTga(const std::string& fileName)
{
	FILE* file;
	auto res = _wfopen_s(&file, utils::DecodeUtf8(fileName).c_str(), L"rb");

	if (!file)
		return std::nullopt;

	// Read the header of the TGA, compare it with the known headers for compressed and uncompressed TGAs
	unsigned char header[18];
	fread(header, sizeof(unsigned char) * 18, 1, file);

	while (header[0] > 0)
	{
		--header[0];

		unsigned char temp;
		fread(&temp, sizeof(unsigned char), 1, file);
	}

	unsigned int width = header[13] * 256 + header[12];
	unsigned int height = header[15] * 256 + header[14];
	unsigned int bpp = header[16] / 8;

	// Check if parameters are valid
	if ((width <= 0) || (height <= 0) || ((bpp != 1) && (bpp != 3) && (bpp != 4)))
	{
		fclose(file);
		return std::nullopt;
	}

	auto pixels = std::vector<unsigned char>(width * height * 4);

	if (header[2] == 2)
	{
		unsigned char pixel[4] = { 255, 255, 255, 255 };
		const unsigned int numPixels = width * height;

		for (unsigned int i = 0; i < numPixels; ++i)
		{
			fread(pixel, sizeof(unsigned char) * bpp, 1, file);

			if (bpp == 1)
			{
				pixel[1] = pixel[0];
				pixel[2] = pixel[0];
				pixel[3] = pixel[0];
			}

			pixels[(i * 4) + 0] = pixel[0];
			pixels[(i * 4) + 1] = pixel[1];
			pixels[(i * 4) + 2] = pixel[2];
			pixels[(i * 4) + 3] = pixel[3];
		}
	}
	else
	{
		fclose(file);
		return std::nullopt;
	}

	fclose(file);
	return std::tuple<std::vector<unsigned char>, unsigned int, unsigned int>(pixels, width, height);
}
