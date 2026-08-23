#include "stdafx.h"
#include "NinjamNetworkService.h"
#include <cmath>
#include <set>
#include <iostream>

using namespace engine;

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

	void NinjamNetworkService::PrepareTempoSyncOnConnect(
		const std::optional<engine::QuantisationTiming>& localTiming)
	{
		_timingCoordinator.Connect(_tempoJoinOptions, localTiming);
	}

	void NinjamNetworkService::ResetTempoSyncOnDisconnect()
	{
		_timingCoordinator.Disconnect();
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

			if (snapshot.Timing.IntervalLengthSamps > 0)
			{
				auto visualIntervalSamps = snapshot.Timing.IntervalLengthSamps;
				if (snapshot.Timing.IsValid)
				{
					const auto derivedInterval = IntervalSampsFromTempo(snapshot.Timing.Bpm,
						snapshot.Timing.Bpi,
						snapshot.Timing.SourceSampleRate);
					if (derivedInterval > 0u)
						visualIntervalSamps = std::max(visualIntervalSamps, derivedInterval);
				}

				remoteStation->SetRemoteInterval(snapshot.Timing.IntervalLengthSamps,
					snapshot.Timing.IntervalPositionSamps, visualIntervalSamps);
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

	NinjamTimingUpdate NinjamNetworkService::ObserveTiming(const NinjamTiming& timing,
		const std::optional<engine::QuantisationTiming>& localTiming,
		bool hasLocalContent,
		const io::UserConfig& userConfig,
		utils::Timer& clock)
	{
		return _timingCoordinator.Observe(timing, localTiming, hasLocalContent, userConfig, clock);
	}

	NinjamTimingUpdate NinjamNetworkService::ResolveRemoteTempoPromptDecision(bool accept,
		const std::optional<engine::QuantisationTiming>& localTiming,
		utils::Timer& clock)
	{
		return _timingCoordinator.ResolveTempoChange(accept, localTiming, clock);
	}

	void NinjamNetworkService::SendTempoRequest(const NinjamTempoRequest& request)
	{
		bool sent = false;
		if (auto* session = _ninjamController->Session())
			sent = session->RequestServerTempo(request.Bpm, static_cast<int>(request.Bpi));
		// Report delivery back so the coordinator can retry on failure (§3.5).
		_timingCoordinator.NotifyTempoRequestSent(sent);
	}
}
