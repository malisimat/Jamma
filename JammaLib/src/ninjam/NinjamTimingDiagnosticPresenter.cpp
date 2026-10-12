#include "NinjamTimingDiagnosticPresenter.h"
#include <ostream>
#include "NinjamLoopAlignment.h"

using namespace ninjam;

void NinjamTimingDiagnosticPresenter::Present(const NinjamTimingDiagnostics& diagnostics,
	std::ostream& output)
{
	const auto present = [this, &output](const ninjam::NinjamTimingDiagnosticEvent& event)
	{
		output << "[NINJAM][TimingDiagnostic] reason="
			<< ninjam::NinjamTimingCoordinator::DiagnosticReasonName(event.Reason)
			<< " sessionEpoch=" << event.SessionEpoch
			<< " appliedSessionEpoch=" << event.AppliedSessionEpoch
			<< " desiredVersion=" << event.DesiredVersion
			<< " appliedVersion=" << event.AppliedVersion
			<< " generation=" << event.Generation
			<< " value=" << event.ValueSamps
			<< " limit=" << event.LimitSamps
			<< " occurrences=" << event.OccurrenceCount
			<< " cumulativeSuppressed=" << event.CumulativeSuppressedCount << '\n';
		_lastPresentedSequence = event.Sequence;
	};

	for (auto eventIndex = 0u; eventIndex < diagnostics.CapturedEventCount; ++eventIndex)
	{
		const auto& event = diagnostics.Events[eventIndex];
		if (event.Sequence > _lastPresentedSequence)
			present(event);
	}
	if (diagnostics.LatestEvent.Sequence > _lastPresentedSequence)
		present(diagnostics.LatestEvent);
	if (diagnostics.EventOverflowSummaryCount > _lastPresentedOverflowCount)
	{
		output << "[NINJAM][TimingDiagnostic] overflowSummary="
			<< diagnostics.EventOverflowSummaryCount
			<< " overflowTotal=" << diagnostics.EventOverflowCount << '\n';
		_lastPresentedOverflowCount = diagnostics.EventOverflowSummaryCount;
	}
}

bool NinjamTimingDiagnosticPresenter::PlaybackTraceDue(std::chrono::steady_clock::time_point now) noexcept
{
	if (_lastPlaybackTraceAt && now - *_lastPlaybackTraceAt < std::chrono::milliseconds(500))
		return false;
	_lastPlaybackTraceAt = now;
	return true;
}

void NinjamTimingDiagnosticPresenter::PresentPlayback(const NinjamTiming& timing,
	const NinjamDesiredTimingReceipt& receipt,
	std::span<const NinjamPlaybackTakeDiagnostic> takes, std::ostream& output)
{
	const SyncPhaseMap map{ receipt.SourceLengthSamps, receipt.RemoteLengthSamps,
		receipt.SceneCoordinateSamps, receipt.SourceCoordinateSamps };
	const auto scene = timing.LocalTransport.SceneSamplePos;
	output << "[NINJAM][PlaybackTrace] epoch=" << receipt.SessionEpoch
		<< " version=" << receipt.Version << " generation=" << receipt.Generation
		<< " policy=" << NinjamTimingCoordinator::FollowPolicyName(receipt.Policy)
		<< " remoteValid=" << timing.IsValid
		<< " device=" << timing.DeviceAudioSampleAtObservation
		<< " scene=" << scene;
	if (_lastPlaybackTiming && _lastPlaybackEpoch == receipt.SessionEpoch)
	{
		output << " deviceDelta=" << static_cast<std::int64_t>(timing.DeviceAudioSampleAtObservation)
			- static_cast<std::int64_t>(_lastPlaybackTiming->DeviceAudioSampleAtObservation)
			<< " sceneDelta=" << static_cast<std::int64_t>(scene)
			- static_cast<std::int64_t>(_lastPlaybackTiming->LocalTransport.SceneSamplePos);
	}
	output << " remotePhase=" << timing.IntervalPositionSamps << '/' << timing.IntervalLengthSamps
		<< " bpm=" << timing.Bpm << " bpi=" << timing.Bpi
		<< " clockPhase=" << timing.LocalTransport.MasterPhaseSamps << '/' << timing.LocalTransport.MasterLengthSamps
		<< " map=" << map.SourceLengthSamps << '/' << map.RemoteLengthSamps
		<< " mappedSource=" << map.SourceCoordinateAt(scene)
		<< " appliedScene=" << receipt.SceneCoordinateSamps
		<< " correction=" << receipt.LocalSourceCorrectionSamps
		<< " observationAge=" << timing.ObservationAgeSamps << '\n';
	for (const auto& take : takes)
	{
		output << "[NINJAM][PlaybackTake] station=\"" << take.Station
			<< "\" take=" << take.Take << " midiPhase=" << take.MidiPhaseSamps << '/' << take.MidiLengthSamps
			<< " anchorCorrection=" << take.MidiAnchorCorrectionSamps
			<< " recorded=" << take.RecordedSamps
			<< " restore=" << take.RestoreBeforeSamps << "->" << take.RestoreTargetSamps
			<< " restores=" << take.RestoreCount << " moved=" << take.RestoreChangedCount
			<< " maxBackstep=" << take.LargestRestoreBackstepSamps << '\n';
	}
	_lastPlaybackTiming = timing;
	_lastPlaybackEpoch = receipt.SessionEpoch;
}
