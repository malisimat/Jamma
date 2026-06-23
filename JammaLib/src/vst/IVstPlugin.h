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

namespace vst
{
	// Snapshot of host transport/tempo state passed to each plugin before processing
	// each audio block.  Populated on the audio thread; must be trivially copyable.
	struct HostTimeState
	{
		double samplePos  = 0.0;
		double sampleRate = 44100.0;
		double tempo      = 120.0;
		int32_t bpi       = 4;
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

		// Editor focus lifecycle hooks (VST2: effEditTop/effEditSleep).
		// Called from the editor host window on activation/focus transitions.
		virtual void OnEditorActivated() noexcept {}
		virtual void OnEditorDeactivated() noexcept {}

		// Returns the size the editor requested, or {0,0} if no editor.
		virtual utils::Size2d GetEditorSize() const noexcept = 0;

		virtual bool IsLoaded() const noexcept = 0;
		virtual const std::string& Name() const noexcept = 0;

		virtual void SetBypassed(bool bypass) noexcept = 0;
		virtual bool IsBypassed() const noexcept = 0;

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
