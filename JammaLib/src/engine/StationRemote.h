#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "Station.h"

namespace engine
{
	class LoopRemote;

	class StationRemote :
		public Station
	{
	public:
		StationRemote(StationParams params,
			audio::AudioMixerParams mixerParams);

		virtual std::string ClassName() const override { return "StationRemote"; }

		void EnsureRemoteTake();
		void SetRemoteUserName(const std::string& userName);
		const std::string& RemoteUserName() const { return _remoteUserName; }

		void SetRemoteChannelCount(unsigned int channelCount);
		unsigned int RemoteChannelCount() const { return _remoteChannelCount; }

		void SetAssignedOutputChannel(unsigned int outChannelLeft);
		unsigned int AssignedOutputChannel() const { return _assignedOutputChannel; }

		void SetConnectedRemote(bool connected);
		bool IsConnectedRemote() const { return _isConnectedRemote; }

		void SetRemoteInterval(unsigned int lengthSamps, unsigned int positionSamps);
		void IngestStereoBlock(const float* left,
			const float* right,
			unsigned int numSamps);

	private:
		std::string _remoteUserName;
		unsigned int _remoteChannelCount;
		unsigned int _assignedOutputChannel;
		bool _isConnectedRemote;

		std::atomic<unsigned int> _intervalLengthSamps;
		std::atomic<unsigned int> _intervalPositionSamps;

		std::shared_ptr<LoopTake> _remoteTake;
		std::shared_ptr<LoopRemote> _leftLoop;
		std::shared_ptr<LoopRemote> _rightLoop;
	};
}
