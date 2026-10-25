#include "NinjamTimingDiagnosticPresenter.h"
#include <algorithm>
#include <ostream>
#include "NinjamSession.h"
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

void NinjamTimingDiagnosticPresenter::PresentConnect(std::uint64_t join, std::uint64_t request,
	const NinjamTempoJoinOptions& options, const std::optional<engine::QuantisationTiming>& localTiming,
	std::ostream& output)
{
	output << "[NINJAM][TimingPolicy] changed policy=no-sync reason=reconnect"
		<< " join=" << join << '\n';
	output << "[NINJAM][TempoJoin] connect join=" << join
		<< " request=" << (options.PushLocalTempoOnJoin ? request : 0u)
		<< " pushLocal=" << options.PushLocalTempoOnJoin;
	if (localTiming.has_value())
		output << " bpm=" << localTiming->Bpm << " bpi=" << localTiming->SeedCount
			<< " interval=" << localTiming->MasterLoopSamps << " grain=" << localTiming->SeedSamps;
	output << '\n';
}

void NinjamTimingDiagnosticPresenter::PresentUpdate(const NinjamTimingUpdate& update,
	std::chrono::steady_clock::time_point now, const NinjamLocalTransportObservation& clock,
	unsigned int grain, bool pendingPrompt, std::ostream& output)
{
	if (update.DesiredTransport || update.PromptForTempoChange || update.TempoRequest)
	{
		output << "[NINJAM][Flow] ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
			now.time_since_epoch()).count()
			<< " reason=" << NinjamTimingCoordinator::NoSyncReasonName(update.NoSyncReason)
			<< " prompt=" << update.PromptForTempoChange
			<< " tempoRequest=" << update.TempoRequest.has_value()
			<< " clock=" << clock.MasterPhaseSamps << '/' << clock.MasterLengthSamps
			<< " grain=" << grain
			<< " scene=" << clock.SceneSamplePos;
		if (update.DesiredTransport)
		{
			const auto& desired = *update.DesiredTransport;
			output << " epoch=" << desired.SessionEpoch << " version=" << desired.Version
				<< " generation=" << desired.Generation << " intent=" << static_cast<unsigned int>(desired.Intent)
				<< " policy=" << NinjamTimingCoordinator::FollowPolicyName(desired.LocalFollowPolicy)
				<< " remote=" << desired.RemoteMasterPhaseSamps << '/' << desired.RemoteMasterIntervalLengthSamps
				<< " bpm=" << desired.TempoBpm << " bpi=" << desired.BeatsPerInterval
				<< " phaseDevice=" << desired.RemotePhaseDeviceSample;
		}
		if (pendingPrompt)
			output << " pendingPrompt=1";
		output << '\n';
	}
	if (update.DesiredTransport && update.DesiredTransport->HasRemoteTiming)
	{
		const auto& desired = *update.DesiredTransport;
		output << "[NINJAM][TimingPolicy] determined policy="
			<< ninjam::NinjamTimingCoordinator::FollowPolicyName(desired.LocalFollowPolicy)
			<< " remoteBpm=" << desired.TempoBpm
			<< " generation=" << desired.Generation
			<< " remotePhaseDeviceSample=" << desired.RemotePhaseDeviceSample << '\n';
	}
}

void NinjamTimingDiagnosticPresenter::PresentTempoState(TempoRequestState state,
	const NinjamTimingDiagnostics& diagnostics, std::uint64_t join, std::uint64_t request,
	std::ostream& output)
{
	const char* name = "idle";
	switch (state)
	{
	case ninjam::TempoRequestState::Queued: name = "queued"; break;
	case ninjam::TempoRequestState::SentAwaitingOutcome: name = "awaiting-server-observation"; break;
	case ninjam::TempoRequestState::Acknowledged: name = "acknowledged"; break;
	case ninjam::TempoRequestState::Expired: name = "expired-unknown"; break;
	default: break;
	}
	output << "[NINJAM][TempoJoin] state join=" << join
		<< " request=" << request
		<< " value=" << name
		<< " sent=" << diagnostics.TempoRequestsSent
		<< " retries=" << diagnostics.TempoRequestRetries
		<< " acknowledged=" << diagnostics.TempoAcknowledged
		<< " expired=" << diagnostics.TempoRequestsExpired << '\n';
}

void NinjamTimingDiagnosticPresenter::PresentSessionEdge(const NinjamSessionTimingStatus& status,
	bool hasLocalContent, const std::optional<engine::QuantisationTiming>& localTiming, std::ostream& output)
{
	output << "[NINJAM][SessionEdge] available=" << status.IsAvailable
		<< " epoch=" << status.SessionEpoch << " hasLocalContent=" << hasLocalContent
		<< " localTiming=" << localTiming.has_value()
		<< " localMaster=" << (localTiming ? localTiming->MasterLoopSamps : 0u) << '\n';
}

void NinjamTimingDiagnosticPresenter::ObserveJobTick(std::chrono::steady_clock::time_point now) noexcept
{
	if (_lastJobAt)
		_maxJobGapMs = std::max(_maxJobGapMs,
			std::chrono::duration_cast<std::chrono::milliseconds>(now - *_lastJobAt).count());
	_lastJobAt = now;
}

void NinjamTimingDiagnosticPresenter::ObservePumpDuration(std::chrono::steady_clock::duration elapsed) noexcept
{
	_maxPumpUs = std::max(_maxPumpUs, std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

void NinjamTimingDiagnosticPresenter::PresentJobTrace(bool hasLocalContent,
	const std::optional<engine::QuantisationTiming>& localTiming, TempoRequestState requestState,
	bool pendingPrompt, const NinjamRemoteTiming* raw, std::ostream& output)
{
	output << "[NINJAM][JobTrace] hasLocalContent=" << hasLocalContent
		<< " localTiming=" << localTiming.has_value()
		<< " localMaster=" << (localTiming ? localTiming->MasterLoopSamps : 0u)
		<< " localBpm=" << (localTiming ? localTiming->Bpm : 0.0f)
		<< " localBpi=" << (localTiming ? localTiming->Bpi : 0u)
		<< " requestState=" << static_cast<unsigned int>(requestState)
		<< " pendingPrompt=" << pendingPrompt
		<< " maxJobGapMs=" << _maxJobGapMs << " maxPumpUs=" << _maxPumpUs
		<< " jobQueueDepthPreviousTick=" << _jobQueueDepth << '\n';
	_maxJobGapMs = 0;
	_maxPumpUs = 0;
	if (raw)
	{
		output << "[NINJAM][RemoteObservation] valid=" << raw->IsValid
			<< " phase=" << raw->IntervalPositionSamps << '/' << raw->IntervalLengthSamps
			<< " bpm=" << raw->Bpm << " bpi=" << raw->Bpi << " sr=" << raw->SourceSampleRate
			<< " expectedInterval=" << ninjam::IntervalSampsFromTempo(raw->Bpm, raw->Bpi, raw->SourceSampleRate)
			<< " device=" << raw->DeviceAudioSampleAtObservation << '\n';
	}
}
