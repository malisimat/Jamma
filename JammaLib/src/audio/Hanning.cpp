#include "Hanning.h"

using namespace audio;

Hanning::Hanning(unsigned int size)
{
	SetSize(size);
}

std::tuple<float, float> Hanning::operator[](unsigned int i) const
{
	if (i >= _size)
		return { 1.0f, 0.0f };

	return { _fadeIn[i], _fadeOut[i] };
}

float Hanning::Mix(float fadeInSamp, float fadeOutSamp, unsigned int i) const
{
	if (i >= _size)
		return fadeInSamp;

	return (_fadeIn[i] * fadeInSamp) + (_fadeOut[i] * fadeOutSamp);
}

unsigned int Hanning::Size() const
{
	return _size;
}

void Hanning::SetSize(unsigned int size)
{
	_size = size > constants::MaxLoopFadeSamps ? constants::MaxLoopFadeSamps : size;

	if (_size > 0)
	{
		for (auto i = 0u; i < _size; i++)
		{
			// fadeOut[i] = 0.5 + 0.5*cos(pi*i/N)       (1 -> 0 over the window)
			// fadeIn[i]  = 0.5 + 0.5*cos(pi*(N-i)/N)   (0 -> 1 over the window)
			//            = 0.5 - 0.5*cos(pi*i/N)
			// Their sum is exactly 1.0 for all i, giving constant-amplitude crossfade.
			_fadeIn[i] = Calc(_size - i, _size);
			_fadeOut[i] = Calc(i, _size);
		}
	}
}

float Hanning::Calc(unsigned int index, unsigned int size) const
{
	if (size == 0)
		return 1.0;

	double scale = constants::TWOPI * 0.5 / (double)size;
	return (float)(0.5 + 0.5 * cos(scale * (double)index));
}
