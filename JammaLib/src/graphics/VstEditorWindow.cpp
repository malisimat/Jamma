///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstEditorWindow.h"
#include "../vst/IVstPlugin.h"
#include <algorithm>
#include <vector>

using namespace graphics;
using namespace actions;

// Thread-local tracking of active VstEditorWindow instances.
// All VST editor operations run on the main UI thread, so a plain
// (non-atomic) vector is safe here — every access is on that thread.
static std::vector<VstEditorWindow*> s_activeEditorWindows;
static HHOOK s_callWndRetHook = nullptr;

// WH_CALLWNDPROCRET hook: fires after any window proc on this thread returns.
// If the message was WM_MOUSEMOVE directed at a child of one of our editor
// frames, dispatch effEditIdle immediately so the plugin can repaint the
// updated control position without waiting for the next 50 ms timer tick.
LRESULT CALLBACK VstEditorWindow::CallWndRetProc(int code, WPARAM /*wParam*/, LPARAM lParam) noexcept
{
	if (code == HC_ACTION)
	{
		const auto* info = reinterpret_cast<const CWPRETSTRUCT*>(lParam);
		if (info->message == WM_MOUSEMOVE && info->hwnd)
		{
			HWND root = GetAncestor(info->hwnd, GA_ROOT);
			for (auto* w : s_activeEditorWindows)
			{
				HWND editorHwnd = w->_editorWnd.load(std::memory_order_acquire);
				if (editorHwnd && root == editorHwnd)
				{
					if (w->_plugin)
						w->_plugin->IdleEditor();
					break;
				}
			}
		}
	}
	return CallNextHookEx(s_callWndRetHook, code, 0, lParam);
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
	std::shared_ptr<vst::IVstPlugin> plugin,
	HWND /*parentHwnd*/)
{
	if (!plugin || !plugin->IsLoaded())
		return false;

	_plugin = plugin;

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
		_plugin.reset();
		return false;
	}

	_editorWnd.store(wnd, std::memory_order_release);
	_editorHostWnd = wnd;

	// Call attached() directly, matching the Steinberg editorhost pattern.
	// This must be called outside DispatchMessage so that any PostMessage-based
	// callbacks inside attached() can be queued and processed by the caller's
	// message pump after Create() returns.
	const bool ok = _plugin->OpenEditor(wnd);
	if (!ok || !IsWindow(wnd))
	{
		_editorWnd.store(nullptr, std::memory_order_release);
		_editorHostWnd = nullptr;
		_plugin.reset();
		if (IsWindow(wnd))
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

	// Drive periodic effEditIdle dispatches so VST2 plugins can repaint
	// dynamic controls (sliders, meters, etc.) independently of mouse events.
	// 50 ms (~20 Hz) matches common VST host practice.
	SetTimer(wnd, 1, 50, nullptr);

	// Register for the mouse-move hook so drags repaint immediately.
	s_activeEditorWindows.push_back(this);
	if (!s_callWndRetHook)
		s_callWndRetHook = SetWindowsHookEx(WH_CALLWNDPROCRET, CallWndRetProc,
			nullptr, GetCurrentThreadId());

	return true;
}

void VstEditorWindow::Destroy()
{
	// Unregister from the hook tracking list.
	auto it = std::find(s_activeEditorWindows.begin(), s_activeEditorWindows.end(), this);
	if (it != s_activeEditorWindows.end())
		s_activeEditorWindows.erase(it);
	if (s_activeEditorWindows.empty() && s_callWndRetHook)
	{
		UnhookWindowsHookEx(s_callWndRetHook);
		s_callWndRetHook = nullptr;
	}

	HWND wnd = _editorWnd.exchange(nullptr, std::memory_order_acq_rel);

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

	case WM_TIMER:
		if (self->_plugin)
			self->_plugin->IdleEditor();
		return 0;

	case WM_DESTROY:
	{
		// CloseEditor was already called from WM_CLOSE or Destroy().
		// Kill the idle timer and update bookkeeping.
		// NOTE: do NOT call PostQuitMessage here — the main PeekMessage loop
		// must keep running after the editor window closes.
		KillTimer(hWnd, 1);
		WindowAction action;
		action.WindowEventType = WindowAction::DESTROY;
		self->OnAction(action);
		return 0;
	}

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
