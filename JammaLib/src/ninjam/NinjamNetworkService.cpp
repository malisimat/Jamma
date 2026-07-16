#include "stdafx.h"
#include "NinjamNetworkService.h"
#include <cmath>
#include <set>
#include <iostream>

using namespace engine;
using namespace timing;

namespace ninjam
{
	NinjamNetworkService::NinjamNetworkService() :
		_ninjamController(std::make_shared<ninjam::NinjamController>())
	{
	}

	void NinjamNetworkService::SendChat(const std::string& msg)
	{
		_ninjamController->SendChat(msg);
	}

	void NinjamNetworkService::Connect(const std::string& host)
	{
		_ninjamController->Connect(host);
	}

	void NinjamNetworkService::Disconnect()
	{
		_ninjamController->Disconnect();
	}

	void NinjamNetworkService::SetTempoJoinOptions(const NinjamTempoJoinOptions& options)
	{
		_tempoJoinOptions = options;
	}

	void NinjamNetworkService::PrepareTempoSyncOnConnect(timing::TimingQuantiser& quantisation,
		unsigned int currentSampleRate,
		const std::vector<std::shared_ptr<engine::Station>>& stations)
	{
		_pendingRemoteTempoPrompt.reset();
		_ignoredRemoteTempoPrompt.reset();
		_joinPushAwaitingOutcome = false;
		_joinPushSentAtAcceptedWrap = 0u;
		_locallyRequestedTempo.reset();

		// Enter connected mode and rebuild connected-sync runtime state from scratch;
		// stale phase/alignment from a previous session must never carry over.
		_externalTransport.Connect();
		_externalJoinAligned = false;
		_externalGeneration = 0u;
		_InvalidateExternalPhaseCorrections(stations);

		if (_tempoJoinOptions.PushLocalTempoOnJoin)
		{
			const auto queued = quantisation.ForceQueueCurrentTempoAsPending(true, currentSampleRate);
			_joinPushAwaitingOutcome = queued;
			if (!queued)
			{
				std::cout << "[NINJAM] Join push requested, but no valid local tempo is available" << std::endl;
			}
		}
	}

	void NinjamNetworkService::ResetTempoSyncOnDisconnect(timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<engine::Station>>& stations)
	{
		_pendingRemoteTempoPrompt.reset();
		_ignoredRemoteTempoPrompt.reset();
		_joinPushAwaitingOutcome = false;
		_joinPushSentAtAcceptedWrap = 0u;
		_locallyRequestedTempo.reset();
		_externalTransport.Disconnect();
		_externalJoinAligned = false;
		_externalGeneration = 0u;
		_InvalidateExternalPhaseCorrections(stations);
		quantisation.ResetPendingTempoSyncState();
	}

