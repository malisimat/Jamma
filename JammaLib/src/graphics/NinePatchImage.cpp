#include "NinePatchImage.h"
#include <algorithm>

using namespace graphics;
using namespace utils;

std::optional<NinePatchImage::BorderInfo> NinePatchImage::DetectBorder(
	std::vector<unsigned char>& pixels,
	unsigned int width,
	unsigned int height)
{
	if ((width == 0) || (height == 0))
		return std::nullopt;

	const auto numPixels = width * height;
	if (pixels.size() < static_cast<size_t>(numPixels) * NumChannels)
		return std::nullopt;

	for (auto pixelIndex = 0u; pixelIndex < numPixels; ++pixelIndex)
	{
		const auto px = pixelIndex * NumChannels;
		const auto blue = pixels[px + 0];
		const auto green = pixels[px + 1];
		const auto red = pixels[px + 2];
		const auto alpha = pixels[px + 3];

		if ((blue == 255) && (green == 0) && (red == 255) && (alpha == 255))
		{
			const auto x = pixelIndex % width;
			const auto y = pixelIndex / width;

			auto replacementX = x;
			auto replacementY = y;

			if (x + 1u < width)
				replacementX = x + 1u;
			else if (x > 0u)
				replacementX = x - 1u;
			else if (y + 1u < height)
				replacementY = y + 1u;
			else if (y > 0u)
				replacementY = y - 1u;

			// 1x1 textures have no adjacent pixel to copy from, so we keep the same pixel value.
			const auto replacementPixelIndex = replacementY * width + replacementX;
			const auto replacementPx = replacementPixelIndex * NumChannels;
			for (auto i = 0; i < NumChannels; ++i)
				pixels[px + i] = pixels[replacementPx + i];

			return BorderInfo{
				x,
				y
			};
		}
	}

	return std::nullopt;
}

std::array<GLfloat, 162> NinePatchImage::BuildPositions(
	unsigned int borderX,
	unsigned int borderY,
	Size2d size)
{
	const auto xBounds = BuildBounds(borderX, size.Width);
	const auto yBounds = BuildBounds(borderY, size.Height);
	std::array<GLfloat, 162> positions = {};
	auto outIndex = 0u;

	for (auto row = 0; row < CellsPerAxis; ++row)
	{
		for (auto col = 0; col < CellsPerAxis; ++col)
		{
			const auto x0 = static_cast<GLfloat>(xBounds[col]);
			const auto x1 = static_cast<GLfloat>(xBounds[col + 1]);
			const auto y0 = static_cast<GLfloat>(yBounds[row]);
			const auto y1 = static_cast<GLfloat>(yBounds[row + 1]);

			const GLfloat verts[VertsPerCell * FloatsPerVertex] = {
				x0, y0, 0.0f,
				x1, y0, 0.0f,
				x0, y1, 0.0f,
				x0, y1, 0.0f,
				x1, y0, 0.0f,
				x1, y1, 0.0f
			};

			for (auto v : verts)
				positions[outIndex++] = v;
		}
	}

	return positions;
}

std::array<unsigned int, 4> NinePatchImage::BuildBounds(unsigned int border, unsigned int extent)
{
	const auto left = (std::min)(border, extent);
	const auto right = extent > left ? (std::max)(left, extent - left) : left;
	return { 0u, left, right, extent };
}
