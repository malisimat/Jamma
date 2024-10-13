///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "MathUtils.h"

unsigned int utils::ModNeg(int v, unsigned int len)
{
	return (v % len + len) % len;
}

unsigned long utils::ModNeg(int v, unsigned long len)
{
	return (v % len + len) % len;
}