	bool NinjamNetworkService::UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot,
		std::vector<std::shared_ptr<Station>>& stations)
	{
		bool stationsChanged = false;
		std::set<std::string> seenUsers;

		auto findRemoteStation = [](const std::vector<std::shared_ptr<Station>>& stations, const std::string& userName) -> std::shared_ptr<StationRemote> {
			for (const auto& station : stations)
			{
				if (station->IsRemote())
				{
					auto remote = std::dynamic_pointer_cast<StationRemote>(station);
					if (remote && remote->RemoteUserName() == userName)
						return remote;
				}
			}
			return nullptr;
		};

		for (const auto& remoteUser : snapshot.Users)
		{
			seenUsers.insert(remoteUser.UserName);

			auto remoteStation = findRemoteStation(stations, remoteUser.UserName);

			if (!remoteStation)
			{
				StationParams stationParams;
				stationParams.Name = remoteUser.UserName;
				stationParams.Size = { 200, 280 };
				stationParams.Index = static_cast<unsigned int>(stations.size());
				stationParams.Position = {
					static_cast<int>(stationParams.Index) * 600,
					0 };
				stationParams.ModelPosition = {
					static_cast<float>(stationParams.Index) * 600.0f,
					0.0f,
					0.0f };

				audio::MergeMixBehaviourParams merge;
				auto mixerParams = Station::GetMixerParams(stationParams.Size, merge);
				remoteStation = std::make_shared<StationRemote>(stationParams, mixerParams);
				remoteStation->SetRemoteUserName(remoteUser.UserName);
				remoteStation->SetNumBusChannels(2);
				remoteStation->SetNumDacChannels(2);
				stations.push_back(remoteStation);
				stationsChanged = true;
				std::cout << "[NINJAM] User joined: " << remoteUser.UserName << std::endl;
			}

			remoteStation->SetAssignedOutputChannel(remoteUser.AssignedOutputChannel);
			remoteStation->SetRemoteChannelCount(remoteUser.ChannelCount);
			remoteStation->SetConnectedRemote(true);

			if (snapshot.IntervalLengthSamps > 0)
			{
				auto visualIntervalSamps = snapshot.IntervalLengthSamps;
				if (snapshot.HasTiming)
				{
					const auto derivedInterval = TimingQuantiser::IntervalSampsFromTempo(snapshot.Bpm,
						static_cast<unsigned int>(snapshot.Bpi),
						snapshot.SampleRate);
					if (derivedInterval > 0u)
						visualIntervalSamps = std::max(visualIntervalSamps, derivedInterval);
				}

				remoteStation->SetRemoteInterval(snapshot.IntervalLengthSamps, snapshot.IntervalPositionSamps, visualIntervalSamps);
			}

			remoteStation->EnsureRemoteTake();
			remoteStation->UpdateRemoteVisuals();
		}

		for (auto it = stations.begin(); it != stations.end();)
		{
			auto remoteStation = std::dynamic_pointer_cast<StationRemote>(*it);
			if (!remoteStation)
			{
				++it;
				continue;
			}

			if (seenUsers.find(remoteStation->RemoteUserName()) == seenUsers.end())
			{
				remoteStation->SetConnectedRemote(false);
				std::cout << "[NINJAM] User left: " << remoteStation->RemoteUserName() << std::endl;
				it = stations.erase(it);
				stationsChanged = true;
			}
			else
			{
				++it;
			}
		}

		return stationsChanged;
	}

	void NinjamNetworkService::ApplyRemoteTempoToClock(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<Station>>& stations,
		const io::UserConfig& userConfig)
	{
		quantisation.ApplyRemoteTempo(snapshot, stations, userConfig);
	}

	void NinjamNetworkService::QueueLocalTempoFromClock(timing::TimingQuantiser& quantisation,
		const io::UserConfig& userConfig,
		unsigned int currentSampleRate)
	{
		quantisation.QueueLocalTempo(0u, currentSampleRate, userConfig);
	}

	void NinjamNetworkService::SendQueuedTempoAtIntervalWrap(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		unsigned int currentSampleRate)
	{
		const auto hadPendingTempo = quantisation.HasPendingTempo();
		const auto requestSampleRate = currentSampleRate;
		const auto requestedTempo = hadPendingTempo
			? quantisation.CurrentTempoTiming(requestSampleRate)
			: std::nullopt;
		quantisation.SendQueuedTempo(snapshot,
			_ninjamController->Session(),
			0u,
			currentSampleRate);
		if (hadPendingTempo && !quantisation.HasPendingTempo() && requestedTempo.has_value())
		{
			_locallyRequestedTempo = requestedTempo;
			_joinPushSentAtAcceptedWrap = _externalTransport.Diagnostics().AcceptedWraps;
		}
	}

	void NinjamNetworkService::_FeedExternalTransport(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		unsigned int currentSampleRate)
	{
		auto clock = quantisation.Clock();
		if (!clock)
			return;

		// Resolve the authoritative remote interval length, deriving it from tempo
		// when the raw interval sample count is not yet available.
		auto remoteIntervalLen = snapshot.IntervalLengthSamps;
		if (remoteIntervalLen == 0u && snapshot.HasTiming)
		{
			remoteIntervalLen = TimingQuantiser::IntervalSampsFromTempo(snapshot.Bpm,
				static_cast<unsigned int>(snapshot.Bpi),
				snapshot.SampleRate);
		}
		const auto intervalLen = timing::ExternalTransport::ScaleSampleRate(remoteIntervalLen,
			snapshot.SampleRate,
			currentSampleRate);
		const auto intervalPos = timing::ExternalTransport::ScaleSampleRate(
			snapshot.IntervalPositionSamps,
			snapshot.SampleRate,
			currentSampleRate);

		timing::ExternalTransportSnapshot xsnap;
		xsnap.IntervalLengthSamps = intervalLen;
		xsnap.IntervalPositionSamps = intervalPos;
		xsnap.SampleRate = currentSampleRate;
		xsnap.LocalAnchorSamps = clock->AbsoluteSamplePos(0ul);

		const auto decision = _externalTransport.IngestSnapshot(xsnap);
		const auto generation = _externalTransport.Generation();
		if (generation != _externalGeneration)
		{
			_externalGeneration = generation;
			_externalJoinAligned = false;
			_InvalidateExternalPhaseCorrections(stations);
		}

		// Record the mid-cycle join alignment once, the first time we observe a
		// valid remote interval against a seeded local master clock.  The alignment
		// commits at the next authoritative remote wrap.
		if (!_externalJoinAligned && intervalLen > 0u && clock->SeedSourceLength() > 0ul)
		{
			_externalTransport.BeginJoinAlignment(clock->SampOffset());
			_externalJoinAligned = true;
		}

		// Wrap-gated phase discipline: absorb accumulated drift exactly once per
		// remote interval so the local master clock stays phase-locked to NINJAM.
		if (decision.has_value())
		{
			std::optional<long long> correction;
			if (decision->IsJoin)
			{
				if (quantisation.ApplyRemotePhaseCorrection(decision->DeltaSamps,
					decision->IntervalLengthSamps))
					correction = decision->DeltaSamps;
			}
			else
				correction = quantisation.DisciplineRemotePhase(decision->RemotePositionSamps,
					decision->IntervalLengthSamps);

			if (correction.has_value() && *correction != 0)
				_QueueExternalPhaseCorrection(stations, *correction, decision->Generation);
		}
	}

	void NinjamNetworkService::_QueueExternalPhaseCorrection(
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		long long deltaSamps,
		std::uint64_t generation)
	{
		for (const auto& station : stations)
		{
			if (!station || station->IsRemote())
				continue;
			for (const auto& take : station->GetLoopTakeSnapshot())
				if (take) take->QueueExternalPhaseCorrection(deltaSamps, generation);
		}
	}

	void NinjamNetworkService::_InvalidateExternalPhaseCorrections(
		const std::vector<std::shared_ptr<engine::Station>>& stations)
	{
		for (const auto& station : stations)
		{
			if (!station || station->IsRemote())
				continue;
			for (const auto& take : station->GetLoopTakeSnapshot())
				if (take) take->InvalidateExternalPhaseCorrection();
		}
	}

	void NinjamNetworkService::HandleRemoteTempoSnapshot(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		const io::UserConfig& userConfig,
		unsigned int currentSampleRate)
	{
		// Continuously feed the authoritative external transport, then apply
		// wrap-gated phase discipline so connected playback tracks the remote
		// interval rather than free-running after a one-shot seed.
		_FeedExternalTransport(snapshot, quantisation, stations, currentSampleRate);

		auto proposal = quantisation.ProposeRemoteTempoChange(snapshot, userConfig);
		if (!proposal.has_value())
		{
			if (_locallyRequestedTempo.has_value()
				&& _MatchesLocallyRequestedTempo(snapshot.Bpm,
					static_cast<unsigned int>(std::max(0, snapshot.Bpi)),
					_locallyRequestedTempo.value()))
			{
				_locallyRequestedTempo.reset();
				_joinPushAwaitingOutcome = false;
			}
			return;
		}

		if (_locallyRequestedTempo.has_value()
			&& _MatchesLocallyRequestedTempo(proposal.value(), _locallyRequestedTempo.value()))
		{
			quantisation.AcknowledgeLocallyRequestedRemoteTempo(proposal.value());
			_pendingRemoteTempoPrompt.reset();
			_ignoredRemoteTempoPrompt.reset();
			_locallyRequestedTempo.reset();
			_joinPushAwaitingOutcome = false;
			TimingQuantiser::LogNinjamTempoEvent("Local tempo acknowledged by server",
				proposal->MasterLoopLengthSamps,
				proposal->GrainSamps,
				proposal->Bpi,
				proposal->Bpm,
				proposal->SampleRate);
			return;
		}

		if (_joinPushAwaitingOutcome)
		{
			const auto acceptedWraps = _externalTransport.Diagnostics().AcceptedWraps;
			if (snapshot.Users.empty()
				&& acceptedWraps <= (_joinPushSentAtAcceptedWrap + 1u))
				return;

			std::cout << "[NINJAM] Join push fallback: server stayed on another tempo" << std::endl;
			_joinPushAwaitingOutcome = false;
			_locallyRequestedTempo.reset();
		}

		const auto hasLocalContent = _HasAnyLocalLoopContent(stations);
		if (!_tempoJoinOptions.PromptBeforeApplyingRemoteTempo || !hasLocalContent)
		{
			if (!hasLocalContent)
				std::cout << "[NINJAM] No local loop content - auto-applying remote tempo" << std::endl;

			quantisation.ApplyAcceptedRemoteTempo(proposal.value(), stations);
			_pendingRemoteTempoPrompt.reset();
			_ignoredRemoteTempoPrompt.reset();
			return;
		}

		if (_ignoredRemoteTempoPrompt.has_value()
			&& IsSameRemoteTempoChange(_ignoredRemoteTempoPrompt.value(), proposal.value()))
		{
			return;
		}

		const auto isNewProposal = !_pendingRemoteTempoPrompt.has_value()
			|| !IsSameRemoteTempoChange(_pendingRemoteTempoPrompt.value(), proposal.value());
		_pendingRemoteTempoPrompt = proposal;
		if (isNewProposal)
		{
			timing::TimingQuantiser::LogNinjamTempoEvent("Remote tempo proposed",
				proposal->MasterLoopLengthSamps,
				proposal->GrainSamps,
				proposal->Bpi,
				proposal->Bpm,
				proposal->SampleRate);
		}
	}

	bool NinjamNetworkService::_HasAnyLocalLoopContent(const std::vector<std::shared_ptr<Station>>& stations)
	{
		for (const auto& station : stations)
		{
			if (station && !station->IsRemote() && (station->NumTakes() > 0u))
				return true;
		}

		return false;
	}

	void NinjamNetworkService::ResolveRemoteTempoPromptDecision(bool accept,
		timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<engine::Station>>& stations)
	{
		if (!_pendingRemoteTempoPrompt.has_value())
			return;

		const auto change = _pendingRemoteTempoPrompt.value();
		if (accept)
		{
			quantisation.ApplyAcceptedRemoteTempo(change, stations);
			_ignoredRemoteTempoPrompt.reset();
		}
		else
		{
			_ignoredRemoteTempoPrompt = change;
			timing::TimingQuantiser::LogNinjamTempoEvent("Remote tempo ignored by user",
				change.MasterLoopLengthSamps,
				change.GrainSamps,
				change.Bpi,
				change.Bpm,
				change.SampleRate);
			timing::TimingQuantiser::LogNinjamManualTempoCommands(change.Bpm, change.Bpi);
		}

		_pendingRemoteTempoPrompt.reset();
		_joinPushAwaitingOutcome = false;
	}

	bool NinjamNetworkService::IsSameRemoteTempoChange(const timing::PendingRemoteTempoChange& lhs,
		const timing::PendingRemoteTempoChange& rhs) noexcept
	{
		return (lhs.IntervalLengthSamps == rhs.IntervalLengthSamps)
			&& (lhs.SampleRate == rhs.SampleRate)
			&& (lhs.GrainSamps == rhs.GrainSamps)
			&& (lhs.MasterLoopLengthSamps == rhs.MasterLoopLengthSamps)
			&& (lhs.Bpi == rhs.Bpi)
			&& (std::abs(lhs.Bpm - rhs.Bpm) < 0.01f);
	}

	bool NinjamNetworkService::_MatchesLocallyRequestedTempo(
		const timing::PendingRemoteTempoChange& proposal,
		const timing::QuantisationTiming& requested) noexcept
	{
		return _MatchesLocallyRequestedTempo(proposal.Bpm, proposal.Bpi, requested);
	}

	bool NinjamNetworkService::_MatchesLocallyRequestedTempo(float bpm,
		unsigned int bpi,
		const timing::QuantisationTiming& requested) noexcept
	{
		return bpi == requested.Bpi && std::abs(bpm - requested.Bpm) < 0.01f;
	}
}
