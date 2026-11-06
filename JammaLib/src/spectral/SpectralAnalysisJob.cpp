#include "SpectralAnalysisJob.h"

#include <new>
#include <utility>

using namespace spectral;

SpectralAnalysisJob::SpectralAnalysisJob(
	std::shared_ptr<const SpectralSource> source, SpectralTransformConfig transform) :
	Source(std::move(source)), Transform(transform)
{
}

void SpectralAnalysisJob::Cancel() noexcept
{
	auto expected = State::Pending;
	if (_state.compare_exchange_strong(expected, State::Cancelled,
		std::memory_order_acq_rel, std::memory_order_acquire))
		_stop.request_stop();
}

std::shared_ptr<const SpectralAnalysis> SpectralAnalysisJob::Result() const noexcept
{
	return Status() == State::Complete ? _result : nullptr;
}

std::string SpectralAnalysisJob::Reason() const
{
	return Status() == State::Failed ? _reason : std::string{};
}

std::shared_ptr<const SpectralPlaybackSnapshot> SpectralAnalysisJob::Playback() const noexcept
{
	return Status() == State::Complete ? _playback : nullptr;
}

std::shared_ptr<const SpectralEditState> SpectralAnalysisJob::EditState() const noexcept
{
	return Status() == State::Complete ? _editState : nullptr;
}

void SpectralAnalysisJob::Run()
{
	if (Status() != State::Pending) return;
	try
	{
		_result = SpectralDsp::Analyze(Source, Transform, _reason, _stop.get_token());
		if (_result)
		{
			auto gains = SpectralGainField::Unity(_result->Geometry.FrameCount,
				_result->Geometry.BinCount, _reason);
			if (gains)
				_editState = SpectralEditState::Create(Source, Transform,
					std::make_shared<const SpectralGainField>(std::move(*gains)), nullptr,
					1u, 0u, 0u, _reason);
			if (_editState)
			{
				auto pcm = std::make_shared<SpectralPcm>();
				pcm->Source = Source;
				_playback = SpectralPlaybackSnapshot::Create(std::move(pcm), nullptr,
					1u, 0u, 0u, _reason, _stop.get_token());
			}
		}
	}
	catch (const std::bad_alloc&)
	{
		_reason = "Not enough memory to analyze the audio source";
	}
	if (_stop.stop_requested())
	{
		Cancel();
		return;
	}
	if ((!_result || !_playback || !_editState) && _reason.empty()) _reason = "Audio source analysis is unavailable";
	auto expected = State::Pending;
	_state.compare_exchange_strong(expected, _result && _playback && _editState ? State::Complete : State::Failed,
		std::memory_order_release, std::memory_order_relaxed);
	// If cancellation won, no reader receives this result. Ownership and any
	// discarded coefficient storage are released on this worker/UI boundary.
}
