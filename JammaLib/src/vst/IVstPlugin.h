///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <atomic>
#include <memory>
#include <windows.h>
#include "../midi/MidiEvent.h"
#include "../utils/CommonTypes.h"
#include "VstAudioBuffers.h"
#include "../utils/StringUtils.h"

namespace vst
{
	// Snapshot of host transport/tempo state passed to each plugin before processing
	// each audio block.  Populated on the audio thread; must be trivially copyable.
	struct HostTimeState
	{
		// Monotonic callback-owned project timeline. Keep this integral until an
		// adapter converts it to its SDK representation; the old uint32 block
		// counter wraps during a long-running live session.
		std::uint64_t samplePos = 0u;
		double sampleRate = 44100.0;
		double tempo      = 120.0;
		int32_t bpi       = 4;
		double ppqPos     = 0.0;
		bool hasPpqPos    = false;
		bool musicalPositionChanged = false;
		bool isPlaying    = false;
	};

	// IVstPlugin is the common interface for all hosted VST plugin types
	// (VST2 via Vst2Plugin, VST3 via Vst3Plugin).
	//
	// Threading contract: identical to Vst3Plugin — see that class for details.
	class IVstPlugin
	{
	public:
		virtual ~IVstPlugin() = default;

		// Identity is non-RT metadata, assigned before publication/import only.
		const std::string& StateId() const noexcept { return _stateId; }
		void SetStateId(const std::string& id) { if (!id.empty()) _stateId = id; }
		std::uint64_t AutomationGeneration() const noexcept { return _stateGeneration.load(std::memory_order_acquire); }
		bool IsStateOperationActive() const noexcept
		{
			return (_stateAccess.load(std::memory_order_acquire) & ExclusiveState) != 0
				|| _blobSdkActive.load(std::memory_order_acquire);
		}

		// UI-thread only. A failed bounded acquisition leaves output/state untouched;
		// the UI pump retries later. Blob calls run while all SDK entry guards are
		// excluded. Editors and blob work share the same UI owner. No audio wait.
		bool TryCaptureLiveState(std::vector<std::uint8_t>& output) const
		{
			if (_blobSdkActive.load(std::memory_order_acquire)) return false;
			std::uint32_t expected = 0;
			if (!_stateAccess.compare_exchange_strong(expected, ExclusiveState, std::memory_order_acq_rel)) return false;
			ExclusiveRelease release{_stateAccess};
			auto blob = GetState();
			_stateGeneration.fetch_add(1, std::memory_order_release);
			if (blob.empty()) return false;
			output = std::move(blob);
			return true;
		}
		bool TryRestoreLiveState(const std::vector<std::uint8_t>& blob)
		{
			if (_blobSdkActive.load(std::memory_order_acquire)) return false;
			std::uint32_t expected = 0;
			if (!_stateAccess.compare_exchange_strong(expected, ExclusiveState, std::memory_order_acq_rel)) return false;
			ExclusiveRelease release{_stateAccess};
			SetState(blob);
			_stateGeneration.fetch_add(1, std::memory_order_release);
			return true;
		}

