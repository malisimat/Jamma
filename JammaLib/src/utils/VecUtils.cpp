///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////


#include "VecUtils.h"


unsigned int utils::VecToId(std::vector<unsigned int> vec)
{
	unsigned int id = 0u;
	auto len = vec.size();

	for (auto i = 0u; i < 3; i++)
	{
		auto v = i < len ? (vec[i] & 0xFF) : 0xFF;
		id += v << (8 * i);
	}

	return id;
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