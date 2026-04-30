#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include "../io/JamFile.h"
#include "../io/NinjamConnection.h"

namespace engine
{
	// Owns a NinjamConnection together with the console chat-input thread.
	// All ninjam-specific lifecycle, threading, and chat I/O lives here so
	// that Scene stays free of those concerns.
	class NinjamSession
	{
	public:
		NinjamSession() = default;
		~NinjamSession();

		NinjamSession(const NinjamSession&) = delete;
		NinjamSession& operator=(const NinjamSession&) = delete;

		// Start the session from config. No-op if host/user are empty.
		void Start(const io::JamFile::NinjamConfig& config);

		// Disconnect and stop the chat thread. Idempotent.
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

	private:
		void _ChatInputLoop();

	private:
		std::unique_ptr<io::NinjamConnection> _connection;
		std::thread _chatInputThread;
		std::atomic_bool _chatInputStop{ false };
	};
}
