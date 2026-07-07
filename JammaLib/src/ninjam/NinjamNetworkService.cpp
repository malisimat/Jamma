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
		unsigned int currentSampleRate)
	{
		_pendingRemoteTempoPrompt.reset();
		_ignoredRemoteTempoPrompt.reset();
		_joinPushAwaitingOutcome = false;

		// Enter connected mode and rebuild connected-sync runtime state from scratch;
		// stale phase/alignment from a previous session must never carry over.
		_externalTransport.Connect();
		_externalJoinAligned = false;

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

	void NinjamNetworkService::ResetTempoSyncOnDisconnect(timing::TimingQuantiser& quantisation)
	{
		_pendingRemoteTempoPrompt.reset();
		_ignoredRemoteTempoPrompt.reset();
		_joinPushAwaitingOutcome = false;
		_externalTransport.Disconnect();
		_externalJoinAligned = false;
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
		quantisation.QueueLocalTempo(quantisation.RemoteSampleRate(), currentSampleRate, userConfig);
	}

	void NinjamNetworkService::SendQueuedTempoAtIntervalWrap(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		unsigned int currentSampleRate)
	{
		quantisation.SendQueuedTempo(snapshot,
			_ninjamController->Session(),
			quantisation.RemoteSampleRate(),
			currentSampleRate);
	}

	void NinjamNetworkService::_FeedExternalTransport(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation)
	{
		auto clock = quantisation.Clock();
		if (!clock)
			return;

		// Resolve the authoritative remote interval length, deriving it from tempo
		// when the raw interval sample count is not yet available.
		auto intervalLen = snapshot.IntervalLengthSamps;
		if (intervalLen == 0u && snapshot.HasTiming)
		{
			intervalLen = TimingQuantiser::IntervalSampsFromTempo(snapshot.Bpm,
				static_cast<unsigned int>(snapshot.Bpi),
				snapshot.SampleRate);
		}

		timing::ExternalTransportSnapshot xsnap;
		xsnap.IntervalLengthSamps = intervalLen;
		xsnap.IntervalPositionSamps = snapshot.IntervalPositionSamps;
		xsnap.SampleRate = snapshot.SampleRate;
		xsnap.LocalAnchorSamps = clock->AbsoluteSamplePos(0ul);

		const auto wrapBefore = _externalTransport.Published()->RemoteWrapCount;
		_externalTransport.IngestSnapshot(xsnap);

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
		const auto wrapAfter = _externalTransport.Published()->RemoteWrapCount;
		if (intervalLen > 0u && wrapAfter > wrapBefore)
			quantisation.DisciplineRemotePhase(snapshot.IntervalPositionSamps, intervalLen);
	}

	void NinjamNetworkService::HandleRemoteTempoSnapshot(const NinjamRemoteSnapshot& snapshot,
		timing::TimingQuantiser& quantisation,
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		const io::UserConfig& userConfig)
	{
		// Continuously feed the authoritative external transport, then apply
		// wrap-gated phase discipline so connected playback tracks the remote
		// interval rather than free-running after a one-shot seed.
		_FeedExternalTransport(snapshot, quantisation);

		const auto joinPushSent = _joinPushAwaitingOutcome && !quantisation.HasPendingTempo();

		auto proposal = quantisation.ProposeRemoteTempoChange(snapshot, userConfig);
		if (!proposal.has_value())
		{
			if (joinPushSent)
			{
				_joinPushAwaitingOutcome = false;
			}
			return;
		}

		if (joinPushSent && snapshot.Users.empty())
		{
			_joinPushAwaitingOutcome = false;
			return;
		}

		if (_joinPushAwaitingOutcome)
		{
			std::cout << "[NINJAM] Join push fallback: server stayed on another tempo" << std::endl;
			_joinPushAwaitingOutcome = false;
		}

		if (!_tempoJoinOptions.PromptBeforeApplyingRemoteTempo)
		{
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
}
