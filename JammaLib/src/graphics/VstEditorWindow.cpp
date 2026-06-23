///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstEditorWindow.h"
#include "../vst/IVstPlugin.h"
#include <algorithm>
#include <iostream>
#include <vector>

using namespace graphics;
using namespace actions;

namespace
{
	void TraceEditorWnd(const char* event, HWND frame, HWND host, HWND child, UINT message = 0)
	{
		std::cout << "[VST EDITOR TRACE] tid=" << GetCurrentThreadId()
			<< " event=" << event
			<< " frame=" << frame
			<< " host=" << host
			<< " child=" << child;
		if (message)
			std::cout << " msg=0x" << std::hex << message << std::dec;
		std::cout << std::endl;
	}
}

std::vector<VstEditorWindow*> VstEditorWindow::s_activeEditorWindows;
HHOOK VstEditorWindow::s_callWndRetHook = nullptr;

BOOL CALLBACK VstEditorWindow::_EnumChildrenProc(HWND hWnd, LPARAM lParam) noexcept
{
	auto* outChildren = reinterpret_cast<std::vector<HWND>*>(lParam);
	if (outChildren)
		outChildren->push_back(hWnd);
	return TRUE;
}

bool VstEditorWindow::_IsFastIdleMessage(UINT message) noexcept
{
	switch (message)
	{
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDBLCLK:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	case WM_SETFOCUS:
	case WM_MOUSEACTIVATE:
	case WM_ACTIVATE:
		return true;
	default:
		return false;
	}
}

void VstEditorWindow::_CaptureChildWindows(std::vector<HWND>& outChildren) const
{
	outChildren.clear();
	const HWND wnd = (_editorHostWnd && IsWindow(_editorHostWnd))
		? _editorHostWnd
		: _editorWnd.load(std::memory_order_acquire);
	if (!wnd)
		return;
	EnumChildWindows(wnd, _EnumChildrenProc, reinterpret_cast<LPARAM>(&outChildren));
}

