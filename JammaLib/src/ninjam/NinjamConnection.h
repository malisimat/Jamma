#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../audio/AudioBuffer.h"
#include "ExportLaneTiming.h"

class NJClient;

namespace ninjam
{
	struct NinjamRemoteChannel
	{
		int ChannelIndex = -1;
		std::string Name;
		bool Subscribed = false;
		float PeakLeft = 0.0f;
		float PeakRight = 0.0f;
	};

	struct NinjamRemoteUser
	{
		std::string UserName;
		unsigned int AssignedOutputChannel = 0;
		unsigned int ChannelCount = 0;
		std::vector<NinjamRemoteChannel> Channels;
	};

	// Thread-safe snapshot of current ninjam session state.
	// Populated on the job thread; consumed by audio and UI.
	struct NinjamRemoteSnapshot
	{
		unsigned int IntervalPositionSamps = 0;
		unsigned int IntervalLengthSamps = 0;
		unsigned int SampleRate = 0;
		float Bpm = 0.0f;
		int Bpi = 0;
		bool HasTiming = false;
		std::vector<NinjamRemoteUser> Users;
	};

	struct NinjamLanePacking
	{
		std::uint16_t LaneCount = 0;
		std::uint16_t DacPairs = 0;
		std::uint16_t AdcPairs = 0;
		std::uint8_t Modulo = 0;
		std::uint8_t FromFallback = 0;
		std::uint16_t SlotLimit = 0;
	};

	struct NinjamLiveTiming
	{
		unsigned int intervalPositionSamps = 0u;
		unsigned int intervalLengthSamps = 0u;
		unsigned int sampleRate = 0u;
		float bpm = 0.0f;
		unsigned int bpi = 0u;
		bool valid = false;
	};

	class NinjamConnection
	{
	public:
		static constexpr unsigned int DefaultLocalChannelSlotLimit = 8u;

		// Escape hatch for the export-lane latency compensation (delayed DAC/ADC
		// packing) described in doc/ninjam-live-loop-latency-sync-planC.md.
		// Default OFF: flip to true only after the physical DAC-to-ADC loopback
		// verification in the plan's §5/step 10 has been run against a real or
		// test NINJAM server; false keeps today's uncompensated pass-through.
		static constexpr bool ExportLatencyCompensationEnabled = false;

		enum class ConnectionState
		{
			Disconnected,
			Connecting,
			Connected,
			Retrying,
			Failed
		};

		NinjamConnection(std::string host,
			std::string user,
			std::string pass,
			std::string workDir);
		~NinjamConnection();

		bool Connect();
		void Disconnect();
		bool IsConnected() const noexcept;
		ConnectionState State() const noexcept;
		std::string LastError() const;
		void Pump();

		void SetAudioFormat(unsigned int sampleRate,
			unsigned int blockSize,
			unsigned int numInputChannels,
			unsigned int numOutputChannels,
			unsigned int inLatencySamps = 0u,
			unsigned int outLatencySamps = 0u);

		void ProcessExportBlock(const float* interleavedDacOutput,
			unsigned int numDacChannels,
			const float* interleavedAdcInput,
			unsigned int numAdcChannels,
			unsigned int numFrames,
			unsigned int sampleRate);

		NinjamLiveTiming GetLiveTiming() const noexcept;

		NinjamRemoteSnapshot Snapshot() const;

		// Sends NINJAM tempo-change commands to the server for BPM and BPI.
		// Two forms are sent in parallel:
		//   - Admin form (/bpm N, /bpi N): applied immediately if the connected
		//     user has admin privileges on the server.
		//   - Vote form (!vote bpm N, !vote bpi N): non-admin fallback; the server
		//     counts votes across all participants and applies the change once a
		//     majority (>50% of non-silent users) agree.
		// Server vote-progress and confirmation announcements are delivered back
		// as MSG chat messages with no sender, which _OnChatMessage prints to the
		// console.  When the change takes effect (via either path), GetActualBPM()
		// updates and Scene applies it to the local clock within one
		// job tick (~20 ms).
		// Safe to call from the job thread; returns false if not connected.
		bool RequestServerTempo(float bpm, int bpi);
		static std::string FormatTempoBpm(float bpm);

		// Fills left/right with the decoded stereo pair for the given output-channel
		// index (assigned by NinjamConnection). Returns false if no audio is ready.
		bool ConsumeStereoPair(unsigned int outChannelLeft,
			const float*& left,
			const float*& right,
			unsigned int& numFrames) const;

		// Broadcasts a text message to all users in the current session.
		// Safe to call from any thread while connected.
		void SendChat(const std::string& message);

