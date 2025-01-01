///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>

namespace utils
{
	unsigned int VecToId(std::vector<unsigned int> vec);
	std::vector<unsigned char> IdToVec(unsigned int i);
}
