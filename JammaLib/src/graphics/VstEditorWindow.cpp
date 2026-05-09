///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstEditorWindow.h"
#include "Window.h"
#include "../vst/VstPlugin.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ole2.h>

namespace
{
	bool ReadVstWindowDebugFlag(const wchar_t* name)
	{
		wchar_t value[32]{};
		const auto len = GetEnvironmentVariableW(name, value, static_cast<DWORD>(_countof(value)));
		if (len == 0 || len >= _countof(value))
			return false;

		return (_wcsicmp(value, L"0") != 0)
			&& (_wcsicmp(value, L"false") != 0)
			&& (_wcsicmp(value, L"off") != 0)
			&& (_wcsicmp(value, L"no") != 0);
	}

	void AppendWindowDebugEvent(const std::string& message)
	{
		static const bool enabled = ReadVstWindowDebugFlag(L"JAMMA_VST_DEBUG");
		if (!enabled)
			return;

		wchar_t appData[MAX_PATH]{};
		if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0)
			return;

		static std::mutex logMutex;
		std::scoped_lock lock(logMutex);

		const auto logPath = std::filesystem::path(appData) / "Jamma" / "vst-debug-events.log";
		std::ofstream out(logPath, std::ios::app);
		if (!out.is_open())
			return;

		SYSTEMTIME now{};
		GetLocalTime(&now);
		out << now.wHour << ':' << now.wMinute << ':' << now.wSecond << '.' << now.wMilliseconds
			<< " [VstEditorWindow] " << message << std::endl;
	}
}

using namespace graphics;
using namespace actions;

VstEditorWindow::VstEditorWindow() :
	_editorWnd(nullptr),
	_editorHostWnd(nullptr),
	_plugin(nullptr),
	_uiThreadId(0),
	_ready(false),
	_failed(false)
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
	AppendWindowDebugEvent("Create begin");

	if (!plugin || !plugin->IsLoaded())
		return false;

	_plugin = plugin;

	// Build a title from the plugin name
	auto nameStr = plugin->Name();
	auto nameLen = nameStr.size();
	std::wstring title(nameLen + 1, L'\0');
	size_t converted = 0;
	mbstowcs_s(&converted, title.data(), nameLen + 1, nameStr.c_str(), nameLen);
	if (converted > 0)
		title.resize(converted - 1);

	// Run the entire VST editor lifecycle (window creation, IPlugView::attached,
	// message pump, IPlugView::removed, destroy) on its own dedicated UI
	// thread. Many VST3 plug-ins (notably VSTGUI-based ones such as Valhalla)
	// block inside attached() while waiting for messages to be pumped on the
	// thread that owns the parent HWND. Giving the editor its own thread lets
	// the host's main render thread keep pumping its own messages, eliminating
	// the deadlock.
	_ready.store(false, std::memory_order_release);
	_failed.store(false, std::memory_order_release);
	_uiThread = std::thread(&VstEditorWindow::UiThreadProc, this, hInstance, std::move(title));

	// Do NOT wait for the UI thread to finish attaching. The plug-in's
	// IPlugView::attached() may take a long time (or, for misbehaving
	// plug-ins, may pump messages internally). Blocking the main render
	// thread here would freeze the entire host. Return immediately; callers
	// can poll IsOpen() to learn when the editor is actually visible.
	AppendWindowDebugEvent("Create launched UI thread");
	return true;
}

