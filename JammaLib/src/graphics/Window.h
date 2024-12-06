///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include "glew/glew.h"
#include "gl/gl.h"
#include "gl/glext.h"
#include "gl/wglext.h"
#include "../engine/Scene.h"
#include "Action.h"
#include "ActionReceiver.h"
#include "CommonTypes.h"
#include "../base/DrawContext.h"
#include "../utils/VecUtils.h"
#include "../include/Constants.h"
#include "../resources/Resource.h"
#include "../resources/ResourceLib.h"
#include "../actions/KeyAction.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../actions/CursorAction.h"
#include "../actions/WindowAction.h"

namespace graphics
{
	class Window : public base::ActionReceiver
	{
	public:
		enum WindowState
		{
			WINDOWED,
			MINIMISED,
			MAXIMISED,
			FULLSCREEN
		};

		struct Config
		{
			utils::Size2d Size;
			utils::Position2d Position;
			WindowState State;
		};

	public:
		Window(engine::Scene& scene,
			resources::ResourceLib& resourceLib);
		~Window();

	public:
		void SetWindowHandle(HWND wnd);
		void ShowMessage(LPCWSTR message);
		int Create(HINSTANCE hInstance, int nCmdShow);
		Config GetConfig() const;
		utils::Size2d GetMinSize() const;
		bool IsFullscreen() const;
		bool IsResizing() const;
		void SetResizing(bool resizing);
		bool IsTrackingMouse() const;
		void SetTrackingMouse(bool resizing);
		void Resize(utils::Size2d size);
		void SetWindowState(WindowState state);
		utils::Size2d GetSize();
		void Render();
		void Swap();
		void Release();

		virtual actions::ActionResult OnAction(actions::WindowAction winAction) override;
		virtual actions::ActionResult OnAction(actions::TouchAction touchAction) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction touchAction) override;
		virtual actions::ActionResult OnAction(actions::KeyAction touchAction) override;

		static utils::Size2d AdjustSize(utils::Size2d size, DWORD style);
		static utils::Position2d Center(utils::Size2d size);
		static ATOM Register(HINSTANCE hInstance);
		static LRESULT CALLBACK WindowProcedure(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	private:
		void LoadResources();
		void InitScene();

		static void InitStyle(WNDCLASSEX& wcex) noexcept;
		static void APIENTRY MessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);

	private:
		LPTSTR _windowClass;	// Window Class
		HGLRC _rc;			// Rendering Context
		HDC	_dc;				// Device Context
		HWND _wnd;			// Window
		DWORD _style;
		Config _config;
		bool _resizing;
		bool _trackingMouse;
		unsigned int _buttonsDown;
		unsigned int _lastHoverObjectId;

		GlDrawContext _drawContext;
		GlDrawContext _pickContext;
		engine::Scene& _scene;
		resources::ResourceLib& _resourceLib;
		actions::Modifier _modifiers;
	};
}
