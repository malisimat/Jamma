///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <memory>
#include <windows.h>
#include "../actions/WindowAction.h"
#include "../utils/CommonTypes.h"

namespace vst { class VstPlugin; }

namespace graphics
{
	// VstEditorWindow owns a Win32 HWND that hosts a VST3 plugin's IPlugView.
	//
	// All methods must be called from the main (UI) thread.
	//
	// IPlugView::attached() is called directly from Create(), matching the
	// Steinberg editorhost sample exactly. The host window must NOT be inside
	// DispatchMessage when Create() is called; any PostMessage-based callbacks
	// from attached() will be serviced by the caller's message pump after
	// Create() returns.
	//
	// Usage (main thread only):
	//   auto editorWnd = std::make_unique<VstEditorWindow>();
	//   editorWnd->Create(hInstance, plugin);
	//   ...
	//   editorWnd->Destroy();
	class VstEditorWindow
	{
	public:
		VstEditorWindow();
		~VstEditorWindow();

		// Not copyable
		VstEditorWindow(const VstEditorWindow&) = delete;
		VstEditorWindow& operator=(const VstEditorWindow&) = delete;

	public:
		// Create the editor window and attach the plugin view.
		// Must be called from the main thread, NOT from within DispatchMessage.
		// hInstance   – the application HINSTANCE.
		// plugin      – the already-loaded VstPlugin whose editor to show.
		// parentHwnd  – reserved, pass nullptr.
		// Returns true on success.
		bool Create(HINSTANCE hInstance,
			std::shared_ptr<vst::VstPlugin> plugin,
			HWND parentHwnd = nullptr);

		// Detach the plugin view and destroy the HWND.
		// Must be called from the main thread.
		void Destroy();

		bool IsOpen() const noexcept { return _editorWnd.load() != nullptr; }

		// Called by the window's WNDPROC for WM_SIZE.
		void OnAction(const actions::WindowAction& action);
		void ResizeEditorHostWindow() noexcept;

		static LRESULT CALLBACK WindowProcedure(HWND hWnd, UINT message,
			WPARAM wParam, LPARAM lParam) noexcept;

	private:
		static constexpr LPCWSTR _ClassName = L"JammaVstEditorWindow";

		std::atomic<HWND> _editorWnd;
		HWND _editorHostWnd;
		std::shared_ptr<vst::VstPlugin> _plugin;
	};
}
