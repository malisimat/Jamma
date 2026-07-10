///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "MathUtils.h"

unsigned int utils::ModNeg(int v, unsigned int len)
{
	// Do the modulo in signed arithmetic. If v (int) were combined directly
	// with an unsigned len, the usual arithmetic conversions would convert v
	// to unsigned FIRST (wrapping a negative v to ~2^32+v) before the modulo
	// ran, which only agrees with the intended signed-modulo idiom when len
	// happens to divide 2^32 evenly (i.e. len is a power of 2) -- for any
	// other len (e.g. a NINJAM interval length) it silently produces a wrong,
	// non-obvious result.
	const auto l = static_cast<int>(len);
	return static_cast<unsigned int>(((v % l) + l) % l);
}

unsigned long utils::ModNeg(int v, unsigned long len)
{
	const auto l = static_cast<long>(len);
	return static_cast<unsigned long>(((v % l) + l) % l);
}
