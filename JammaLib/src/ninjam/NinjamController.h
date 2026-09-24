#pragma once

#include <mutex>
#include <optional>
#include <string>
#include "NinjamSession.h"

namespace ninjam
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
			unsigned int numOutputChannels,
			unsigned int inLatencySamps = 0u,
			unsigned int outLatencySamps = 0u);

		NinjamSessionPumpResult Pump();
		void ApplySessionPumpResult(const NinjamSessionPumpResult& result);
		std::optional<NinjamRemoteSnapshot> TakePendingSnapshot();

		void SendChat(const std::string& msg);
		void Connect(const std::string& host);
		void Disconnect();
		void Stop();

		NinjamRemoteTiming ProcessExportBlock(const float* interleavedDacOutput,
			unsigned int numDacChannels,
			const float* interleavedAdcInput,
			unsigned int numAdcChannels,
			unsigned int numFrames,
			unsigned int sampleRate,
			std::uint64_t audioBlockStartSample);

		// Pins the current connection; keep the guard alive while using any
		// connection-owned storage returned through it.
		NinjamConnectionUse AcquireConnectionUse() const noexcept;

		NinjamSession* Session() noexcept { return &_session; }

	private:
		std::optional<io::JamFile::NinjamConfig> _config;
		NinjamSession _session;
		mutable std::mutex _pendingSnapshotMutex;
		std::optional<NinjamRemoteSnapshot> _pendingSnapshot;
	};
}
