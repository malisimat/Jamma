///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <cmath>

namespace utils
{
	unsigned int ModNeg(int v, unsigned int len);
	unsigned long ModNeg(int v, unsigned long len);

	inline double NormalizeLoopFraction(double fraction) noexcept
	{
		if (!std::isfinite(fraction))
			return 0.0;
		if (fraction == 1.0)
			return 1.0;

		fraction = std::fmod(fraction, 1.0);
		return fraction < 0.0 ? fraction + 1.0 : fraction;
	}
}
