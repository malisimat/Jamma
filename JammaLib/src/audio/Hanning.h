#pragma once

#include <array>
#include <tuple>
#include <math.h>
#include "../include/Constants.h"

namespace audio
{
	class Hanning
	{
	public:
		Hanning(unsigned int size);

	public:
		std::tuple<float, float> operator[](unsigned int i) const;
		float Mix(float fadeInSamp, float fadeOutSamp, unsigned int i) const;

		unsigned int Size() const;
		void SetSize(unsigned int size);

	protected:
		float Calc(unsigned int index, unsigned int size) const;

	protected:
		unsigned int _size;
		std::array<float, constants::MaxLoopFadeSamps> _fadeIn;
		std::array<float, constants::MaxLoopFadeSamps> _fadeOut;
	};
}