		// Rate-limited health signal for the export-lane timing anomaly
		// detector (planC §3.2/§3.3): count of blocks where NJClient's
		// reported (pos, length) jumped further than expected without a
		// length change. Should stay at zero in normal operation.
		unsigned int ExportAnomalyCount() const noexcept;

	private:
		NinjamLanePacking _ResolveLanePacking() const;
		unsigned int _InputScratchChannelCapacity() const noexcept;
		void _RefreshLanePacking();
		static void _OnChatMessage(void* userData,
			NJClient* inst,
			const char** parms,
			int nparms);
		bool _StartConnectAttempt(std::chrono::steady_clock::time_point now);
		bool _HasActiveConnectAttempt() const noexcept;
		void _ResetReconnectState(std::chrono::steady_clock::time_point now);
		void _ScheduleRetry(std::chrono::steady_clock::time_point now);
		void _EnsureWorkDir();
		void _ResizeScratchBuffers(unsigned int numFrames,
			unsigned int numInputScratchChannels);
		void _ApplyLocalChannels();
		void _UpdateSnapshot();
		unsigned int _AssignOutputChannel(const std::string& userName);
		void _ResizeExportDelayLines();
		static void _WriteExportDelayLine(std::vector<std::shared_ptr<audio::AudioBuffer>>& lines,
			const float* interleaved,
			unsigned int numChannels,
			unsigned int numFrames,
			std::vector<float>& silenceScratch);
		static void _ReadExportDelayLine(std::vector<std::shared_ptr<audio::AudioBuffer>>& lines,
			unsigned int delaySamps,
			unsigned int numFrames,
			std::vector<float>& delayedInterleaved,
			std::vector<float>& tempBuf);

	private:
		std::string _host;
		std::string _user;
		std::string _pass;
		std::string _workDir;
		bool _autoReconnect = false;
		unsigned int _connectAttempts = 0u;
		std::chrono::steady_clock::time_point _nextRetryAt{};
		std::chrono::steady_clock::time_point _connectStartedAt{};
		std::chrono::milliseconds _retryDelayMin{ std::chrono::milliseconds(1500) };
		std::chrono::milliseconds _retryDelay{ std::chrono::milliseconds(1500) };
		std::chrono::milliseconds _retryDelayMax{ std::chrono::milliseconds(30000) };
		std::chrono::milliseconds _connectTimeout{ std::chrono::seconds(20) };
		std::atomic_bool _isConnected{ false };
		std::atomic<ConnectionState> _state{ ConnectionState::Disconnected };

		unsigned int _sampleRate = 0u;
		unsigned int _blockSize = 0u;
		unsigned int _numInputChannels = 0u;
		unsigned int _numOutputChannels = 2u;
		// Raw physical DAC channel count as passed to SetAudioFormat, before
		// _numOutputChannels is clamped to kMinimumNinjamOutputChannels for
		// NJClient's remote-playback outnch. Used to size/index the export
		// DAC delay lines, which pack OUR physical output into NINJAM send
		// lanes and have nothing to do with NJClient's remote-receive channel
		// count.
		unsigned int _numDacPhysicalChannels = 0u;
		unsigned int _inLatencySamps = 0u;
		unsigned int _outLatencySamps = 0u;
		std::atomic_uint _lastNumFrames{ 0u };
		std::atomic<NinjamLanePacking> _lanePacking{};

		std::vector<std::vector<float>> _outScratch;
		std::vector<std::vector<float>> _inScratch;
		std::vector<float*> _outPtrs;
		std::vector<float*> _inPtrs;

		// Export-lane latency compensation (planC): one delay line per
		// physical DAC/ADC channel, indexed independently of NINJAM lane
		// packing (which can change without a format change). Held by
		// shared_ptr (matching ChannelMixer's AudioBuffer ownership
		// convention) since AudioBuffer is non-copyable.
		std::vector<std::shared_ptr<audio::AudioBuffer>> _dacDelayLines;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _adcDelayLines;
		std::vector<float> _delayedDacInterleaved;
		std::vector<float> _delayedAdcInterleaved;
		std::vector<float> _dacDelayTemp;
		std::vector<float> _adcDelayTemp;
		std::vector<float> _exportSilenceScratch;
		unsigned int _exportTick = 0u;
		ExportLaneTimingState _exportTimingState;
		std::atomic_uint _exportAnomalyCount{ 0u };

		std::unordered_map<std::string, unsigned int> _userOutputChannels;
		std::vector<std::string> _lastLoggedUsers;
		NinjamRemoteSnapshot _snapshot;
		std::string _lastError;

		mutable std::mutex _snapshotMutex;
		mutable std::mutex _connectionMutex;
		std::unique_ptr<NJClient> _client;
	};
}
