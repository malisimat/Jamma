#pragma once

#include <memory>
#include <span>

namespace audio
{
	// Worker-owned real float FFT. Construct, transform and destroy off callback.
	// One instance is reusable scratch for one worker, not a concurrent service.
	class AudioSpectralFft
	{
	public:
		struct Bin
		{
			float Real = 0.0f;
			float Imaginary = 0.0f;
		};

		// Power-of-two sizes >= 4 avoid KissFFT's generic-butterfly allocations.
		// Invalid size, allocation failure or plan failure leaves IsValid() false.
		explicit AudioSpectralFft(unsigned int windowSize);
		~AudioSpectralFft();
		AudioSpectralFft(const AudioSpectralFft&) = delete;
		AudioSpectralFft& operator=(const AudioSpectralFft&) = delete;

		bool IsValid() const noexcept;
		unsigned int WindowSize() const noexcept;
		// Forward is unscaled; Inverse applies 1/N exactly once.
		float InverseNormalization() const noexcept;
		// Exact spans: N samples and N/2+1 positive-frequency bins.
		// Invalid state/size/input throws invalid_argument; nonfinite output throws
		// overflow_error. Destinations are unchanged on failure. No silent limiting.
		// Inverse requires real DC/Nyquist (their imaginary parts must be zero).
		void Forward(std::span<const float> samples, std::span<Bin> bins);
		void Inverse(std::span<const Bin> bins, std::span<float> samples);

	private:
		struct State;
		std::unique_ptr<State> _state;
	};
}
