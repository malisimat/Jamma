#include "AudioSpectralAnalysisJob.h"

#include <new>
#include <utility>

using namespace audio;

AudioSpectralAnalysisJob::AudioSpectralAnalysisJob(
	std::shared_ptr<const AudioSpectralSource> source, AudioSpectralTransformConfig transform) :
	Source(std::move(source)), Transform(transform)
{
}

void AudioSpectralAnalysisJob::Cancel() noexcept
{
	auto expected = State::Pending;
	if (_state.compare_exchange_strong(expected, State::Cancelled,
		std::memory_order_acq_rel, std::memory_order_acquire))
		_stop.request_stop();
}

std::shared_ptr<const AudioSpectralAnalysis> AudioSpectralAnalysisJob::Result() const noexcept
{
	return Status() == State::Complete ? _result : nullptr;
}

std::string AudioSpectralAnalysisJob::Reason() const
{
	return Status() == State::Failed ? _reason : std::string{};
}

std::shared_ptr<const AudioSpectralPlaybackSnapshot> AudioSpectralAnalysisJob::Playback() const noexcept
{
	return Status() == State::Complete ? _playback : nullptr;
}

std::shared_ptr<const AudioSpectralEditState> AudioSpectralAnalysisJob::EditState() const noexcept
{
	return Status() == State::Complete ? _editState : nullptr;
}

void AudioSpectralAnalysisJob::Run()
{
	if (Status() != State::Pending) return;
	try
	{
		_result = AudioSpectralDsp::Analyze(Source, Transform, _reason, _stop.get_token());
		if (_result)
		{
			auto gains = AudioSpectralGainField::Unity(_result->Geometry.FrameCount,
				_result->Geometry.BinCount, _reason);
			if (gains)
				_editState = AudioSpectralEditState::Create(Source, Transform,
					std::make_shared<const AudioSpectralGainField>(std::move(*gains)), nullptr,
					1u, 0u, 0u, _reason);
			if (_editState)
			{
				auto pcm = std::make_shared<AudioSpectralPcm>();
				pcm->Source = Source;
				_playback = AudioSpectralPlaybackSnapshot::Create(std::move(pcm), nullptr,
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
