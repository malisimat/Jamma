///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstEditorWindow.h"
#include "Window.h"
#include "../vst/VstPlugin.h"
#include <filesystem>
#include <fstream>
#include <mutex>

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
	_plugin(nullptr)
{
}

VstEditorWindow::~VstEditorWindow()
{
	Destroy();
}

bool VstEditorWindow::Create(HINSTANCE hInstance,
	std::shared_ptr<vst::VstPlugin> plugin,
	HWND parentHwnd)
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
		title.resize(converted - 1);  // strip null terminator added by mbstowcs_s

	// Use the shared helper to register our class and create the window.
	// The window is initially small; it will be resized after the plugin view
	// is attached and reports its preferred size via GetEditorSize().
	_editorWnd = Window::CreateSimpleWindow(
		hInstance,
		_ClassName,
		title.c_str(),
		WS_OVERLAPPEDWINDOW,
		100, 100, 600, 400,
		parentHwnd,
		VstEditorWindow::WindowProcedure,
		this);

	if (!_editorWnd)
	{
		AppendWindowDebugEvent("CreateWindow failed");
		return false;
	}

	AppendWindowDebugEvent("Host window created");

	// Attach the plugin's editor view to our HWND
	if (!_plugin->OpenEditor(_editorWnd))
	{
		AppendWindowDebugEvent("OpenEditor failed");
		DestroyWindow(_editorWnd);
		_editorWnd = nullptr;
		_plugin.reset();
		return false;
	}

	AppendWindowDebugEvent("OpenEditor succeeded");

	// Resize to the editor's preferred size
	auto sz = _plugin->GetEditorSize();
	if (sz.Width > 0 && sz.Height > 0)
	{
		RECT rect{ 0, 0,
			static_cast<LONG>(sz.Width),
			static_cast<LONG>(sz.Height) };

		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowPos(_editorWnd, nullptr, 0, 0,
			rect.right - rect.left,
			rect.bottom - rect.top,
			SWP_NOMOVE | SWP_NOZORDER);
	}

	ShowWindow(_editorWnd, SW_SHOW);
	AppendWindowDebugEvent("Create complete");
	return true;
}

void VstEditorWindow::Destroy()
{
	if (_plugin)
	{
		AppendWindowDebugEvent("Destroy closing plugin editor");
		_plugin->CloseEditor();
		_plugin.reset();
	}

	if (_editorWnd)
	{
		DestroyWindow(_editorWnd);
		_editorWnd = nullptr;
	}
}

void VstEditorWindow::OnAction(const WindowAction& action)
{
	switch (action.WindowEventType)
	{
	case WindowAction::DESTROY:
		// Plugin CloseEditor is called before DestroyWindow, so just clear the handle.
		if (_plugin)
			_plugin->CloseEditor();

		AppendWindowDebugEvent("WM_DESTROY handled");

		_editorWnd = nullptr;
		_plugin.reset();
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
	case WM_DESTROY:
	{
		WindowAction action;
		action.WindowEventType = WindowAction::DESTROY;
		self->OnAction(action);
		return 0;
	}
	case WM_CLOSE:
	{
		// Detach the plugin view before destroying the window so the plugin
		// has a chance to clean up before the HWND becomes invalid.
		AppendWindowDebugEvent("WM_CLOSE received");
		if (self->_plugin)
			self->_plugin->CloseEditor();

		DestroyWindow(hWnd);
		return 0;
	}
	default:
		// All other messages (keyboard, mouse, paint) are forwarded to the
		// default handler so the plugin's embedded child window can process
		// them natively.
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
