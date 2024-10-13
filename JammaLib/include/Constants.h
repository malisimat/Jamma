///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#define MAJOR_VERSION       5
#define MINOR_VERSION       0
#define PATCH_VERSION       2

#define QUOTE_(x) #x
#define QUOTE(x) QUOTE_(x)
#define LIB_VERSION			QUOTE(MAJOR_VERSION.MINOR_VERSION.PATCH_VERSION)

namespace constants
{
	constexpr auto TWOPI = 6.283185307179586476925286766559;
	const unsigned int MaxBlockSize = 4096u;
	const unsigned int MaxLoopFadeSamps = 70000u;
	const unsigned long MaxLoopBufferSize = 40000000ul;
	const unsigned int GrainSamps = 300u;
}
