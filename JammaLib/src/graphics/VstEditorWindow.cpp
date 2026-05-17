///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstEditorWindow.h"
#include "../vst/VstPlugin.h"
#include "../vst/VstDiagnostics.h"
#include <sstream>

using namespace graphics;
using namespace actions;

namespace
{
	std::string PointerString(const void* ptr)
	{
		std::ostringstream ss;
		ss << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(ptr);
		return ss.str();
	}

	const char* WindowMessageName(UINT message)
	{
		switch (message)
		{
		case WM_NCCREATE: return "WM_NCCREATE";
		case WM_CREATE: return "WM_CREATE";
		case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
		case WM_WINDOWPOSCHANGING: return "WM_WINDOWPOSCHANGING";
		case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
		case WM_SIZE: return "WM_SIZE";
		case WM_PAINT: return "WM_PAINT";
		case WM_NCPAINT: return "WM_NCPAINT";
		case WM_ERASEBKGND: return "WM_ERASEBKGND";
		case WM_TIMER: return "WM_TIMER";
		case WM_SETFOCUS: return "WM_SETFOCUS";
		case WM_KILLFOCUS: return "WM_KILLFOCUS";
		case WM_ACTIVATE: return "WM_ACTIVATE";
		case WM_CLOSE: return "WM_CLOSE";
		case WM_DESTROY: return "WM_DESTROY";
		default: return nullptr;
		}
	}

	void LogWindowMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		const auto* name = WindowMessageName(message);
		if (!name)
			return;

		vst::VstDiagnostics::Log("VstEditorWindow", "window-message",
			std::string(name) + ", hwnd=" + PointerString(hWnd)
			+ ", wParam=" + std::to_string(static_cast<unsigned long long>(wParam))
			+ ", lParam=" + std::to_string(static_cast<long long>(lParam)));
	}
}

VstEditorWindow::VstEditorWindow() :
	_editorWnd(nullptr),
	_editorHostWnd(nullptr),
	_plugin(nullptr)
{
}

VstEditorWindow::~VstEditorWindow()
{
	Destroy();
}

bool VstEditorWindow::Create(HINSTANCE hInstance,
	std::shared_ptr<vst::VstPlugin> plugin,
	HWND /*parentHwnd*/)
{
	if (!plugin || !plugin->IsLoaded())
		return false;

	_plugin = plugin;
	vst::VstDiagnostics::Log("VstEditorWindow", "create-request", std::string("plugin=") + plugin->Name());

	// Build a title from the plugin name.
	auto nameStr = plugin->Name();
	auto nameLen = nameStr.size();
	std::wstring title(nameLen + 1, L'\0');
	size_t converted = 0;
	mbstowcs_s(&converted, title.data(), nameLen + 1, nameStr.c_str(), nameLen);
	if (converted > 0)
		title.resize(converted - 1);

	// Register the window class with editorhost-compatible style the first
	// time. CS_DBLCLKS matches the reference implementation. hbrBackground=nullptr
	// prevents Win32 from filling the client area with a colour brush, which
	// would paint over the plugin's child window content.
	{
		WNDCLASSEX existing{};
		existing.cbSize = sizeof(existing);
		if (!GetClassInfoEx(hInstance, _ClassName, &existing))
		{
			WNDCLASSEX wcex{};
			wcex.cbSize = sizeof(wcex);
			wcex.style = CS_DBLCLKS;
			wcex.lpfnWndProc = VstEditorWindow::WindowProcedure;
			wcex.hInstance = hInstance;
			wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
			wcex.hbrBackground = nullptr;
			wcex.lpszClassName = _ClassName;
			if (!RegisterClassEx(&wcex))
			{
				vst::VstDiagnostics::Log("VstEditorWindow", "class-register-failed",
					std::string("error=") + std::to_string(GetLastError()));
				_plugin.reset();
				return false;
			}
		}
	}

	// WS_CLIPCHILDREN prevents the frame from painting over the plugin's child
	// HWND. WS_CLIPSIBLINGS follows the editorhost convention.
	constexpr DWORD kEditorStyle =
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

	HWND wnd = CreateWindowEx(
		0,
		_ClassName,
		title.c_str(),
		kEditorStyle,
		100, 100, 600, 400,
		nullptr,
		nullptr,
		hInstance,
		this);

	if (!wnd)
	{
		vst::VstDiagnostics::Log("VstEditorWindow", "host-window-create-failed",
			std::string("error=") + std::to_string(GetLastError()));
		_plugin.reset();
		return false;
	}

	_editorWnd.store(wnd, std::memory_order_release);
	_editorHostWnd = wnd;
	vst::VstDiagnostics::Log("VstEditorWindow", "host-window-created", std::string("hwnd=") + PointerString(wnd));

	// Call attached() directly, matching the Steinberg editorhost pattern.
	// This must be called outside DispatchMessage so that any PostMessage-based
	// callbacks inside attached() can be queued and processed by the caller's
	// message pump after Create() returns.
	vst::VstDiagnostics::Log("VstEditorWindow", "calling-open-editor", std::string("hwnd=") + PointerString(wnd));
	const bool ok = _plugin->OpenEditor(wnd);
	if (!ok)
	{
		vst::VstDiagnostics::Log("VstEditorWindow", "plugin-open-failed",
			std::string("hwnd=") + PointerString(wnd));
		_editorWnd.store(nullptr, std::memory_order_release);
		_editorHostWnd = nullptr;
		_plugin.reset();
		DestroyWindow(wnd);
		return false;
	}

	// Resize the frame to the plugin's preferred client size.
	auto sz = _plugin->GetEditorSize();
	if (sz.Width > 0 && sz.Height > 0)
	{
		RECT rect{ 0, 0,
			static_cast<LONG>(sz.Width),
			static_cast<LONG>(sz.Height) };
		AdjustWindowRect(&rect, kEditorStyle, FALSE);
		SetWindowPos(wnd, nullptr, 0, 0,
			rect.right - rect.left,
			rect.bottom - rect.top,
			SWP_NOMOVE | SWP_NOZORDER);
	}

	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);

	vst::VstDiagnostics::Log("VstEditorWindow", "editor-ready",
		std::string("hwnd=") + PointerString(wnd));
	return true;
}

