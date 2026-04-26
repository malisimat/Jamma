#pragma once

#include <atomic>
#include <chrono>
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

		// Fills left/right with the decoded stereo pair for the given output-channel
		// index (assigned by NinjamConnection). Returns false if no audio is ready.
		bool ConsumeStereoPair(unsigned int outChannelLeft,
			const float*& left,
			const float*& right,
			unsigned int& numFrames) const;

	private:
		void _EnsureWorkDir();
		void _EnsureScratchBuffers(unsigned int numFrames);
		void _ConfigureLocalChannels();
		void _RefreshSnapshot();
		unsigned int _AssignOutputChannel(const std::string& userName);

	private:
		std::string _host;
		std::string _user;
		std::string _pass;
		std::string _workDir;
		std::atomic_bool _isConnected;
		std::atomic<ConnectionState> _state;

		unsigned int _sampleRate;
		unsigned int _blockSize;
		unsigned int _numInputChannels;
		unsigned int _numOutputChannels;
		unsigned int _lastNumFrames;

		std::vector<std::vector<float>> _outScratch;
		std::vector<std::vector<float>> _inScratch;
		std::vector<float*> _outPtrs;
		std::vector<float*> _inPtrs;

		std::unordered_map<std::string, unsigned int> _userOutputChannels;
		NinjamRemoteSnapshot _snapshot;
		std::string _lastError;

		mutable std::mutex _snapshotMutex;
		mutable std::mutex _audioBufferMutex;
		mutable std::mutex _connectionMutex;
		NJClient* _clientRaw;
	};
}
