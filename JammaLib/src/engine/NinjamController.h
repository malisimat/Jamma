#pragma once

#include <mutex>
#include <optional>
#include <string>
#include "NinjamSession.h"

namespace engine
{
	class NinjamController
	{
	public:
		NinjamController() = default;
		~NinjamController() { Stop(); }

		NinjamController(const NinjamController&) = delete;
		NinjamController& operator=(const NinjamController&) = delete;

		void LoadConfig(const std::optional<io::JamFile::NinjamConfig>& config);
		const std::optional<io::JamFile::NinjamConfig>& Config() const noexcept { return _config; }

		void SetAudioFormat(unsigned int sampleRate,
			unsigned int blockSize,
			unsigned int numInputChannels,
			unsigned int numOutputChannels);

		std::optional<io::NinjamRemoteSnapshot> Pump();
		std::optional<io::NinjamRemoteSnapshot> TakePendingSnapshot();

		void SendChat(const std::string& msg);
		void Connect(const std::string& host);
		void Disconnect();
		void Stop();

		void ProcessAudioBlock(const float* interleavedInput,
			unsigned int numFrames,
			unsigned int sampleRate);

		bool ConsumeStereoPair(unsigned int outChannelLeft,
			const float*& left,
			const float*& right,
			unsigned int& numFrames) const;

		NinjamSession* Session() noexcept { return &_session; }

	private:
		std::optional<io::JamFile::NinjamConfig> _config;
		NinjamSession _session;
		mutable std::mutex _pendingSnapshotMutex;
		std::optional<io::NinjamRemoteSnapshot> _pendingSnapshot;
	};
}