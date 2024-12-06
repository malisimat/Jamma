///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////


#include "VecUtils.h"


unsigned int utils::VecToId(std::vector<unsigned int> vec)
{
	return (vec[0] & 0xFF) +
		(((vec[1] & 0xFF) << 8) & 0xFF00) +
		(((vec[2] & 0xFF) << 16) & 0xFF0000);
}

std::vector<unsigned char> utils::IdToVec(unsigned int id)
{
	std::vector<unsigned char> vec;
	unsigned char mask = 0xFF;
	vec.push_back((id >> 16) & mask);
	vec.push_back((id >> 8) & mask);
	vec.push_back(id & mask);

	return vec;
}