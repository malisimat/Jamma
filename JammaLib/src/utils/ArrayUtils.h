///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>
#include <algorithm> 

namespace utils
{
	template <typename T>
	T ArrayMin(std::vector<T> arr)
	{
		if (arr.empty())
			return T();

		T curMin = arr[0];
		for (auto v : arr)
		{
			if (v < curMin)
				curMin = v;
		}

		return curMin;
	}

	template <typename T>
	T ArraySubMin(std::vector<T> arr, unsigned int i1, unsigned int i2)
	{
		if (arr.empty())
			return T();

		auto arrSize = (unsigned int)arr.size();
		i1 = i1 > 0 ? i1 : 0;
		i1 = i1 < arrSize ? i1 : arrSize;
		i2 = i2 < arrSize ? i2 : arrSize;

		if (i2 <= i1)
			return T();

		T curMin = arr[0];
		for (auto i = i1; i < i2; i++)
		{
			if (arr[i] < curMin)
				curMin = arr[i];
		}

		return curMin;
	}

	template <typename T>
	T ArrayMax(std::vector<T> arr)
	{
		if (arr.empty())
			return T();

		T curMax = arr[0];
		for (auto v : arr)
		{
			if (v > curMax)
				curMax = v;
		}

		return curMax;
	}

	template <typename T>
	T ArraySubMax(std::vector<T> arr, unsigned int i1, unsigned int i2)
	{
		if (arr.empty())
			return T();

		auto arrSize = (unsigned int)arr.size();
		i1 = i1 > 0 ? i1 : 0;
		i1 = i1 < arrSize ? i1 : arrSize;
		i2 = i2 < arrSize ? i2 : arrSize;

		if (i2 <= i1)
			return T();

		T curMax = arr[0];
		for (auto i = i1; i < i2; i++)
		{
			if (arr[i] > curMax)
				curMax = arr[i];
		}

		return curMax;
	}
}
