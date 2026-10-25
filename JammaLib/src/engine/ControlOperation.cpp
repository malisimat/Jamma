#include "ControlOperation.h"
#include "../io/IoInputSubsystem.h"

using namespace engine;

void ControlOperation::RequestBoundary(io::IoInputSubsystem& input, std::uint64_t revision)
{
	_revision = revision;
	if (!input.RequestCloseRigTriggerInputFromUi(_revision)) return;
	_inputClosed = true;
	_admissionPending.store(true, std::memory_order_release);
	++_identity;
	// The job owner acknowledges on its next turn; no wait under a caller lock.
}

bool ControlOperation::TryAcquireBoundary(io::IoInputSubsystem& input)
{
	if (!input.RigTriggerInputReadyForAudioBoundary(_revision)) return false;
	_writer = std::unique_lock<std::mutex>(_lifecycleMutex, std::try_to_lock);
	return _writer.owns_lock();
}

bool ControlOperation::PauseAudio(audio::AudioHost& audio, std::string& error)
{
	if (!audio.BeginQuiescence(_pause, error)) return false;
	_paused = true;
	return true;
}

ControlOperation::AudioRestoration ControlOperation::RestoreAudio(audio::AudioHost& audio, std::string& error)
{
	const bool success = !_paused || audio.EndQuiescence(_pause, error);
	const bool admissionRestored = _paused && success && _pause.AdmissionWasOpen;
	_paused = false;
	return {success, admissionRestored};
}

void ControlOperation::Finish(io::IoInputSubsystem& input, std::optional<std::uint64_t> acceptedRevision)
{
	if (_inputClosed && acceptedRevision) input.OpenRigTriggerInput(*acceptedRevision);
	_inputClosed = false;
	ReleaseWriter();
	_kind = None;
	_loadReady = false;
	_admissionPending.store(false, std::memory_order_release);
}

void ControlOperation::FinishReplacement()
{
	// Replacement keeps the old input gate closed and abandons its paused stream.
	_paused = _loadReady = false;
	ReleaseWriter();
	_kind = None;
}
