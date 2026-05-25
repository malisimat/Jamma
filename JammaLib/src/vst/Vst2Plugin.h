///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>
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

		// Open the plugin's GUI editor as a child of parentHwnd.
		// Must be called from the main/UI thread only.
		bool OpenEditor(HWND parentHwnd) override;

		// Close the plugin's GUI editor.
		// Must be called from the main/UI thread only.
		void CloseEditor() override;

		// Dispatch effEditIdle to let the plugin update its editor GUI.
		void IdleEditor() noexcept override;

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

	private:
#ifdef JAMMA_VST2_ENABLED
		// Host callback dispatched by the plugin back to us.
		static VstIntPtr __cdecl HostCallback(AEffect* effect, VstInt32 opcode,
			VstInt32 index, VstIntPtr value, void* ptr, float opt);

		AEffect* _effect;
#endif

		HMODULE _moduleHandle;
		bool _isLoaded;
		std::atomic<bool> _isActivated;
		std::string _name;
		std::atomic<bool> _isBypassed;
		utils::Size2d _editorSize;

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
