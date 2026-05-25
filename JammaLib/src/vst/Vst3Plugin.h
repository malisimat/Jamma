///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <windows.h>
#include "../../include/Constants.h"
#include "../utils/CommonTypes.h"
#include "VstAudioBuffers.h"
#include "IVstPlugin.h"

namespace vst
{
	// Vst3Plugin hosts a single loaded VST3 effect plugin.
	//
	// Threading contract:
	//   Load / Unload / OpenEditor / CloseEditor – call only from a non-RT
	//   thread (e.g. the job thread or main thread).
	//
	//   ProcessBlock – called only from the audio callback while the Scene
	//   _audioMutex is held.  No heap allocation, no locks.
	class Vst3Plugin final : public IVstPlugin
	{
	public:
		Vst3Plugin();
		~Vst3Plugin() override;

		// Not copyable or moveable
		Vst3Plugin(const Vst3Plugin&) = delete;
		Vst3Plugin& operator=(const Vst3Plugin&) = delete;

	public:
		// Pre-initialise the plugin DLL on the UI thread before Load() runs on
		// the job thread so editor attach work stays bound to the UI thread.
		//
		// Must be called from the Win32 UI (message-pump) thread only.
		// If PreInit() succeeds, Load() will reuse the DLL handle and factory
		// it pre-loaded; it never calls LoadLibraryW or GetPluginFactory again.
		// Returns true on success; false leaves the object in an unloaded state.
		bool PreInit(const std::wstring& path) override;

		// Load a VST3 plugin from path (the .vst3 bundle or DLL path).
		// sampleRate and blockSize are forwarded to setupProcessing().
		// numChannels is the host channel request.
		// Exact mode is for full-bus station processing; MonoFlexible is for mono
		// loop buffers that may be adapted onto wider plugin buses.
		// Returns true on success.  Thread-safe relative to non-RT callers; do
		// NOT call while ProcessBlock may be running concurrently.
		bool Load(const std::wstring& path,
			float sampleRate,
			unsigned int blockSize,
			unsigned int numChannels,
			HostedLayoutMode layoutMode = HostedLayoutMode::Exact) override;

		// Unload the plugin and release all resources.
		// Do NOT call while ProcessBlock may be running.
		void Unload() override;

		// Process numSamples of mono audio in-place.
		// monoBuf is read and overwritten. Stereo plugin output is folded back to
		// mono when the negotiated output bus is wider than one channel.
		// No-op when not loaded or bypassed.
		// Real-time safe: no heap allocation, no locks.
		void ProcessBlock(float* monoBuf, int32_t numSamples) noexcept override;

		// Process numSamples of stereo audio in-place.
		// leftBuf and rightBuf are read and overwritten. If the plugin is
		// configured as mono, each channel is processed independently.
		void ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept override;

		// Process numSamples of an exact-match multichannel bus in-place.
		// channelBufs must contain numChannels writable channel buffers.
		void ProcessBlockMulti(float* const* channelBufs, int32_t numChannels, int32_t numSamples) noexcept override;

		// Open the plugin's GUI editor as a child of parentHwnd.
		// Must be called from the main/UI thread only.
		// Returns true if the editor was opened successfully.
		bool OpenEditor(HWND parentHwnd) override;

		// Close the plugin's GUI editor.
		// Must be called from the main/UI thread only.
		void CloseEditor() override;

		// Returns the size the editor requested, or {0,0} if no editor /
		// editor not yet opened.
		utils::Size2d GetEditorSize() const noexcept override;

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
		class Impl;
		void ResetLoadedObjects(bool terminateComponent);

		bool _isLoaded;
		// True only after setActive(true) + setProcessing(true) have been called.
		// ProcessBlock is a no-op until this is set, preventing process() calls
		// before the plugin's audio lifecycle has been started.
		std::atomic<bool> _isActivated;
		std::string _name;
		std::atomic<bool> _isBypassed;
		std::atomic<bool> _editorOpening;
		utils::Size2d _editorSize;
		HMODULE _moduleHandle;
		std::unique_ptr<Impl> _impl;
	};

	// Queue an IVstPlugin to be destroyed on the UI thread.
	// Declared in IVstPlugin.h; implemented in Vst3Plugin.cpp.
}
