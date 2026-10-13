#include "AudioSpectralDisplayJob.h"

#include <new>
#include <utility>

using namespace editor;

AudioSpectralDisplayJob::AudioSpectralDisplayJob(
	std::shared_ptr<const audio::AudioSpectralAnalysis> analysis,
	std::shared_ptr<const audio::AudioSpectralEditState> editState,
	unsigned int timeSegments, unsigned int frequencySegments,
	graphics::AudioSpectralViewport viewport, std::uint64_t presentationRevision) :
	Analysis(std::move(analysis)), EditState(std::move(editState)),
	TimeSegments(timeSegments), FrequencySegments(frequencySegments), Viewport(viewport), PresentationRevision(presentationRevision)
{
}

void AudioSpectralDisplayJob::Cancel() noexcept
{
	auto expected = State::Pending;
	if (_state.compare_exchange_strong(expected, State::Cancelled,
		std::memory_order_acq_rel, std::memory_order_acquire))
		_stop.request_stop();
}

std::shared_ptr<const graphics::AudioSpectralDisplay> AudioSpectralDisplayJob::Result() const noexcept
{
	return Status() == State::Complete ? _result : nullptr;
}

std::string AudioSpectralDisplayJob::Reason() const
{
	return Status() == State::Failed ? _reason : std::string{};
}

void AudioSpectralDisplayJob::Run()
{
	if (Status() != State::Pending) return;
	try
	{
		if (!Analysis || !EditState || !Analysis->Source || !EditState->Source || !EditState->Gains
			|| Analysis->Source->Identity != EditState->Source->Identity
			|| Analysis->Source->Generation != EditState->Source->Generation
			|| Analysis->Source->Layout != EditState->Source->Layout
			|| Analysis->Transform != EditState->Transform)
			_reason = "Spectral display analysis does not match its committed edit source";
		else
			_result = graphics::AudioSpectralModel::BuildDisplay(*Analysis, EditState->Gains.get(),
				EditState->Envelope.get(), EditState->Revision, TimeSegments, FrequencySegments,
				_reason, _stop.get_token(), Viewport, PresentationRevision);
	}
	catch (const std::bad_alloc&)
	{
		_reason = "Not enough memory for the spectral display";
	}
	if (_stop.stop_requested())
	{
		Cancel();
		return;
	}
	if (!_result && _reason.empty()) _reason = "Spectral display is unavailable";
	auto expected = State::Pending;
	_state.compare_exchange_strong(expected, _result ? State::Complete : State::Failed,
		std::memory_order_release, std::memory_order_relaxed);
	// Cancellation winning the publication prevents obsolete display data from
	// escaping. Captured analysis/edit handles remain alive until this job retires.
}