void VstEditorWindow::Destroy()
{
	HWND wnd = _editorWnd.exchange(nullptr, std::memory_order_acq_rel);
	vst::VstDiagnostics::Log("VstEditorWindow", "destroy-request",
		std::string("hwnd=") + PointerString(wnd));

	if (wnd)
	{
		// CloseEditor (IPlugView::removed) before DestroyWindow so the plugin
		// can clean up while the HWND is still valid.
		if (_plugin)
			_plugin->CloseEditor();

		if (IsWindow(wnd))
			DestroyWindow(wnd);  // WM_DESTROY fires; _editorWnd is null, skips CloseEditor
	}

	_plugin.reset();
	_editorHostWnd = nullptr;
}

void VstEditorWindow::ResizeEditorHostWindow() noexcept
{
	// Plug-in is parented directly to the editor frame; nothing to size here.
}

void VstEditorWindow::OnAction(const actions::WindowAction& action)
{
	switch (action.WindowEventType)
	{
	case WindowAction::DESTROY:
		_editorWnd.store(nullptr, std::memory_order_release);
		_editorHostWnd = nullptr;
		break;

	default:
		break;
	}
}

LRESULT CALLBACK VstEditorWindow::WindowProcedure(HWND hWnd,
	UINT message,
	WPARAM wParam,
	LPARAM lParam) noexcept
{
	VstEditorWindow* self = nullptr;

	if (message == WM_NCCREATE)
	{
		self = reinterpret_cast<VstEditorWindow*>(
			reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	else
	{
		self = reinterpret_cast<VstEditorWindow*>(
			GetWindowLongPtr(hWnd, GWLP_USERDATA));
	}

	if (!self)
		return DefWindowProc(hWnd, message, wParam, lParam);

	LogWindowMessage(hWnd, message, wParam, lParam);

	switch (message)
	{
	case WM_ERASEBKGND:
		// Suppress background painting. The plugin's child HWND owns its own
		// rendering; letting Win32 fill the parent area would paint over it.
		return TRUE;

	case WM_PAINT:
	{
		// Validate the update region without drawing anything.
		PAINTSTRUCT ps{};
		BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
		return 0;
	}

	case WM_SIZE:
		self->ResizeEditorHostWindow();
		return 0;

	case WM_CLOSE:
	{
		// Atomically take ownership of the HWND before calling CloseEditor.
		// The exchange guards against re-entrant WM_CLOSE if the plugin pumps
		// messages internally during IPlugView::removed().
		HWND owned = self->_editorWnd.exchange(nullptr, std::memory_order_acq_rel);
		if (!owned)
			return 0;

		self->_editorHostWnd = nullptr;
		if (self->_plugin)
			self->_plugin->CloseEditor();

		DestroyWindow(hWnd);
		return 0;
	}

	case WM_DESTROY:
	{
		// CloseEditor was already called from WM_CLOSE or Destroy().
		// Just update bookkeeping and notify any listeners.
		// NOTE: do NOT call PostQuitMessage here — the main PeekMessage loop
		// must keep running after the editor window closes.
		WindowAction action;
		action.WindowEventType = WindowAction::DESTROY;
		self->OnAction(action);
		return 0;
	}

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
