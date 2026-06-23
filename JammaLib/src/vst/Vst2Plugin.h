///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <memory>
#include <windows.h>
#include "IVstPlugin.h"
#include "../../include/Constants.h"

// Include the VST2 SDK headers only when VST2 support is compiled in.
#ifdef JAMMA_VST2_ENABLED
#include "vst2sdk.h"
#endif

namespace vst
{
	// Vst2Plugin hosts a single loaded VST2 effect plugin (.dll).
	//
	// Threading contract:
	//   PreInit / Load / Unload / OpenEditor / CloseEditor – non-RT thread only.
	//   ProcessBlock – audio callback only; no heap allocation, no locks.
	class Vst2Plugin final : public IVstPlugin
	{
	public:
		Vst2Plugin();
		~Vst2Plugin() override;

		Vst2Plugin(const Vst2Plugin&) = delete;
		Vst2Plugin& operator=(const Vst2Plugin&) = delete;

	public:
		// Pre-load the DLL and locate the plugin entry-point on the UI thread
		// before Load() runs on the job thread.
		// For VST2 this is lighter-weight than VST3: we only LoadLibraryW and
		// resolve the entry-point function.  No plugin code is called here.
		// Returns true on success; false leaves the object in an unloaded state.
		bool PreInit(const std::wstring& path) override;

		// Load and activate the VST2 plugin.
		// sampleRate, blockSize and numChannels are forwarded to the effect.
		// Returns true on success.
		bool Load(const std::wstring& path,
			float sampleRate,
			unsigned int blockSize,
			unsigned int numChannels,
			HostedLayoutMode layoutMode = HostedLayoutMode::Exact) override;

		// Unload the plugin and release all resources.
		// Do NOT call while ProcessBlock may be running.
		void Unload() override;

		// Process numSamples of mono audio in-place.
		// Real-time safe: no heap allocation, no locks.
		void ProcessBlock(float* monoBuf, int32_t numSamples) noexcept override;

		// Process numSamples of stereo audio in-place.
		void ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept override;

		// Process numSamples of an exact-match multichannel bus in-place.
		void ProcessBlockMulti(float* const* channelBufs, int32_t numChannels, int32_t numSamples) noexcept override;

		// Set / get a hosted parameter by index. Real-time safe (single opcode dispatch).
		void SetParameter(unsigned int index, float value) noexcept override;
		float GetParameter(unsigned int index) const noexcept override;

		void BeginMidiBlock(std::uint32_t blockStartSample,
			std::uint32_t numSamples) noexcept override;
		void SendMidiEvent(const midi::MidiEvent& event,
			bool isRealtime) noexcept override;

		// Update host transport/tempo context for the next ProcessBlock call.
		// Called once per block on the audio thread before BeginMidiBlock.
		// Real-time safe: plain struct copy, no allocation.
		void UpdateHostTime(const HostTimeState& state) noexcept override;

		// Open the plugin's GUI editor as a child of parentHwnd.
		// Must be called from the main/UI thread only.
		bool OpenEditor(HWND parentHwnd) override;

		// Close the plugin's GUI editor.
		// Must be called from the main/UI thread only.
		void CloseEditor() override;

		// Dispatch effEditIdle to let the plugin update its editor GUI.
		void IdleEditor() noexcept override;
		void OnEditorActivated() noexcept override;
		void OnEditorDeactivated() noexcept override;

		utils::Size2d GetEditorSize() const noexcept override { return _editorSize; }

		bool IsLoaded() const noexcept override { return _isLoaded; }
		const std::string& Name() const noexcept override { return _name; }

		void SetBypassed(bool bypass) noexcept override
		{
			_isBypassed.store(bypass, std::memory_order_relaxed);
		}

		bool IsBypassed() const noexcept override
		{
			return _isBypassed.load(std::memory_order_relaxed);
		}

