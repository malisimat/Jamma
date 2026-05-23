#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "Station.h"
#include "../gui/GuiLabel.h"

namespace engine
{
	class LoopRemote;

	// A Station that receives audio from a single remote ninjam user.
	// One StationRemote is created per user when _UpdateRemoteStationsFromSnapshot runs.
	// It owns a LoopTake containing two LoopRemote loops (L/R) that buffer
	// the ninjam-decoded audio so it can be played back and visualised.
	class StationRemote :
		public Station
	{
	public:
		StationRemote(StationParams params,
			audio::AudioMixerParams mixerParams);

		virtual std::string ClassName() const override { return "StationRemote"; }
		virtual bool IsRemote() const noexcept override { return true; }
		virtual void SetSelectDepth(base::SelectDepth depth) override;

		// Creates the LoopTake + LoopRemote pair if not already present.
		void EnsureRemoteTake();
		void UpdateRemoteVisuals();

		void SetRemoteUserName(const std::string& userName);
		const std::string& RemoteUserName() const { return _remoteUserName; }

		void SetRemoteChannelCount(unsigned int channelCount);
		unsigned int RemoteChannelCount() const { return _remoteChannelCount; }

		void SetAssignedOutputChannel(unsigned int outChannelLeft);
		unsigned int AssignedOutputChannel() const { return _assignedOutputChannel; }

		void SetConnectedRemote(bool connected);
		bool IsConnectedRemote() const { return _isConnectedRemote; }

		// Synchronise loop cursors to the ninjam interval on the job thread.
		void SetRemoteInterval(unsigned int lengthSamps, unsigned int positionSamps, unsigned int visualLengthSamps = 0u);

		// Write a decoded stereo block into the station's LoopRemote buffers.
		// Called from the audio callback.
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
		std::shared_ptr<gui::GuiLabel> _nameLabel;
	};
}
