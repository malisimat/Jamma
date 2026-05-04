#pragma once

#include <memory>
#include <optional>
#include <string>
#include "../io/JamFile.h"
#include "../io/NinjamConnection.h"

namespace engine
{
	// Owns a NinjamConnection and manages all ninjam-specific lifecycle so
	// that Scene stays free of those concerns.
	// Chat I/O is driven externally: the caller sends messages via SendChat()
	// and inbound messages are surfaced through std::cout (which the
	// application-level ConsoleTui may intercept).
	class NinjamSession
	{
	public:
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
		std::optional<io::NinjamRemoteSnapshot> Pump();

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

	private:
		std::unique_ptr<io::NinjamConnection> _connection;
	};
}