	public:
		// Bounded atomic operations only; no retries, waits or allocations.
		class PluginCallGuard
		{
		public:
			explicit PluginCallGuard(const IVstPlugin& plugin, bool midiRecovery = false) noexcept : _access(plugin._stateAccess), _plugin(plugin)
			{
				const auto previous = _access.fetch_add(1u, std::memory_order_acq_rel);
				_entered = !(previous & ExclusiveState)
					&& !plugin._blobSdkActive.load(std::memory_order_acquire);
				if (!_entered)
				{
					if (midiRecovery) _plugin._pendingMidiPanic.store(true, std::memory_order_release);
					_access.fetch_sub(1u, std::memory_order_release);
				}
			}
			~PluginCallGuard() { if (_entered) _access.fetch_sub(1u, std::memory_order_release); }
			PluginCallGuard(const PluginCallGuard&) = delete;
			PluginCallGuard& operator=(const PluginCallGuard&) = delete;
			explicit operator bool() const noexcept { return _entered; }
		private:
			std::atomic<std::uint32_t>& _access;
			const IVstPlugin& _plugin;
			bool _entered = false;
		};
		// Callback owner only, inside an acquired guard. Recover skipped note-offs
		// before normal MIDI/process delivery. Concrete adapters reserve the panic
		// events in their fixed buffers and bypass VST3 parameter CC mappings.
		void RequestMidiPanic() noexcept { _pendingMidiPanic.store(true, std::memory_order_release); }
		void FlushPendingMidiPanic() noexcept
		{
			// Retain recovery while bypassed; queued events are not processed then.
			if (!IsLoaded() || IsBypassed()) return;
			if (_pendingMidiPanic.exchange(false, std::memory_order_acq_rel)) SendMidiPanicImmediately();
		}
	protected:
		// Raw export/import callers still require audio quiescence. Track their
		// SDK activity independently of live exclusive entry so pumped editor
		// calls defer, and live wrappers can invoke the same raw implementation.
		class BlobSdkCallGuard
		{
		public:
			explicit BlobSdkCallGuard(const IVstPlugin& plugin) noexcept : _active(plugin._blobSdkActive)
			{
				bool expected = false;
				_entered = _active.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
			}
			~BlobSdkCallGuard() { if (_entered) _active.store(false, std::memory_order_release); }
			BlobSdkCallGuard(const BlobSdkCallGuard&) = delete;
			BlobSdkCallGuard& operator=(const BlobSdkCallGuard&) = delete;
			explicit operator bool() const noexcept { return _entered; }
		private:
			std::atomic<bool>& _active;
			bool _entered = false;
		};

		// Non-RT housekeeping mutates adapter metadata read by SDK/audio calls.
		// A nested message pump or an active reader must defer it, never wait.
		class ExclusivePluginCallGuard
		{
		public:
			explicit ExclusivePluginCallGuard(const IVstPlugin& plugin) noexcept : _access(plugin._stateAccess)
			{
				if (plugin._blobSdkActive.load(std::memory_order_acquire)) return;
				std::uint32_t expected = 0;
				_entered = _access.compare_exchange_strong(expected, ExclusiveState, std::memory_order_acq_rel);
			}
			~ExclusivePluginCallGuard() { if (_entered) _access.fetch_and(~ExclusiveState, std::memory_order_release); }
			ExclusivePluginCallGuard(const ExclusivePluginCallGuard&) = delete;
			ExclusivePluginCallGuard& operator=(const ExclusivePluginCallGuard&) = delete;
			explicit operator bool() const noexcept { return _entered; }
		private:
			std::atomic<std::uint32_t>& _access;
			bool _entered = false;
		};

		virtual void SendMidiPanicImmediately() noexcept
		{
			for (std::uint8_t channel = 0; channel < 16; ++channel)
				SendMidiEvent({0, static_cast<std::uint8_t>(0xb0u | channel), 123, 0}, true);
		}
	private:
		// Writers: rejected MIDI/processing guards only; editor and parameter
		// readers never request panic. Skipped processing conservatively recovers
		// possibly queued note-offs before the next block clears its events. Reader/reset: callback reentry;
		// scalar atomic, no ownership/destruction crosses the callback.
		mutable std::atomic<bool> _pendingMidiPanic{false};
		// Non-RT raw blob owner writes; SDK/editor guards and close timers read.
		// It marks activity, and does not replace raw callers' audio quiescence.
		mutable std::atomic<bool> _blobSdkActive{false};
		static constexpr std::uint32_t ExclusiveState = 0x80000000u;
		struct ExclusiveRelease
		{
			std::atomic<std::uint32_t>& Access;
			// Preserve a rejected reader's transient count so its decrement cannot
			// underflow if it overlaps exclusive release.
			~ExclusiveRelease() { Access.fetch_and(~ExclusiveState, std::memory_order_release); }
		};
		// Readers: callback/editor guards. Writer: UI state operation; atomics
		// serialize entry. Lifetime follows the owner snapshot, no RT reclamation.
		mutable std::atomic<std::uint32_t> _stateAccess{0};
		mutable std::atomic<std::uint64_t> _stateGeneration{0};
		std::string _stateId = utils::GetGuid();
	public:


