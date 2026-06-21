#pragma once

#include <array>
#include <optional>
#include <vector>
#include <gl/glew.h>
#include "../utils/CommonTypes.h"

namespace graphics
{
	class NinePatchImage
	{
	public:
		static constexpr int VertexCount = 54;
		static constexpr int FloatsPerVertex = 3;
		static constexpr int UvFloatsPerVertex = 2;
		static constexpr int VertsPerCell = 6;
		static constexpr int CellsPerAxis = 3;

		struct BorderInfo
		{
			unsigned int borderX;
			unsigned int borderY;
		};

		static std::optional<BorderInfo> DetectBorder(
			std::vector<unsigned char>& pixels,
			unsigned int width,
			unsigned int height);

		static std::array<GLfloat, 162> BuildPositions(
			unsigned int borderX,
			unsigned int borderY,
			utils::Size2d size);
		static std::array<unsigned int, 4> BuildBounds(unsigned int border, unsigned int extent);

	private:
		static constexpr int NumChannels = 4;
	};
}
