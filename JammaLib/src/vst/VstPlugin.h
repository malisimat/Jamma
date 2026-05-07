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

namespace vst
{
	// VstPlugin hosts a single loaded VST3 effect plugin.
	//
	// Threading contract:
	//   Load / Unload / OpenEditor / CloseEditor – call only from a non-RT
	//   thread (e.g. the job thread or main thread).
	//
	//   ProcessBlock – called only from the audio callback while the Scene
	//   _audioMutex is held.  No heap allocation, no locks.
	class VstPlugin
	{
	public:
		VstPlugin();
		~VstPlugin();

		// Not copyable or moveable
		VstPlugin(const VstPlugin&) = delete;
		VstPlugin& operator=(const VstPlugin&) = delete;

	public:
		// Load a VST3 plugin from path (the .vst3 bundle or DLL path).
		// sampleRate, blockSize and numChannels are forwarded to the plugin's
		// IAudioProcessor::setupProcessing call.
		// Returns true on success.  Thread-safe relative to non-RT callers; do
		// NOT call while ProcessBlock may be running concurrently.
		bool Load(const std::wstring& path,
			float sampleRate,
			unsigned int blockSize,
			unsigned int numChannels);

		// Unload the plugin and release all resources.
		// Do NOT call while ProcessBlock may be running.
		void Unload();

		// Process numSamples of mono audio in-place.
		// monoBuf is read and overwritten.  No-op when not loaded or bypassed.
		// Real-time safe: no heap allocation, no locks.
		void ProcessBlock(float* monoBuf, int32_t numSamples) noexcept;

		// Open the plugin's GUI editor as a child of parentHwnd.
		// Must be called from the main/UI thread only.
		// Returns true if the editor was opened successfully.
		bool OpenEditor(HWND parentHwnd);

		// Close the plugin's GUI editor.
		// Must be called from the main/UI thread only.
		void CloseEditor();

		// Returns the size the editor requested, or {0,0} if no editor /
		// editor not yet opened.
		utils::Size2d GetEditorSize() const noexcept;

		bool IsLoaded() const noexcept { return _isLoaded; }
		const std::string& Name() const noexcept { return _name; }

		void SetBypassed(bool bypass) noexcept
		{
			_isBypassed.store(bypass, std::memory_order_relaxed);
		}

		bool IsBypassed() const noexcept
		{
			return _isBypassed.load(std::memory_order_relaxed);
		}

	private:
		class Impl;

		bool _isLoaded;
		std::string _name;
		std::atomic<bool> _isBypassed;
		utils::Size2d _editorSize;
		HMODULE _moduleHandle;
		std::unique_ptr<Impl> _impl;
	};
}