void VstEditorWindow::UiThreadProc(HINSTANCE hInstance, std::wstring title)
{
	_uiThreadId.store(GetCurrentThreadId(), std::memory_order_release);

	const HRESULT oleHr = OleInitialize(nullptr);
	const bool oleOk = SUCCEEDED(oleHr);
	AppendWindowDebugEvent(oleOk ? "UI thread OleInitialize ok" : "UI thread OleInitialize failed");

	HWND wnd = Window::CreateSimpleWindow(
		hInstance,
		_ClassName,
		title.c_str(),
		WS_OVERLAPPEDWINDOW,
		100, 100, 600, 400,
		nullptr,
		VstEditorWindow::WindowProcedure,
		this);

	if (!wnd)
	{
		AppendWindowDebugEvent("UI thread CreateWindow failed");
		_failed.store(true, std::memory_order_release);
		if (oleOk)
			OleUninitialize();
		return;
	}

	_editorWnd.store(wnd, std::memory_order_release);
	_editorHostWnd = wnd;
	AppendWindowDebugEvent("UI thread host window created");

	ShowWindow(wnd, SW_SHOWNORMAL);
	UpdateWindow(wnd);
	AppendWindowDebugEvent("UI thread host window shown");

	if (!_plugin)
	{
		AppendWindowDebugEvent("UI thread no plugin");
		DestroyWindow(wnd);
		_editorWnd.store(nullptr, std::memory_order_release);
		_failed.store(true, std::memory_order_release);
		if (oleOk)
			OleUninitialize();
		return;
	}

	// Call OpenEditor on a worker thread while THIS thread (the HWND owner)
	// pumps messages. Many VST3 plug-ins do internal PostMessage/SendMessage
	// or COM marshalling round-trips inside IPlugView::attached() and require
	// the HWND owner thread to be pumping messages while attached() runs. If
	// we called OpenEditor directly here we would block our own message pump
	// and deadlock the plug-in.
	HANDLE openEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	bool openResult = false;
	std::thread openThread([this, wnd, openEvent, &openResult]() {
		const HRESULT workerOle = OleInitialize(nullptr);
		const bool workerOleOk = SUCCEEDED(workerOle);
		AppendWindowDebugEvent(workerOleOk ? "Open worker OleInitialize ok" : "Open worker OleInitialize failed");
		AppendWindowDebugEvent("Open worker calling OpenEditor");
		openResult = _plugin->OpenEditor(wnd);
		AppendWindowDebugEvent(openResult ? "Open worker OpenEditor true" : "Open worker OpenEditor false");
		if (workerOleOk)
			OleUninitialize();
		SetEvent(openEvent);
	});

	// Pump messages until the worker signals completion.
	for (;;)
	{
		const DWORD waitRes = MsgWaitForMultipleObjectsEx(
			1, &openEvent,
			INFINITE,
			QS_ALLINPUT,
			MWMO_INPUTAVAILABLE);

		if (waitRes == WAIT_OBJECT_0)
			break;

		if (waitRes == WAIT_OBJECT_0 + 1)
		{
			MSG pm{};
			while (PeekMessageW(&pm, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&pm);
				DispatchMessageW(&pm);
			}
		}
		else
		{
			AppendWindowDebugEvent("MsgWaitForMultipleObjectsEx failed");
			break;
		}
	}

	openThread.join();
	CloseHandle(openEvent);

	if (!openResult)
	{
		AppendWindowDebugEvent("UI thread OpenEditor failed");
		DestroyWindow(wnd);
		_editorWnd.store(nullptr, std::memory_order_release);
		_failed.store(true, std::memory_order_release);
		if (oleOk)
			OleUninitialize();
		return;
	}

	AppendWindowDebugEvent("UI thread OpenEditor succeeded");

	// Resize to the editor's preferred size now that the view is attached.
	auto sz = _plugin->GetEditorSize();
	if (sz.Width > 0 && sz.Height > 0)
	{
		RECT rect{ 0, 0,
			static_cast<LONG>(sz.Width),
			static_cast<LONG>(sz.Height) };
		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowPos(wnd, nullptr, 0, 0,
			rect.right - rect.left,
			rect.bottom - rect.top,
			SWP_NOMOVE | SWP_NOZORDER);
	}

	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);

	_ready.store(true, std::memory_order_release);
	AppendWindowDebugEvent("UI thread entering message loop");

	MSG msg{};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	AppendWindowDebugEvent("UI thread leaving message loop");

	if (_plugin)
		_plugin->CloseEditor();

	if (HWND existing = _editorWnd.exchange(nullptr, std::memory_order_acq_rel))
	{
		if (IsWindow(existing))
			DestroyWindow(existing);
	}
	_editorHostWnd = nullptr;

	if (oleOk)
		OleUninitialize();
}

void VstEditorWindow::Destroy()
{
	const DWORD tid = _uiThreadId.load(std::memory_order_acquire);
	HWND wnd = _editorWnd.load(std::memory_order_acquire);

	if (wnd)
	{
		AppendWindowDebugEvent("Destroy posting WM_CLOSE");
		PostMessageW(wnd, WM_CLOSE, 0, 0);
	}
	else if (tid != 0)
	{
		PostThreadMessageW(tid, WM_QUIT, 0, 0);
	}

	if (_uiThread.joinable())
		_uiThread.join();

	_plugin.reset();
	_editorHostWnd = nullptr;
}

void VstEditorWindow::ResizeEditorHostWindow() noexcept
{
	// Plug-in is parented directly to the editor frame; nothing to size here.
}

void VstEditorWindow::OnAction(const WindowAction& action)
{
	switch (action.WindowEventType)
	{
	case WindowAction::DESTROY:
		AppendWindowDebugEvent("WM_DESTROY handled");
		// Mark the window gone; the UI thread will exit its message loop and
		// perform CloseEditor / cleanup before returning.
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
	case WM_SIZE:
		self->ResizeEditorHostWindow();
		return 0;
	case WM_DESTROY:
	{
		WindowAction action;
		action.WindowEventType = WindowAction::DESTROY;
		self->OnAction(action);
		PostQuitMessage(0);
		return 0;
	}
	case WM_CLOSE:
	{
		AppendWindowDebugEvent("WM_CLOSE received");
		if (self->_plugin)
			self->_plugin->CloseEditor();
		DestroyWindow(hWnd);
		return 0;
	}
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