		// Pre-initialise the plugin DLL on the UI thread before Load() runs on
		// the job thread. No-op (returns true) when not needed.
		// Must be called from the Win32 UI (message-pump) thread only.
		virtual bool PreInit(const std::wstring& path) = 0;

		// Load the plugin from path.
		// sampleRate, blockSize and numChannels are forwarded to the plugin.
		// Returns true on success.
		virtual bool Load(const std::wstring& path,
			float sampleRate,
			unsigned int blockSize,
			unsigned int numChannels,
			HostedLayoutMode layoutMode = HostedLayoutMode::Exact) = 0;

		// Unload the plugin and release all resources.
		// Do NOT call while ProcessBlock may be running.
		virtual void Unload() = 0;
		// UI/control owner only with processing, MIDI dispatch and job work excluded.
		// Stops processing, changes geometry, then resumes the existing processor.
		virtual bool ReconfigureAudioFormat(float sampleRate, unsigned int blockSize) { return !IsLoaded(); }

		// Process numSamples of mono audio in-place.  Real-time safe.
		virtual void ProcessBlock(float* monoBuf, int32_t numSamples) noexcept = 0;

		// Process numSamples of stereo audio in-place.  Real-time safe.
		virtual void ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept = 0;

		// Process numSamples of an exact-match multichannel bus in-place.  Real-time safe.
		virtual void ProcessBlockMulti(float* const* channelBufs, int32_t numChannels, int32_t numSamples) noexcept = 0;

		// Set / get a plugin parameter by index. Real-time safe: a single opcode
		// dispatch into the hosted plugin, no allocation. Used by the audio-thread
		// automation player to drive parameters from recorded control curves.
		virtual void SetParameter(unsigned int index, float value) noexcept = 0;
		virtual float GetParameter(unsigned int index) const noexcept = 0;

		// Called once per block before ProcessBlock to supply host transport/tempo
		// context.  Real-time safe.  Default is a no-op (e.g. VST3 uses its own
		// mechanism).
		virtual void UpdateHostTime(const HostTimeState& /*state*/) noexcept {}

		// Start a new audio block for sample-accurate MIDI delivery. Real-time safe.
		virtual void BeginMidiBlock(std::uint32_t blockStartSample,
			std::uint32_t numSamples) noexcept = 0;

		// Queue a MIDI event for the current block. event.sampleOffset is an
		// absolute sample position. Real-time safe.
		virtual void SendMidiEvent(const midi::MidiEvent& event,
			bool isRealtime) noexcept = 0;

		// False means the fixed input queue rejected this event.
		virtual bool TrySendMidiEvent(const midi::MidiEvent& event, bool isRealtime) noexcept
		{
			SendMidiEvent(event, isRealtime);
			return true;
		}
		// Queried by the route owner after ProcessBlockMulti.
		virtual bool MidiBlockWasProcessed() const noexcept { return true; }

		// Non-RT host pump, also used when the editor is closed.
		virtual void PollPendingChanges() const noexcept {}

		// Open the plugin's GUI editor as a child of parentHwnd.
		// Must be called from the main/UI thread only.
		virtual bool OpenEditor(HWND parentHwnd) = 0;

		// Close the plugin's GUI editor.
		// Must be called from the main/UI thread only.
		virtual void CloseEditor() = 0;

		// Called periodically by the host's idle timer to let the plugin
		// update its editor GUI (VST2: dispatches effEditIdle; VST3: no-op).
		// Must be called from the main/UI thread only.
		virtual void IdleEditor() noexcept {}

