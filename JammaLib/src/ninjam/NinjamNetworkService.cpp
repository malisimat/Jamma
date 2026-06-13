#include "stdafx.h"
#include "NinjamNetworkService.h"
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
}