		// Capture or restore the plugin's full state as an opaque byte blob.
		// The blob is self-describing: a 1-byte version, a 1-byte type flag
		// (0 = param array, 1 = VST2 chunk), a 4-byte LE payload size, then
		// the payload.  Returns {} when not loaded or state is unavailable.
		// Not RT-safe; call from a non-RT thread only.
		std::vector<std::uint8_t> GetState() const override;
		void SetState(const std::vector<std::uint8_t>& blob) override;

		static bool SupportsHostCanDo(const char* canDo) noexcept
		{
			if (!canDo) return false;
			const std::string_view sv(canDo);
			return (sv == "sendVstEvents") ||
				   (sv == "sendVstMidiEvent") ||
				   (sv == "sendVstTimeInfo") ||
				   (sv == "sendVstMidiEventFlagIsRealtime") ||
				   (sv == "sizeWindow");
		}

	private:
#ifdef JAMMA_VST2_ENABLED
			static constexpr size_t MaxMidiEventsPerBlock = 256u;
			struct MidiEventBlock
			{
				VstInt32 numEvents;
				VstIntPtr reserved;
				VstEvent* events[MaxMidiEventsPerBlock];
			};

		// Host callback dispatched by the plugin back to us.
		static VstIntPtr __cdecl HostCallback(AEffect* effect, VstInt32 opcode,
			VstInt32 index, VstIntPtr value, void* ptr, float opt);
			void DispatchPendingMidiEvents() noexcept;

		// Instantiate the AEffect (VSTPluginMain), run effOpen, query name and
		// channel counts, and pre-allocate scratch buffers. MUST run on the
		// thread that will host the editor (the UI thread) so plugins that bind
		// their GUI/idle machinery to the instantiating thread repaint and take
		// input correctly. Called from PreInit() (UI thread); Load() only calls
		// it as a fallback when PreInit() was skipped.
		bool _InstantiateEffect(const std::wstring& path);

		// RAII guard that snapshots the current OpenGL context on construction
		// and restores it on destruction. Plugins (e.g. Battery 4) make their
		// own GL context current during effOpen/effEditOpen/effEditIdle — these
		// run on Jamma's OpenGL render thread, so without restoring our context
		// the framebuffer becomes incomplete and the whole app paints white.
		struct GlContextScope
		{
			GlContextScope() noexcept;
			~GlContextScope();
			GlContextScope(const GlContextScope&) = delete;
			GlContextScope& operator=(const GlContextScope&) = delete;
			HGLRC _rc;
			HDC _dc;
		};

		AEffect* _effect;
			std::array<VstMidiEvent, MaxMidiEventsPerBlock> _midiEvents;
			MidiEventBlock _midiEventBlock;
			std::uint32_t _midiBlockStartSample;
			std::uint32_t _midiBlockNumSamples;
			std::uint32_t _midiEventCount;
			VstTimeInfo _timeInfo;
#endif
		float _sampleRate;
		unsigned int _blockSize;
		std::atomic<std::int64_t> _sampleFramePosition;
		HostTimeState _hostTime;

		HMODULE _moduleHandle;
		bool _isLoaded;
		std::atomic<bool> _isActivated;
		// Thread id of the most recent audio-processing call. Used by the host
		// callback to answer audioMasterGetCurrentProcessLevel correctly: the
		// plugin must hear "realtime" only when it queries from the audio thread,
		// and "user" when it queries from the UI/editor thread. Reporting
		// realtime on the UI thread makes many plugins skip editor repaints.
		std::atomic<DWORD> _audioThreadId;
		std::string _name;
		std::atomic<bool> _isBypassed;
		utils::Size2d _editorSize;
		std::atomic<HWND> _editorParentHwnd;
		std::atomic<bool> _isEditorOpen;

		// Pre-allocated audio buffers — never heap-allocated in ProcessBlock.
		std::vector<float*> _inputChannelPtrs;
		std::vector<float*> _outputChannelPtrs;
		std::vector<float> _inputScratchStorage;
		std::vector<float> _outputScratchStorage;

		int32_t _inputChannels;
		int32_t _outputChannels;
		int32_t _requestedChannels;
	};
}
