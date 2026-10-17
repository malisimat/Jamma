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

BOOL CALLBACK VstEditorWindow::_EnumChildrenProc(HWND hWnd, LPARAM lParam) noexcept
{
	auto* outChildren = reinterpret_cast<std::vector<HWND>*>(lParam);
	if (outChildren)
		outChildren->push_back(hWnd);
	return TRUE;
}

void VstEditorWindow::_CaptureChildWindows(std::vector<HWND>& outChildren) const
{
	outChildren.clear();
	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd)
		return;

	for (HWND child = GetWindow(wnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		if (IsWindow(child))
			outChildren.push_back(child);
	}
}

void VstEditorWindow::_RefreshTrackedPluginChild() noexcept
{
	_pluginChildWnd = nullptr;

	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd))
		return;

	// The plugin reparents/creates its editor as a direct child of the frame
	// window (matching the Steinberg minihost convention). The first direct
	// child is the plugin's editor window.
	for (HWND child = GetWindow(wnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		if (GetParent(child) == wnd)
		{
			_pluginChildWnd = child;
			return;
		}
	}
}

void VstEditorWindow::_ResizeFrameToClient(unsigned int clientWidth, unsigned int clientHeight) noexcept
{
	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd))
		return;

	if (clientWidth == 0u || clientHeight == 0u)
		return;

	// Grow the non-client frame so the client area exactly fits the plugin's
	// requested size, then resize the frame. Mirrors minihost: AdjustWindowRectEx
	// followed by SetWindowPos.
	RECT frameRect{ 0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight) };
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(wnd, GWL_STYLE));
	const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(wnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&frameRect, style, FALSE, exStyle))
		return;

	const int frameWidth = frameRect.right - frameRect.left;
	const int frameHeight = frameRect.bottom - frameRect.top;
	SetWindowPos(wnd, nullptr, 0, 0, frameWidth, frameHeight,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	if (_pluginChildWnd && IsWindow(_pluginChildWnd))
	{
		SetWindowPos(_pluginChildWnd, nullptr, 0, 0,
			static_cast<int>(clientWidth), static_cast<int>(clientHeight),
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
	_ClampToWorkArea();
}

void VstEditorWindow::_ClampToWorkArea() noexcept
{
	const auto wnd = EditorHwnd();
	RECT frame{};
	MONITORINFO monitor{};
	monitor.cbSize = sizeof(monitor);
	if (!wnd || !GetWindowRect(wnd, &frame)
		|| !GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
	const auto& work = monitor.rcWork;
	const LONG width = (std::min)(frame.right - frame.left, work.right - work.left);
	const LONG height = (std::min)(frame.bottom - frame.top, work.bottom - work.top);
	if (width <= 0 || height <= 0) return;
	const LONG x = std::clamp(frame.left, work.left, work.right - width);
	const LONG y = std::clamp(frame.top, work.top, work.bottom - height);
	SetWindowPos(wnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

VstEditorWindow::VstEditorWindow() :
	_editorWnd(nullptr),
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

	_plugin = plugin;

	// Build a title from the plugin name.
	auto nameStr = plugin->Name();
	auto nameLen = nameStr.size();
	std::wstring title(nameLen + 1, L'\0');
	size_t converted = 0;
	mbstowcs_s(&converted, title.data(), nameLen + 1, nameStr.c_str(), nameLen);
	if (converted > 0)
		title.resize(converted - 1);

	// Register the window class. CS_DBLCLKS matches the reference implementation.
	// hbrBackground=nullptr prevents Win32 from filling the client area with a
	// colour brush, which would paint over the plugin's child window content.
	{
		WNDCLASSEXW existing{};
		existing.cbSize = sizeof(existing);
		if (!GetClassInfoExW(hInstance, _ClassName, &existing))
		{
			WNDCLASSEXW wcex{};
			wcex.cbSize = sizeof(wcex);
			wcex.style = CS_DBLCLKS;
			wcex.lpfnWndProc = VstEditorWindow::WindowProcedure;
			wcex.hInstance = hInstance;
			wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
			wcex.hbrBackground = nullptr;
			wcex.lpszClassName = _ClassName;
			if (!RegisterClassExW(&wcex))
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

	HWND wnd = CreateWindowExW(
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
	_pluginChildWnd = nullptr;

	std::vector<HWND> childrenBeforeOpen;
	_CaptureChildWindows(childrenBeforeOpen);

	// Open the editor directly into the frame window, exactly as the Steinberg
	// minihost sample does (effEditOpen with the host HWND as parent). The plugin
	// creates its editor as a direct child of this frame. Because the plugin's
	// parent is the frame, host callbacks (audioMasterSizeWindow / idle /
	// updateDisplay) post their notifications straight to this frame's window
	// procedure, which understands the MessageVst2* messages.
	const bool ok = _plugin->OpenEditor(wnd);
	if (!ok || !IsWindow(wnd))
	{
		_editorWnd.store(nullptr, std::memory_order_release);
		_plugin.reset();
		if (IsWindow(wnd))
			DestroyWindow(wnd);
		return false;
	}

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

	// Resize the frame to the plugin's preferred client size, matching minihost:
	// effEditGetRect (queried inside OpenEditor) then AdjustWindowRectEx +
	// SetWindowPos.
	auto sz = _plugin->GetEditorSize();
	if (sz.Width > 0 && sz.Height > 0)
		_ResizeFrameToClient(sz.Width, sz.Height);
	_ClampToWorkArea();

	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);
	if (!_pluginChildWnd || !IsWindow(_pluginChildWnd))
		_RefreshTrackedPluginChild();

	// Drive periodic effEditIdle dispatches so VST2 plugins can repaint
	// dynamic controls (sliders, meters, etc.) independently of mouse events.
	// 20 ms (~50 Hz) matches the minihost idle cadence.
	SetTimer(wnd, 1, 20, nullptr);

	return true;
}

void VstEditorWindow::_DeferClose()
{
 _closePending = true;
 // The manager may remove its reference while a state call pumps messages.
 // Keep HWND userdata alive until the existing timer completes the close.
 if (!_deferredCloseOwner) _deferredCloseOwner = weak_from_this().lock();
}

void VstEditorWindow::_TryClosePending()
{
 if (!_closePending || _idleActive
  || (_plugin && _plugin->IsStateOperationActive())) return;
 Destroy();
}

void VstEditorWindow::_IdlePlugin()
{
 if (_idleActive || !_plugin) return;
 struct IdleRelease
 {
  bool& Active;
  ~IdleRelease() { Active = false; }
 };
 {
  _idleActive = true;
  IdleRelease release{_idleActive};
  const auto plugin = _plugin;
  plugin->IdleEditor();
 }
 _TryClosePending();
}
void VstEditorWindow::Destroy()
{
	if (_closing) return;
	if (_idleActive || (_plugin && _plugin->IsStateOperationActive()))
	{
		_DeferClose();
		return; // Keep the HWND valid until the SDK call has returned.
	}
	const auto keepAlive = weak_from_this().lock();
	const auto deferredOwner = std::move(_deferredCloseOwner);
	_closing = true;
	_closePending = false;
	const auto plugin = _plugin;
	HWND wnd = _editorWnd.exchange(nullptr, std::memory_order_acq_rel);

	if (wnd)
	{
		// CloseEditor (effEditClose) before DestroyWindow so the plugin can
		// clean up while the HWND is still valid.
		if (plugin)
			plugin->CloseEditor();

		if (IsWindow(wnd))
			DestroyWindow(wnd);  // WM_DESTROY fires; _editorWnd is null, skips CloseEditor
	}

	_plugin.reset();
	_pluginChildWnd = nullptr;
	_closing = false;
}

void VstEditorWindow::ResizePluginChild() noexcept
{
	const HWND wnd = _editorWnd.load(std::memory_order_acquire);
	if (!wnd || !IsWindow(wnd))
		return;

	if (!_pluginChildWnd || !IsWindow(_pluginChildWnd))
		return;

	RECT clientRect{};
	if (!GetClientRect(wnd, &clientRect))
		return;

	const int clientWidth = (std::max)(0L, clientRect.right - clientRect.left);
	const int clientHeight = (std::max)(0L, clientRect.bottom - clientRect.top);
	SetWindowPos(_pluginChildWnd, nullptr, 0, 0, clientWidth, clientHeight,
		SWP_NOZORDER | SWP_NOACTIVATE);
}

void VstEditorWindow::OnAction(const actions::WindowAction& action)
{
	switch (action.WindowEventType)
	{
	case WindowAction::DESTROY:
		_editorWnd.store(nullptr, std::memory_order_release);
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

	// A plugin may close/prune its editor while pumping messages in this call.
	const auto keepAlive = self->weak_from_this().lock();

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
		self->ResizePluginChild();
		return 0;

	case WM_SETFOCUS:
		// Forward keyboard focus to the plugin's child window. (Unlike the
		// minieditor modal dialog, this is a normal top-level window, so we route
		// focus to the child explicitly.)
		if (self->_pluginChildWnd && IsWindow(self->_pluginChildWnd))
			SetFocus(self->_pluginChildWnd);
		return 0;

	case MessageVst2SizeWindow:
	{
		const auto requestedWidth = static_cast<unsigned int>((std::max)(0LL, static_cast<long long>(wParam)));
		const auto requestedHeight = static_cast<unsigned int>((std::max)(0LL, static_cast<long long>(lParam)));
		if (requestedWidth == 0u || requestedHeight == 0u)
			return 0;
		if (!self->_pluginChildWnd || !IsWindow(self->_pluginChildWnd))
			self->_RefreshTrackedPluginChild();
		self->_ResizeFrameToClient(requestedWidth, requestedHeight);
		return 1;
	}

	case MessageVst2Idle:
		self->_IdlePlugin();
		return 1;

	case WM_CLOSE:
	{
		if (self->_closing) return 0;
		if (self->_idleActive || (self->_plugin && self->_plugin->IsStateOperationActive()))
		{
			self->_DeferClose();
			return 0;
		}
		const auto deferredOwner = std::move(self->_deferredCloseOwner);
		self->_closing = true;
		self->_closePending = false;
		// Atomically take ownership of the HWND before calling CloseEditor.
		// The exchange guards against re-entrant WM_CLOSE if the plugin pumps
		// messages internally during effEditClose.
		HWND owned = self->_editorWnd.exchange(nullptr, std::memory_order_acq_rel);
		if (!owned)
		{
			self->_closing = false;
			return 0;
		}

		self->_pluginChildWnd = nullptr;
		const auto plugin = self->_plugin;
		if (plugin)
			plugin->CloseEditor();

		DestroyWindow(hWnd);
		self->_closing = false;
		return 0;
	}

	case WM_TIMER:
		self->_TryClosePending();
		if (self->IsOpen()) self->_IdlePlugin();
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
