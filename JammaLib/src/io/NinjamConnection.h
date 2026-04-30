#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class NJClient;

namespace io
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

	class NinjamConnection
	{
	public:
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
			unsigned int numOutputChannels);

		// interleavedInput may be nullptr (sends silence as local audio).
		void ProcessAudioBlock(const float* interleavedInput,
			unsigned int numFrames,
			unsigned int sampleRate);

		NinjamRemoteSnapshot Snapshot() const;

		// Sends NINJAM admin commands to update server tempo (BPM and BPI).
		// Only takes effect if the connected user has admin privileges on the
		// server. Safe to call from job thread; returns false if not connected.
		bool RequestServerTempo(float bpm, int bpi);

		// Fills left/right with the decoded stereo pair for the given output-channel
		// index (assigned by NinjamConnection). Returns false if no audio is ready.
		bool ConsumeStereoPair(unsigned int outChannelLeft,
			const float*& left,
			const float*& right,
			unsigned int& numFrames) const;

	private:
		bool _StartConnectAttempt(std::chrono::steady_clock::time_point now);
		bool _HasActiveConnectAttempt() const noexcept;
		void _ResetReconnectState(std::chrono::steady_clock::time_point now);
		void _ScheduleRetry(std::chrono::steady_clock::time_point now);
		void _EnsureWorkDir();
		void _ResizeScratchBuffers(unsigned int numFrames);
		void _ApplyLocalChannels();
		void _UpdateSnapshot();
		unsigned int _AssignOutputChannel(const std::string& userName);

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
		std::atomic_uint _lastNumFrames{ 0u };

		std::vector<std::vector<float>> _outScratch;
		std::vector<std::vector<float>> _inScratch;
		std::vector<float*> _outPtrs;
		std::vector<float*> _inPtrs;

		std::unordered_map<std::string, unsigned int> _userOutputChannels;
		std::vector<std::string> _lastLoggedUsers;
		NinjamRemoteSnapshot _snapshot;
		std::string _lastError;

		mutable std::mutex _snapshotMutex;
		mutable std::mutex _audioBufferMutex;
		mutable std::mutex _connectionMutex;
		std::unique_ptr<NJClient> _client;
	};
}
