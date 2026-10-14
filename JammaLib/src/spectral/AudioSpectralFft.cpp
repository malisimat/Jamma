#include "AudioSpectralFft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <kissfft/kiss_fftr.h>

using namespace spectral;

// The installed header defaults to float; reject conflicting build definitions.
static_assert(std::is_same_v<kiss_fft_scalar, float>);

struct AudioSpectralFft::State
{
	struct PlanDeleter
	{
		void operator()(kiss_fftr_state* plan) const noexcept { kiss_fftr_free(plan); }
	};
	using Plan = std::unique_ptr<kiss_fftr_state, PlanDeleter>;

	explicit State(unsigned int size) :
		WindowSize(size), Normalization(1.0f / static_cast<float>(size)),
		ForwardPlan(kiss_fftr_alloc(static_cast<int>(size), 0, nullptr, nullptr)),
		InversePlan(kiss_fftr_alloc(static_cast<int>(size), 1, nullptr, nullptr)),
		Samples(size), Bins(size / 2u + 1u)
	{
	}

	unsigned int WindowSize;
	float Normalization;
	Plan ForwardPlan;
	Plan InversePlan;
	std::vector<float> Samples;
	std::vector<kiss_fft_cpx> Bins;
};

AudioSpectralFft::AudioSpectralFft(unsigned int windowSize)
{
	// kiss_fftr_alloc halves N and computes (N/2)*3 using signed int.
	if (windowSize < 4u || (windowSize & (windowSize - 1u)) != 0u
		|| windowSize > static_cast<unsigned int>((std::numeric_limits<int>::max)() / 3) * 2u)
		return;
	try
	{
		auto state = std::make_unique<State>(windowSize);
		if (state->ForwardPlan && state->InversePlan) _state = std::move(state);
	}
	catch (const std::bad_alloc&)
	{
		// All partially prepared plans/scratch are released here, off callback.
	}
}

AudioSpectralFft::~AudioSpectralFft() = default;

bool AudioSpectralFft::IsValid() const noexcept
{
	return _state != nullptr;
}

float AudioSpectralFft::InverseNormalization() const noexcept
{
	return _state ? _state->Normalization : 0.0f;
}

void AudioSpectralFft::Forward(std::span<const float> samples, std::span<Bin> bins)
{
	if (!_state || samples.size() != _state->Samples.size() || bins.size() != _state->Bins.size())
		throw std::invalid_argument("Audio spectral FFT forward requires a valid plan and exact spans");
	for (const auto sample : samples)
		if (!std::isfinite(sample))
			throw std::invalid_argument("Audio spectral FFT input must be finite");

	// Copy native coefficients explicitly; Bin and kiss_fft_cpx are distinct types.
	std::copy(samples.begin(), samples.end(), _state->Samples.begin());
	kiss_fftr(_state->ForwardPlan.get(), _state->Samples.data(), _state->Bins.data());
	for (const auto& bin : _state->Bins)
		if (!std::isfinite(bin.r) || !std::isfinite(bin.i))
			throw std::overflow_error("Audio spectral FFT forward exceeded finite float range");
	for (std::size_t i = 0; i < bins.size(); ++i)
		bins[i] = { _state->Bins[i].r, _state->Bins[i].i };
}

void AudioSpectralFft::Inverse(std::span<const Bin> bins, std::span<float> samples)
{
	if (!_state || samples.size() != _state->Samples.size() || bins.size() != _state->Bins.size())
		throw std::invalid_argument("Audio spectral FFT inverse requires a valid plan and exact spans");
	for (const auto& bin : bins)
		if (!std::isfinite(bin.Real) || !std::isfinite(bin.Imaginary))
			throw std::invalid_argument("Audio spectral FFT coefficients must be finite");
	if (bins.front().Imaginary != 0.0f || bins.back().Imaginary != 0.0f)
		throw std::invalid_argument("Audio spectral FFT DC and Nyquist must be real");

	for (std::size_t i = 0; i < bins.size(); ++i)
		_state->Bins[i] = { bins[i].Real, bins[i].Imaginary };
	kiss_fftri(_state->InversePlan.get(), _state->Bins.data(), _state->Samples.data());
	for (auto& sample : _state->Samples)
	{
		sample *= _state->Normalization;
		if (!std::isfinite(sample))
			throw std::overflow_error("Audio spectral FFT inverse exceeded finite float range");
	}
	std::copy(_state->Samples.begin(), _state->Samples.end(), samples.begin());
}
