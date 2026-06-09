#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "../io/JamFile.h"
#include "NinjamConnection.h"

namespace ninjam
{
	class NinjamSession;

	class NinjamConnectionUse
	{
	public:
		explicit NinjamConnectionUse(const NinjamSession& session) noexcept;
		~NinjamConnectionUse();

		NinjamConnectionUse(const NinjamConnectionUse&) = delete;
		NinjamConnectionUse& operator=(const NinjamConnectionUse&) = delete;

		NinjamConnection* operator->() const noexcept
		{
			return _connection;
		}

		explicit operator bool() const noexcept
		{
			return _connection != nullptr;
		}

	private:
		const NinjamSession& _session;
		NinjamConnection* _connection = nullptr;
	};

	// Owns a NinjamConnection and manages all ninjam-specific lifecycle so
	// that Scene stays free of those concerns.
	// Chat I/O is driven externally: the caller sends messages via SendChat()
	// and inbound messages are surfaced through std::cout (which the
	// application-level ConsoleTui may intercept).
	class NinjamSession
	{
	public:
		struct PublicServerInfo
		{
			std::string Name;
			std::string Host;
			std::string Description;
			std::string Topic;
			std::string Status;
			std::vector<std::string> UserNames;
			int ActiveUsers = -1;
			int Capacity = -1;
			int Bpi = 0;
			float Bpm = 0.0f;
			bool HasLiveData = false;
			bool IsReachable = true;
		};

		struct PublicServerDirectorySnapshot
		{
			std::vector<PublicServerInfo> Servers;
			bool RefreshInFlight = false;
			bool HasLiveData = false;
		};

		NinjamSession() = default;
		~NinjamSession();

		NinjamSession(const NinjamSession&) = delete;
		NinjamSession& operator=(const NinjamSession&) = delete;

		// Start the session from config. No-op if host/user are empty.
		void Start(const io::JamFile::NinjamConfig& config);

		// Disconnect and clean up. Idempotent.
		void Stop();

		bool IsConnected() const noexcept;

		// Pump the connection on the job thread.
		// Returns a snapshot when connected; nullopt otherwise.
		std::optional<NinjamRemoteSnapshot> Pump();

		void SetAudioFormat(unsigned int sampleRate,
			unsigned int blockSize,
			unsigned int numInputChannels,
			unsigned int numOutputChannels);

		// interleavedInput may be nullptr.
		void ProcessAudioBlock(const float* interleavedInput,
			unsigned int numFrames,
			unsigned int sampleRate);

		// Returns false if no audio is ready for this output-channel pair.
		bool ConsumeStereoPair(unsigned int outChannelLeft,
			const float*& left,
			const float*& right,
			unsigned int& numFrames) const;

		// Send a chat message. Logs "[NINJAM] <you> ..." on success.
		// No-op if not connected.
		void SendChat(const std::string& msg);

		// Request a tempo change on the server. Returns true on success.
		// No-op (returns false) if not connected.
		bool RequestServerTempo(float bpm, int bpi);

		static PublicServerDirectorySnapshot GetPublicServerDirectorySnapshot();
		static std::vector<PublicServerInfo> GetReachablePublicServers();
		static bool RefreshPublicServerDirectoryAsync(std::function<void()> onComplete = {});
		static std::string FormatPublicServerSummary(const PublicServerInfo& server);

	private:
		friend class NinjamConnectionUse;

		static const std::array<const char*, 17> _StaticServerHosts;
		static constexpr auto _ServerListFetchTtl = std::chrono::seconds(30);
		static constexpr int _ServerListFetchTimeoutMs = 6500;

		static std::mutex _PublicServerListMutex;
		static std::vector<PublicServerInfo> _PublicServerListCache;
		static std::chrono::steady_clock::time_point _PublicServerListLastFetch;
		static std::atomic_bool _PublicServerListFetchInFlight;
		static bool _PublicServerListHasLiveData;

		static std::vector<PublicServerInfo> BuildStaticServerList();
		static std::optional<std::string> FetchAutosongServerListHtml();
		static std::vector<PublicServerInfo> ParseAutosongServerList(const std::string& html);
		static std::vector<PublicServerInfo> MergeServerLists(const std::vector<PublicServerInfo>& fetched);

		// Serializes Start/Stop while keeping audio and job paths lock-free.
		mutable std::mutex _lifecycleMutex;
		std::unique_ptr<NinjamConnection> _ownedConnection;
		std::atomic<NinjamConnection*> _connection{ nullptr };
		mutable std::atomic_uint _activeConnectionUsers{ 0u };

		std::unique_ptr<NinjamConnection> _UnpublishConnectionLocked();
		NinjamConnection* _AcquireConnectionUse() const noexcept;
		void _ReleaseConnectionUse() const noexcept;

		std::atomic_uint _audioSampleRate{ 0u };
		std::atomic_uint _audioBlockSize{ 0u };
		std::atomic_uint _audioNumInputChannels{ 0u };
		std::atomic_uint _audioNumOutputChannels{ 0u };
	};

	inline NinjamConnectionUse::NinjamConnectionUse(const NinjamSession& session) noexcept
		: _session(session), _connection(session._AcquireConnectionUse())
	{
	}

	inline NinjamConnectionUse::~NinjamConnectionUse()
	{
		if (_connection)
			_session._ReleaseConnectionUse();
	}
}
