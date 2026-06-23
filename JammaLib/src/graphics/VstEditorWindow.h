///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <windows.h>
#include "../actions/WindowAction.h"
#include "../utils/CommonTypes.h"

namespace vst { class IVstPlugin; }

namespace graphics
{
	// VstEditorWindow owns a Win32 HWND that hosts a VST plugin editor
	// (VST3 via IPlugView::attached; VST2 via effEditOpen).
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
		static constexpr UINT MessageVst2SizeWindow = WM_APP + 0x120;
		static constexpr UINT MessageVst2Idle = WM_APP + 0x121;

		VstEditorWindow();
		~VstEditorWindow();

		// Not copyable
		VstEditorWindow(const VstEditorWindow&) = delete;
		VstEditorWindow& operator=(const VstEditorWindow&) = delete;

	public:
		// Create the editor window and attach the plugin view.
		// Must be called from the main thread, NOT from within DispatchMessage.
		// hInstance   – the application HINSTANCE.
		// plugin      – the already-loaded plugin (VST2 or VST3) whose editor to show.
		// parentHwnd  – reserved, pass nullptr.
		// Returns true on success.
		bool Create(HINSTANCE hInstance,
			std::shared_ptr<vst::IVstPlugin> plugin,
			HWND parentHwnd = nullptr);

		// Detach the plugin view and destroy the HWND.
		// Must be called from the main thread.
		void Destroy();

		bool IsOpen() const noexcept { return _editorWnd.load() != nullptr; }
		const std::shared_ptr<vst::IVstPlugin>& Plugin() const noexcept { return _plugin; }
		HWND EditorHwnd() const noexcept { return _editorWnd.load(std::memory_order_acquire); }

		// Called by the window's WNDPROC for WM_SIZE.
		void OnAction(const actions::WindowAction& action);
		void ResizeEditorHostWindow() noexcept;

		static LRESULT CALLBACK WindowProcedure(HWND hWnd, UINT message,
			WPARAM wParam, LPARAM lParam) noexcept;

		// WH_CALLWNDPROCRET hook proc: dispatches effEditIdle after WM_MOUSEMOVE
		// is processed by any child of an active editor frame window.
		static LRESULT CALLBACK CallWndRetProc(int code, WPARAM wParam,
			LPARAM lParam) noexcept;

	private:
		static constexpr LPCWSTR _ClassName = L"JammaVstEditorWindow";

		// All VST editor operations run on the main UI thread, so a plain
		// (non-atomic) vector is safe here — every access is on that thread.
		static std::vector<VstEditorWindow*> s_activeEditorWindows;
		static HHOOK s_callWndRetHook;

		std::atomic<HWND> _editorWnd;
		HWND _editorHostWnd;
		HWND _pluginChildWnd;
		std::shared_ptr<vst::IVstPlugin> _plugin;

		static BOOL CALLBACK _EnumChildrenProc(HWND hWnd, LPARAM lParam) noexcept;
		static bool _IsFastIdleMessage(UINT message) noexcept;
		void _CaptureChildWindows(std::vector<HWND>& outChildren) const;
		void _RefreshTrackedPluginChild() noexcept;
		void _ResizeEditorHostWindow(unsigned int clientWidth, unsigned int clientHeight) noexcept;
	};
}
