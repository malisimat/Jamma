///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <windows.h>
#include "../actions/WindowAction.h"
#include "../utils/CommonTypes.h"

namespace vst { class VstPlugin; }

namespace graphics
{
	// VstEditorWindow owns a Win32 HWND that hosts a VST3 plugin's IPlugView.
	// It creates the window via Window::CreateSimpleWindow (shared helper),
	// so window class registration / creation is not duplicated.
	//
	// The existing wWinMain PeekMessage/TranslateMessage/DispatchMessage loop
	// automatically dispatches messages to this window's WNDPROC once it is
	// created, so no changes to Main.cpp are required.
	//
	// Usage (main / UI thread only):
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
		// hInstance   – the application HINSTANCE.
		// plugin      – the already-loaded VstPlugin whose editor to show.
		// parentHwnd  – optional parent (pass nullptr for a top-level window).
		// Returns true on success.
		bool Create(HINSTANCE hInstance,
			std::shared_ptr<vst::VstPlugin> plugin,
			HWND parentHwnd = nullptr);

		// Detach the plugin view and destroy the HWND.
		void Destroy();

		bool IsOpen() const noexcept { return _editorWnd != nullptr; }

		// Called by the window's WNDPROC for WM_SIZE and WM_CLOSE.
		void OnAction(const actions::WindowAction& action);

		static LRESULT CALLBACK WindowProcedure(HWND hWnd, UINT message,
			WPARAM wParam, LPARAM lParam) noexcept;

	private:
		static constexpr LPCWSTR _ClassName = L"JammaVstEditorWindow";

		HWND _editorWnd;
		std::shared_ptr<vst::VstPlugin> _plugin;
	};
}
