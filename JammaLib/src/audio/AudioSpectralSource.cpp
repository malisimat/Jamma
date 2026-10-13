#include "AudioSpectralSource.h"

#include <algorithm>
#include <cmath>
#include <new>

using namespace audio;

bool AudioSpectralSourceLayout::IsValid() const noexcept
{
	return LogicalLength > 0ul && LogicalLength <= PhysicalLength
		&& PhysicalLength <= constants::MaxLoopBufferSize - BodyOffset
		&& FadeSamps <= BodyOffset && std::isfinite(SampleRate) && SampleRate > 0.0f;
}

unsigned long AudioSpectralSourceLayout::WrappedBodyIndex(unsigned long storageIndex) const noexcept
{
	if (LogicalLength == 0ul) return 0ul;
	const auto length = static_cast<long long>(LogicalLength);
	auto index = (static_cast<long long>(storageIndex) - BodyOffset) % length;
	if (index < 0) index += length;
	return static_cast<unsigned long>(index);
}

AudioSpectralSourceCapture::AudioSpectralSourceCapture(
	AudioSpectralSourceLayout layout, std::uint64_t generation,
	std::shared_ptr<const AudioSpectralSourceIdentity> identity,
    std::shared_ptr<const AudioSpectralResourceBudget::Reservation> resources, AudioSpectralTransformConfig transform) :
	_source(std::make_shared<AudioSpectralSource>(AudioSpectralSource{
		std::move(identity), generation, layout, std::move(resources), {}, transform }))
{
}

void AudioSpectralSourceCapture::Prepare()
{
	if (Status() != State::Preparing) return;
	try
	{
		_source->Samples.resize(_source->Layout.StoredSampleCount());
	}
	catch (const std::bad_alloc&)
	{
		auto expected = State::Preparing;
		_state.compare_exchange_strong(expected, State::Failed,
			std::memory_order_release, std::memory_order_relaxed);
		return;
	}
	auto expected = State::Preparing;
	_state.compare_exchange_strong(expected, State::Pending,
		std::memory_order_release, std::memory_order_relaxed);
}

void AudioSpectralSourceCapture::Cancel() noexcept
{
	// Preparation can finish while cancellation runs; at most one retry is needed.
	auto expected = State::Preparing;
	if (!_state.compare_exchange_strong(expected, State::Cancelled,
		std::memory_order_acq_rel, std::memory_order_acquire) && expected == State::Pending)
		_state.compare_exchange_strong(expected, State::Cancelled,
			std::memory_order_acq_rel, std::memory_order_acquire);
}

std::shared_ptr<const AudioSpectralSource> AudioSpectralSourceCapture::Source() const noexcept
{
	return Status() == State::Complete ? _source : nullptr;
}

void AudioSpectralSourceCapture::CopyAtAudioBoundary(const BufferBank& buffer,
	const AudioSpectralSourceLayout& layout, std::uint64_t generation, bool eligible) noexcept
{
	const auto state = Status();
	if (state != State::Preparing && state != State::Pending) return;
	if (!eligible || generation != _source->Generation || layout != _source->Layout
		|| buffer.Length() != layout.StoredSampleCount())
	{
		Cancel();
		return;
	}

	if (state == State::Preparing) return; // No access to worker-owned PCM topology.

	// At most CopySamplesPerBlock samples, split only at fixed bank boundaries.
	// No other thread reads this destination until the release publication below.
	const auto end = (std::min)(layout.StoredSampleCount(), _copied + CopySamplesPerBlock);
	while (_copied < end)
	{
		const auto count = (std::min)(end - _copied,
			BufferBank::_BufferBankSize - _copied % BufferBank::_BufferBankSize);
		const auto* samples = buffer.BlockPtr(_copied);
		if (!samples)
		{
			Cancel();
			return;
		}
		std::copy_n(samples, count, _source->Samples.data() + _copied);
		_copied += count;
	}
	if (_copied == layout.StoredSampleCount())
	{
		auto expected = State::Pending;
		_state.compare_exchange_strong(expected, State::Complete,
			std::memory_order_release, std::memory_order_relaxed);
	}
}