void VstEditorWindow::_RefreshTrackedPluginChild() noexcept
{
	_pluginChildWnd = nullptr;
	if (_editorHostWnd && IsWindow(_editorHostWnd))
	{
		for (HWND child = GetWindow(_editorHostWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
		{
			if (GetParent(child) == _editorHostWnd)
			{
				_pluginChildWnd = child;
				return;
			}
		}
	}

	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd))
		return;

	for (HWND child = GetWindow(wnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		if (child == _editorHostWnd)
			continue;
		if (GetParent(child) == wnd)
		{
			_pluginChildWnd = child;
			return;
		}
	}
}

void VstEditorWindow::_ResizeEditorHostWindow(unsigned int clientWidth, unsigned int clientHeight) noexcept
{
	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd))
		return;

	if (clientWidth == 0u || clientHeight == 0u)
		return;

	RECT frameRect{ 0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight) };
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(wnd, GWL_STYLE));
	const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(wnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&frameRect, style, FALSE, exStyle))
		return;

	const int frameWidth = frameRect.right - frameRect.left;
	const int frameHeight = frameRect.bottom - frameRect.top;
	SetWindowPos(wnd, nullptr, 0, 0, frameWidth, frameHeight,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	if (_editorHostWnd && IsWindow(_editorHostWnd))
	{
		SetWindowPos(_editorHostWnd, nullptr, 0, 0,
			static_cast<int>(clientWidth), static_cast<int>(clientHeight),
			SWP_NOZORDER | SWP_NOACTIVATE);
	}

	if (_pluginChildWnd && IsWindow(_pluginChildWnd))
	{
		SetWindowPos(_pluginChildWnd, nullptr, 0, 0,
			static_cast<int>(clientWidth), static_cast<int>(clientHeight),
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

// WH_CALLWNDPROCRET hook: fires after any window proc on this thread returns.
// For editor-related input and focus messages directed at a child of one of our
// editor frames, dispatch effEditIdle immediately so plugin repaint stays
// responsive between timer ticks.
LRESULT CALLBACK VstEditorWindow::CallWndRetProc(int code, WPARAM /*wParam*/, LPARAM lParam) noexcept
{
	if (code == HC_ACTION)
	{
		const auto* info = reinterpret_cast<const CWPRETSTRUCT*>(lParam);
		if (info->hwnd && _IsFastIdleMessage(info->message))
		{
			HWND root = GetAncestor(info->hwnd, GA_ROOT);
			for (auto* w : s_activeEditorWindows)
			{
				HWND editorHwnd = w->_editorWnd.load(std::memory_order_acquire);
				if (editorHwnd && root == editorHwnd)
				{
					// Defer idle dispatch until after the current message returns to
					// avoid re-entering plugin UI code from inside a hook callback.
					TraceEditorWnd("CallWndRet.post_idle", editorHwnd, w->_editorHostWnd, w->_pluginChildWnd, info->message);
					PostMessage(editorHwnd, MessageVst2Idle, 0, 0);
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
	_pluginChildWnd(nullptr),
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

	TraceEditorWnd("Create.begin", nullptr, nullptr, nullptr);

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
	TraceEditorWnd("Create.frame_created", wnd, nullptr, nullptr);

	_editorWnd.store(wnd, std::memory_order_release);
	_editorHostWnd = CreateWindowEx(
		0,
		L"STATIC",
		L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 1, 1,
		wnd,
		nullptr,
		hInstance,
		nullptr);
	if (!_editorHostWnd)
	{
		_editorWnd.store(nullptr, std::memory_order_release);
		_plugin.reset();
		DestroyWindow(wnd);
		return false;
	}
	TraceEditorWnd("Create.host_created", wnd, _editorHostWnd, nullptr);
	_pluginChildWnd = nullptr;

	std::vector<HWND> childrenBeforeOpen;
	_CaptureChildWindows(childrenBeforeOpen);

	// Call attached() directly, matching the Steinberg editorhost pattern.
	// This must be called outside DispatchMessage so that any PostMessage-based
	// callbacks inside attached() can be queued and processed by the caller's
	// message pump after Create() returns.
	const bool ok = _plugin->OpenEditor(_editorHostWnd);
	if (!ok || !IsWindow(wnd))
	{
		_editorWnd.store(nullptr, std::memory_order_release);
		_editorHostWnd = nullptr;
		_plugin.reset();
		if (IsWindow(wnd))
			DestroyWindow(wnd);
		return false;
	}
	TraceEditorWnd("Create.open_editor_ok", wnd, _editorHostWnd, nullptr);

	std::vector<HWND> childrenAfterOpen;
	_CaptureChildWindows(childrenAfterOpen);
	for (const auto child : childrenAfterOpen)
	{
		if (std::find(childrenBeforeOpen.begin(), childrenBeforeOpen.end(), child) == childrenBeforeOpen.end())
		{
			_pluginChildWnd = child;
			break;
		}
	}
	if (!_pluginChildWnd)
		_RefreshTrackedPluginChild();
	TraceEditorWnd("Create.child_tracked", wnd, _editorHostWnd, _pluginChildWnd);

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
		SetWindowPos(_editorHostWnd, nullptr, 0, 0,
			static_cast<int>(sz.Width), static_cast<int>(sz.Height),
			SWP_NOZORDER | SWP_NOACTIVATE);
	}

	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);
	if (!_pluginChildWnd || !IsWindow(_pluginChildWnd))
		_RefreshTrackedPluginChild();
	TraceEditorWnd("Create.after_show", wnd, _editorHostWnd, _pluginChildWnd);

	// Drive periodic effEditIdle dispatches so VST2 plugins can repaint
	// dynamic controls (sliders, meters, etc.) independently of mouse events.
	// 20 ms (~50 Hz) tracks common editor-host idle cadence.
	SetTimer(wnd, 1, 20, nullptr);

	// Register for the mouse-move hook so drags repaint immediately.
	s_activeEditorWindows.push_back(this);
	if (!s_callWndRetHook)
	{
		s_callWndRetHook = SetWindowsHookEx(WH_CALLWNDPROCRET, CallWndRetProc,
			GetModuleHandle(nullptr), GetCurrentThreadId());
		if (!s_callWndRetHook)
			std::cerr << "VstEditorWindow: SetWindowsHookEx failed, error="
				<< GetLastError() << "\n";
	}

	return true;
}

void VstEditorWindow::Destroy()
{
	TraceEditorWnd("Destroy.begin", _editorWnd.load(std::memory_order_acquire), _editorHostWnd, _pluginChildWnd);
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
	_pluginChildWnd = nullptr;
	TraceEditorWnd("Destroy.end", nullptr, nullptr, nullptr);
}

void VstEditorWindow::ResizeEditorHostWindow() noexcept
{
	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd) || !_editorHostWnd || !IsWindow(_editorHostWnd))
		return;

	RECT clientRect{};
	if (!GetClientRect(wnd, &clientRect))
		return;

	const int clientWidth = (std::max)(0L, clientRect.right - clientRect.left);
	const int clientHeight = (std::max)(0L, clientRect.bottom - clientRect.top);
	SetWindowPos(_editorHostWnd, nullptr, 0, 0, clientWidth, clientHeight,
		SWP_NOZORDER | SWP_NOACTIVATE);

	if (!_pluginChildWnd || !IsWindow(_pluginChildWnd))
		return;

	RECT hostClientRect{};
	if (!GetClientRect(_editorHostWnd, &hostClientRect))
		return;

	const int hostClientWidth = (std::max)(0L, hostClientRect.right - hostClientRect.left);
	const int hostClientHeight = (std::max)(0L, hostClientRect.bottom - hostClientRect.top);
	SetWindowPos(_pluginChildWnd, nullptr, 0, 0, hostClientWidth, hostClientHeight,
		SWP_NOZORDER | SWP_NOACTIVATE);
	TraceEditorWnd("ResizeEditorHostWindow", wnd, _editorHostWnd, _pluginChildWnd);
}

void VstEditorWindow::OnAction(const actions::WindowAction& action)
{
	switch (action.WindowEventType)
	{
	case WindowAction::DESTROY:
		_editorWnd.store(nullptr, std::memory_order_release);
		_editorHostWnd = nullptr;
		_pluginChildWnd = nullptr;
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
		TraceEditorWnd("WM_SIZE", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		self->ResizeEditorHostWindow();
		return 0;

	case WM_SETFOCUS:
		TraceEditorWnd("WM_SETFOCUS", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		if (self->_plugin)
			self->_plugin->OnEditorActivated();
		if (self->_pluginChildWnd && IsWindow(self->_pluginChildWnd))
			SetFocus(self->_pluginChildWnd);
		return 0;

	case WM_MOUSEACTIVATE:
		TraceEditorWnd("WM_MOUSEACTIVATE", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		if (self->_pluginChildWnd && IsWindow(self->_pluginChildWnd))
			SetFocus(self->_pluginChildWnd);
		return DefWindowProc(hWnd, message, wParam, lParam);

	case WM_ACTIVATE:
		TraceEditorWnd("WM_ACTIVATE", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		if (LOWORD(wParam) != WA_INACTIVE)
		{
			if (self->_plugin)
				self->_plugin->OnEditorActivated();
			if (self->_pluginChildWnd && IsWindow(self->_pluginChildWnd))
				SetFocus(self->_pluginChildWnd);
		}
		else if (self->_plugin)
		{
			self->_plugin->OnEditorDeactivated();
		}
		return DefWindowProc(hWnd, message, wParam, lParam);

	case MessageVst2SizeWindow:
	{
		TraceEditorWnd("MessageVst2SizeWindow", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		const auto requestedWidth = static_cast<unsigned int>((std::max)(0LL, static_cast<long long>(wParam)));
		const auto requestedHeight = static_cast<unsigned int>((std::max)(0LL, static_cast<long long>(lParam)));
		if (requestedWidth == 0u || requestedHeight == 0u)
			return 0;
		if (!self->_pluginChildWnd || !IsWindow(self->_pluginChildWnd))
			self->_RefreshTrackedPluginChild();
		self->_ResizeEditorHostWindow(requestedWidth, requestedHeight);
		return 1;
	}

	case MessageVst2Idle:
		TraceEditorWnd("MessageVst2Idle", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		if (self->_plugin)
			self->_plugin->IdleEditor();
		return 1;

	case WM_CLOSE:
	{
		TraceEditorWnd("WM_CLOSE", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		// Atomically take ownership of the HWND before calling CloseEditor.
		// The exchange guards against re-entrant WM_CLOSE if the plugin pumps
		// messages internally during IPlugView::removed().
		HWND owned = self->_editorWnd.exchange(nullptr, std::memory_order_acq_rel);
		if (!owned)
			return 0;

		self->_editorHostWnd = nullptr;
		self->_pluginChildWnd = nullptr;
		if (self->_plugin)
		{
			self->_plugin->OnEditorDeactivated();
			self->_plugin->CloseEditor();
		}

		DestroyWindow(hWnd);
		return 0;
	}

	case WM_TIMER:
		TraceEditorWnd("WM_TIMER", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		if (self->_plugin)
			self->_plugin->IdleEditor();
		return 0;

	case WM_DESTROY:
	{
		TraceEditorWnd("WM_DESTROY", hWnd, self->_editorHostWnd, self->_pluginChildWnd, message);
		// CloseEditor was already called from WM_CLOSE or Destroy().
		// Kill the idle timer and update bookkeeping.
		// NOTE: do NOT call PostQuitMessage here — the main PeekMessage loop
		// must keep running after the editor window closes.
		if (self->_plugin)
			self->_plugin->OnEditorDeactivated();
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
