#pragma once

#include <memory>
#include <optional>
#include "../io/ConsoleTui.h"
#include "../io/JamFile.h"
#include "../io/NinjamConnection.h"

namespace engine
{
	// Owns a NinjamConnection together with the console TUI used to drive
	// chat I/O. All ninjam-specific lifecycle, threading, and chat I/O lives
	// here so that Scene stays free of those concerns.
	class NinjamSession
	{
	public:
		NinjamSession() = default;
		~NinjamSession();

		NinjamSession(const NinjamSession&) = delete;
		NinjamSession& operator=(const NinjamSession&) = delete;

		// Start the session from config. No-op if host/user are empty.
		void Start(const io::JamFile::NinjamConfig& config);

		// Disconnect, stop the chat TUI, and restore console state.
		// Idempotent.
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
		std::unique_ptr<io::NinjamConnection> _connection;
		std::unique_ptr<io::ConsoleTui> _tui;
	};
}
