#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include "../audio/AudioHost.h"

namespace io { class IoInputSubsystem; }

namespace engine
{
	// Serialized control protocol, never called by the audio callback. The caller
	// keeps operation eligibility, hierarchy effects and restoration notices.
	class ControlOperation
	{
	public:
		enum Kind { None, Audio, Midi, Save, Load };
		struct AudioRestoration { bool Success; bool AdmissionRestored; };
		Kind Current() const { return _kind; }
		void Begin(Kind kind) { _kind = kind; }
		std::uint64_t Identity() const { return _identity; }
		void NextIdentity() { ++_identity; }
		// Job/startup/teardown callers retain their existing lock scopes.
		std::mutex& LifecycleMutex() { return _lifecycleMutex; }
		bool HoldsWriter() const { return _writer.owns_lock(); }
		void ReleaseWriter() { if (_writer.owns_lock()) _writer.unlock(); }
		bool AdmissionPending() const { return _admissionPending.load(std::memory_order_acquire); }
		bool InputClosed() const { return _inputClosed; }
		void RequestBoundary(io::IoInputSubsystem& input, std::uint64_t revision);
		bool TryAcquireBoundary(io::IoInputSubsystem& input);
		bool PauseAudio(audio::AudioHost& audio, std::string& error);
		AudioRestoration RestoreAudio(audio::AudioHost& audio, std::string& error);
		void RestoreRunningAudio() { _pause.WasRunning = _pause.AdmissionWasOpen = true; }
		bool LoadReady() const { return _loadReady; }
		void MarkLoadReady() { _loadReady = true; }
		// Caller resumes MIDI dispatch and applies hierarchy/notice effects first.
		void Finish(io::IoInputSubsystem& input, std::optional<std::uint64_t> acceptedRevision);
		void FinishReplacement();

	private:
		friend class SourceLossIntegrationTestAccess;
		Kind _kind = None;
		std::uint64_t _identity = 0u, _revision = 0u;
		std::mutex _lifecycleMutex;
		std::unique_lock<std::mutex> _writer;
		std::atomic<bool> _admissionPending{false}; // UI publishes; job acquires.
		audio::AudioHost::QuiescenceState _pause{};
		bool _paused = false, _inputClosed = false, _loadReady = false;
	};
}
