#include "AudioSpectralReconstructionJob.h"

#include <new>
#include <utility>

using namespace audio;

AudioSpectralReconstructionJob::AudioSpectralReconstructionJob(
	std::shared_ptr<const AudioSpectralAnalysis> analysis,
	std::shared_ptr<const AudioSpectralGainField> gains, std::uint64_t spectralRevision) :
	Analysis(std::move(analysis)), Gains(std::move(gains)), SpectralRevision(spectralRevision)
{
}

void AudioSpectralReconstructionJob::Cancel() noexcept
{
	auto expected = State::Pending;
	if (_state.compare_exchange_strong(expected, State::Cancelled,
		std::memory_order_acq_rel, std::memory_order_acquire))
		_stop.request_stop();
}

std::shared_ptr<const AudioSpectralPcm> AudioSpectralReconstructionJob::Result() const noexcept
{
	return Status() == State::Complete ? _result : nullptr;
}

std::string AudioSpectralReconstructionJob::Reason() const
{
	return Status() == State::Failed ? _reason : std::string{};
}

void AudioSpectralReconstructionJob::Run()
{
	if (Status() != State::Pending) return;
	try
	{
		if (!Analysis || !Gains)
			_reason = "Spectral reconstruction has no analysis or gain field";
		else
			_result = AudioSpectralDsp::Reconstruct(Analysis, *Gains, _reason, _stop.get_token());
	}
	catch (const std::bad_alloc&)
	{
		_reason = "Not enough memory to reconstruct the audio source";
	}
	if (_stop.stop_requested())
	{
		Cancel();
		return;
	}
	if (!_result && _reason.empty()) _reason = "Spectral reconstruction is unavailable";
	auto expected = State::Pending;
	_state.compare_exchange_strong(expected, _result ? State::Complete : State::Failed,
		std::memory_order_release, std::memory_order_relaxed);
	// Cancellation winning this CAS prevents late PCM from escaping. All PCM
	// ownership and destruction remain on non-audio owners.
}