		// Returns the size the editor requested, or {0,0} if no editor.
		virtual utils::Size2d GetEditorSize() const noexcept = 0;

		virtual bool IsLoaded() const noexcept = 0;
		virtual const std::string& Name() const noexcept = 0;

		virtual void SetBypassed(bool bypass) noexcept = 0;
		virtual bool IsBypassed() const noexcept = 0;

		// Advertised inherent processing latency in samples. VST2 reads
		// AEffect::initialDelay; VST3 reads a cached processor latency refreshed
		// off the callback after notifications. Real-time safe, with no dispatch.
		// Recorded rendering compensates path differences outside source cursors;
		// direct monitoring and NINJAM export retain their own timing domains.
		virtual int GetLatencySamples() const noexcept { return 0; }

		// Capture the full plugin state as an opaque byte blob.
		// VST2: uses effGetChunk (bank-level, index=0) when the plugin supports
		//        program chunks; otherwise serialises all parameters as float32.
		// Returns an empty vector when the plugin is not loaded or has no state.
		// Not RT-safe — call from a non-RT (job/UI) thread only.
		virtual std::vector<std::uint8_t> GetState() const { return {}; }

		// Restore plugin state from a blob previously returned by GetState().
		// No-op when the blob is empty or the plugin is not loaded.
		// Not RT-safe — call from a non-RT (job/UI) thread only.
		virtual void SetState(const std::vector<std::uint8_t>& /*blob*/) {}
	};

	// Records the most recently host-automated plugin parameter so the UI thread
	// can wire it to a MIDI automation lane ("MIDI learn" target half), and so the
	// non-RT MIDI pump can record live editor-driven automation while automation
	// record mode is held.
	//
	// Threading: written on the audio/UI thread inside Vst2Plugin's
	// audioMasterAutomate host callback when the user touches a parameter in a
	// plugin editor; read on the non-audio (UI/action/job) thread when the user
	// presses the wire key or when the MIDI pump consumes editor-origin events.
	// Each field is independently atomic; readers must tolerate a brief torn
	// triple, which is acceptable because wiring only happens after the user
	// deliberately stops touching the control. Sequence is bumped (release) after
	// the triple is stored so a consumer that acquire-loads Sequence observes a
	// coherent Plugin/ParameterIndex/Value and can detect fresh events by change.
	struct LastTouchedParameter
	{
		std::atomic<IVstPlugin*>   Plugin{ nullptr };
		std::atomic<unsigned int>  ParameterIndex{ 0u };
		std::atomic<float>         Value{ 0.0f };
		std::atomic<std::uint64_t> Sequence{ 0u };
	};
	extern LastTouchedParameter _lastTouchedParam;

	// Publish a host automation touch event into the shared registry.
	// Writers store the triple first, then release-bump Sequence so readers can
	// acquire-load Sequence and observe coherent Plugin/ParameterIndex/Value.
	void PublishLastTouchedParameter(IVstPlugin* plugin,
		unsigned int parameterIndex,
		float value) noexcept;

	// Factory: creates the correct plugin type based on file extension.
	// Extension ".dll"  -> Vst2Plugin (VST2)
	// Any other extension (e.g. ".vst3") -> Vst3Plugin (VST3)
	std::shared_ptr<IVstPlugin> MakePluginForPath(const std::wstring& path);
	// Queue a plugin for destruction on the UI thread.
	// VST3 plugins must be destroyed on the UI (message-pump) thread to avoid
	// crashing the host.  Vst2Plugin may also be queued here for uniformity.
	// Safe to call from any thread.
	void QueueForUiThreadDestroy(std::shared_ptr<IVstPlugin> plugin);

	// Drop all queued plugin refs.  Must be called from the UI thread.
	// Returns the number of plugins released.
	std::size_t DrainUiThreadDestroyQueue() noexcept;
}
