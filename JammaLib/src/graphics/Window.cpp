///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Window.h"
#include "StringUtils.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace actions;
using namespace engine;
using namespace graphics;
using namespace utils;
using namespace resources;
using base::GuiElement;

Window::Window(Scene& scene,
	ResourceLib& resourceLib) :
	_scene(scene),
	_resourceLib(resourceLib),
	_style(WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN),
	_resizing(false),
	_trackingMouse(false),
	_buttonsDown(0),
	_lastHoverObjectId(0),
	_modifiers(actions::MODIFIER_NONE),
	_pickContext({ scene.Width(), scene.Height() }, base::DrawContext::ContextTarget::TEXTURE),
	_drawContext({ scene.Width(), scene.Height() }, base::DrawContext::ContextTarget::SCREEN)
{
	_config.Size = { scene.Width(), scene.Height() };
	_config.Position = { CW_USEDEFAULT, 0};
	_config.State = WINDOWED;
}

Window::~Window()
{
}

void Window::LoadResources()
{
	std::ifstream inputFile;
	inputFile.open("./resources/ResourceList.txt", std::ios::in);

	if (inputFile.good())
	{
		std::string line;
		int resourceType;
		std::string name;
		std::string arg;
		std::vector<std::string> args;

		// While we are able to get lines from the file
		while (std::getline(inputFile, line))
		{
			// Parse the line from the file and
			// set appropriate parameters
			std::stringstream ss(line);

			if (ss >> resourceType >> name)
			{
				args.clear();

				while (ss >> arg)
				{
					args.push_back(arg);
				}

				_resourceLib.LoadResource((Type)resourceType, name, args);
			}
		}
	}

	_resourceLib.LoadFonts();

	std::cout << "Loaded " << _resourceLib.NumResources() << " resources" << std::endl;
}

void Window::InitScene()
{
	_scene.InitResources(_resourceLib, true);
	_pickContext.Initialise();
	_drawContext.Initialise();
}

void Window::ShowMessage(LPCWSTR message)
{
	MessageBox(0, message, L"Window::create", MB_ICONERROR);
}

int Window::Create(HINSTANCE hInstance, int nCmdShow)
{
	_windowClass = MAKEINTATOM(Window::Register(hInstance));
	if (_windowClass == 0) {
		ShowMessage(L"registerClass() failed.");
		return 1;
	}

	// Temporary window creation to get supported formats
	HWND fakeWND = CreateWindowEx(
		0,
		_windowClass, L"Fake Window",
		_style,
		0, 0,						// Position x, y
		1, 1,						// width, height
		nullptr, nullptr,					// parent window, menu
		hInstance, nullptr);			// instance, param

	if (fakeWND == 0) {
		auto errCode = GetLastError();
		ShowMessage(L"CreateWindowEx() failed.");
		return 1;
	}

	HDC fakeDC = GetDC(fakeWND);	// Device Context

	PIXELFORMATDESCRIPTOR fakePFD;
	ZeroMemory(&fakePFD, sizeof(fakePFD));
	fakePFD.nSize = sizeof(fakePFD);
	fakePFD.nVersion = 1;
	fakePFD.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	fakePFD.iPixelType = PFD_TYPE_RGBA;
	fakePFD.cColorBits = 32;
	fakePFD.cAlphaBits = 8;
	fakePFD.cDepthBits = 24;

	const int fakePFDID = ChoosePixelFormat(fakeDC, &fakePFD);
	if (fakePFDID == 0) {
		ShowMessage(L"ChoosePixelFormat() failed.");
		return 1;
	}

	if (SetPixelFormat(fakeDC, fakePFDID, &fakePFD) == false) {
		auto errCode = GetLastError();
		ShowMessage(L"SetPixelFormat() failed.");
		return 1;
	}

	HGLRC fakeRC = wglCreateContext(fakeDC);	// Rendering Contex

	if (fakeRC == 0) {
		ShowMessage(L"wglCreateContext() failed.");
		return 1;
	}

	if (wglMakeCurrent(fakeDC, fakeRC) == false) {
		ShowMessage(L"wglMakeCurrent() failed.");
		return 1;
	}

	// Get pointers to functions (or init opengl loader here)
	PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB = nullptr;
	wglChoosePixelFormatARB = reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(wglGetProcAddress("wglChoosePixelFormatARB"));
	if (wglChoosePixelFormatARB == nullptr)
	{
		ShowMessage(L"wglGetProcAddress() failed.");
		return 1;
	}

	PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = nullptr;
	wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(wglGetProcAddress("wglCreateContextAttribsARB"));
	if (wglCreateContextAttribsARB == nullptr)
	{
		ShowMessage(L"wglGetProcAddress() failed.");
		return 1;
	}

	PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = nullptr;
	wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(wglGetProcAddress("wglSwapIntervalEXT"));
	if (wglSwapIntervalEXT == nullptr)
	{
		ShowMessage(L"wglGetProcAddress() failed.");
		return 1;
	}

	auto size = (WINDOWED == _config.State) ? AdjustSize(_config.Size, _style) : _config.Size;
	auto pos = (WINDOWED == _config.State) ? Center(size) : _config.Position;

	// Create a new window and context
	_wnd = CreateWindowEx(
		WS_EX_ACCEPTFILES,
		_windowClass, L"OpenGL Window",	// class name, window name
		_style,							// styles
		pos.X, pos.Y,		// posx, posy. If x is set to CW_USEDEFAULT y is ignored
		size.Width, size.Height,	// width, height
		nullptr, nullptr,						// parent window, menu
		hInstance, this);				// instance, param

	_dc = GetDC(_wnd);

	const int pixelAttribs[] = {
		WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
		WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
		WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
		WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
		WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
		WGL_COLOR_BITS_ARB, 32,
		WGL_ALPHA_BITS_ARB, 8,
		WGL_DEPTH_BITS_ARB, 24,
		WGL_STENCIL_BITS_ARB, 8,
		WGL_SAMPLE_BUFFERS_ARB, GL_TRUE,
		WGL_SAMPLES_ARB, 4,
		0
	};

	int pixelFormatID; UINT numFormats;
	const bool status = wglChoosePixelFormatARB(_dc, pixelAttribs, nullptr, 1, &pixelFormatID, &numFormats);

	if (status == false || numFormats == 0)
	{
		ShowMessage(L"wglChoosePixelFormatARB() failed.");
		return 1;
	}

	PIXELFORMATDESCRIPTOR PFD;
	DescribePixelFormat(_dc, pixelFormatID, sizeof(PFD), &PFD);
	SetPixelFormat(_dc, pixelFormatID, &PFD);

	const int major_min = 4, minor_min = 0;
	const int contextAttribs[] =
	{
		WGL_CONTEXT_MAJOR_VERSION_ARB, major_min,
		WGL_CONTEXT_MINOR_VERSION_ARB, minor_min,
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
//		WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
		0
	};

	_rc = wglCreateContextAttribsARB(_dc, 0, contextAttribs);
	if (_rc == nullptr) {
		ShowMessage(L"wglCreateContextAttribsARB() failed.");
		return 1;
	}

	// Delete temporary context and window
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(fakeRC);
	ReleaseDC(fakeWND, fakeDC);
	DestroyWindow(fakeWND);
	if (!wglMakeCurrent(_dc, _rc))
	{
		ShowMessage(L"wglMakeCurrent() failed.");
		return 1;
	}

	wglSwapIntervalEXT(1);

	// Init opengl loader here (extra safe version)
	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
		auto errStr = (const char*)glewGetErrorString(err);
		ShowMessage(CharsToUnicodeString(errStr));
		return 1;
	}

	// During init, enable debug output
	glEnable(GL_DEBUG_OUTPUT);
	glDebugMessageCallback(Window::MessageCallback, 0);
	
	// Need to re-enable this and disable depth rendering by fonts (semi transparent imgs)
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	LoadResources();
	InitScene();

	const char* glVersion = (const char *)glGetString(GL_VERSION);
	size_t versionStrSize = strlen(glVersion) + 1;
	wchar_t* glVersionStr = new wchar_t[versionStrSize];

	std::stringstream titleStringStream;
	titleStringStream << "Jamma (v" << LIB_VERSION << ")";
	auto titleStr = titleStringStream.str();
	wchar_t* titleChars = new wchar_t[titleStr.size() + 1];

	size_t outSize;
	mbstowcs_s(&outSize, titleChars, titleStr.size() + 1, titleStr.c_str(), titleStr.size());
	//mbstowcs_s(&outSize, glVersionStr, versionStrSize, glVersion, versionStrSize - 1);
	SetWindowText(_wnd, titleChars);
	ShowWindow(_wnd, nCmdShow);

	return 0;
}

void Window::SetWindowHandle(HWND wnd)
{
	_wnd = wnd;
}

Window::Config Window::GetConfig() const
{
	return _config;
}

Size2d Window::GetMinSize() const
{
	return { 200, 200 };
}

bool Window::IsFullscreen() const
{
	return FULLSCREEN == _config.State;
}

bool Window::IsResizing() const
{
	return _resizing;
}

void Window::SetResizing(bool resizing)
{
	_resizing = resizing;
}

bool Window::IsTrackingMouse() const
{
	return _trackingMouse;
}

void Window::SetTrackingMouse(bool tracking)
{
	_trackingMouse = tracking;
}

void Window::Resize(Size2d size)
{
	_config.Size = size;

	_pickContext.Initialise();
	_drawContext.Initialise();
}

void Window::SetWindowState(WindowState state)
{
	_config.State = state;
}

Size2d Window::GetSize()
{
	return _config.Size;
}

void Window::Render()
{
	_scene.CommitChanges();
	_scene.InitResources(_resourceLib, false);

	_pickContext.Bind();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	_scene.Draw3d(_pickContext, 1);

	// Save the picker render to bmp:
	// std::vector<unsigned char> data = _pickContext.GetTexture();
	// stbi_write_bmp("picker.bmp", _config.Size.Width, _config.Size.Height, 4, data.data());

	_drawContext.Bind();

	glClearColor(0.029f, 0.186f, 0.249f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	_scene.Draw3d(_drawContext, 1);
	_scene.Draw(_drawContext);
}

void Window::Swap()
{
	SwapBuffers(_dc);
}

void Window::Release()
{
	wglMakeCurrent(nullptr, nullptr);

	if (_rc)
		wglDeleteContext(_rc);

	if (_dc)
		ReleaseDC(_wnd, _dc);

	if (_wnd)
		DestroyWindow(_wnd);
}

ActionResult Window::OnAction(WindowAction winAction)
{
	auto isEaten = false;

	switch (winAction.WindowEventType)
	{
	case WindowAction::SIZE:
		_config.Size = winAction.Size;
		isEaten = true;
		//AdjustSize();
		break;
	case WindowAction::SIZE_MINIMISE:
		SetWindowState(Window::MINIMISED);
		isEaten = true;
		break;
	case WindowAction::SIZE_MAXIMISE:
		SetWindowState(Window::MAXIMISED);
		_config.Size = winAction.Size;
		isEaten = true;
		//AdjustSize();
	case WindowAction::DESTROY:
		break;
	}

	return { isEaten };
}

ActionResult Window::OnAction(TouchAction touchAction)
{
	switch (touchAction.Touch)
	{
	case TouchAction::TOUCH_MOUSE:
		if (0 == _buttonsDown)
			SetCapture(_wnd);

		switch (touchAction.State)
		{
		case TouchAction::TOUCH_DOWN:
			_buttonsDown |= (1 << touchAction.Index);
			break;
		case TouchAction::TOUCH_UP:
			_buttonsDown &= ~(1 << touchAction.Index);

			if (_buttonsDown == 0)
				ReleaseCapture();
			break;
		}
		break;
	}

	return _scene.OnAction(touchAction);
}

ActionResult Window::OnAction(TouchMoveAction touchAction)
{
	auto objectId = _pickContext.GetPixel({ touchAction.Position.X, touchAction.Position.Y });
	if (objectId != _lastHoverObjectId)
	{
		auto path = utils::IdToVec(objectId);
		_scene.SetHover3d(path);

		_lastHoverObjectId = objectId;
	}

	return _scene.OnAction(touchAction);
}

ActionResult Window::OnAction(KeyAction keyAction)
{
	// Handle modifiers
	int modifiers = _modifiers;

	switch (keyAction.KeyChar)
	{
	case 16:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)actions::MODIFIER_SHIFT);
		else
			modifiers &= ~((int)actions::MODIFIER_SHIFT);

		break;
	case 17:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)actions::MODIFIER_CTRL);
		else
			modifiers &= ~((int)actions::MODIFIER_CTRL);

		break;
	case 18:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)actions::MODIFIER_ALT);
		else
			modifiers &= ~((int)actions::MODIFIER_ALT);

		break;
	}

	_modifiers = (actions::Modifier)modifiers;
	keyAction.Modifiers = _modifiers;

	return _scene.OnAction(keyAction);
}

void APIENTRY Window::MessageCallback(GLenum source,
	GLenum type,
	GLuint id,
	GLenum severity,
	GLsizei length,
	const GLchar* message,
	const void* userParam)
{
	if ((131185 != id) || (33361 != type))
	{
		fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
			(type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
			type, severity, message);
	}
}

Size2d Window::AdjustSize(Size2d size, DWORD style)
{
	RECT rect = { 0, 0, (LONG)size.Width, (LONG)size.Height };
	AdjustWindowRect(&rect, style, false);

	long w = rect.right - rect.left;
	long h = rect.bottom - rect.top;

	return {
		w < 1 ? 1 : (unsigned int)w,
		h < 1 ? 1 : (unsigned int)h };
}

Position2d Window::Center(Size2d size)
{
	RECT primaryDisplaySize;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &primaryDisplaySize, 0);

	return {
		(primaryDisplaySize.right - (int)size.Width) / 2,
		(primaryDisplaySize.bottom - (int)size.Height) / 2 };
}

ATOM Window::Register(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;
	ZeroMemory(&wcex, sizeof(wcex));
	wcex.cbSize = sizeof(wcex);
	InitStyle(wcex);
	wcex.lpfnWndProc = WindowProcedure;
	wcex.hInstance = hInstance;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.lpszClassName = TEXT("JammaWindow");

	return RegisterClassEx(&wcex);
}

void Window::InitStyle(WNDCLASSEX& wcex) noexcept
{
	//wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
}

LRESULT CALLBACK Window::WindowProcedure(HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	Window* window = nullptr;

	if (message == WM_NCCREATE)
	{
		window = reinterpret_cast<Window*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
		SetWindowLongPtr(hWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
		
		if (window)
			window->SetWindowHandle(hWindow);
	}
	else
	{
		window = reinterpret_cast<Window*>(GetWindowLongPtr(hWindow, GWLP_USERDATA));
	}

	if (!window)
		return DefWindowProc(hWindow, message, wParam, lParam);

	switch (message)
	{
	case WM_COMMAND:
	{
		return 0;
	}
	case WM_SIZE:
	{
		if (!window->IsFullscreen())
		{
			WindowAction winAction;

			switch (wParam)
			{
			case SIZE_MINIMIZED:
				winAction.WindowEventType = WindowAction::SIZE_MINIMISE;
				break;
			case SIZE_MAXIMIZED:
				winAction.WindowEventType = WindowAction::SIZE_MAXIMISE;
				break;
			default:
				winAction.WindowEventType = WindowAction::SIZE;
				break;
			}
			winAction.Size = { LOWORD(lParam), HIWORD(lParam) };
			window->OnAction(winAction);
			return 0;
		}

		// If the device is not nullptr and the WM_SIZE message is not a
		// SIZE_MINIMIZED event, store the new dimensions so we can
		// reset the device once sizing has finished.
		//if (!window->IsFullscreen())
		//{
		//	if (/*(_device() != nullptr) &&*/ (wParam != SIZE_MINIMIZED))
		//	{
		//		WindowState windowState = WINDOWED;

		//		if (wParam == SIZE_MAXIMIZED)
		//			windowState = MAXIMISED;

		//		Size2d size = { LOWORD(lParam), HIWORD(lParam) };
		//		window->Resize(size, windowState);

		//		/*if (wParam == SIZE_MAXIMIZED)
		//		WinMan->Reset();
		//		else if ((wParam == SIZE_RESTORED) && (!_resizing))
		//		WinMan->Reset();*/
		//	}
		//}
	}
	break;
	case WM_SIZING:
		window->Render();
		window->Swap();
		return TRUE;
	case WM_GETMINMAXINFO:
	{
		// Window size/position is going to change
		// Intercept lParam pointer to MINMAXINFO structure and apply custom dimensions
		MINMAXINFO* MinMaxInfo = (MINMAXINFO*)lParam;

		auto min = window->GetMinSize();

		MinMaxInfo->ptMinTrackSize.x = (long)min.Width;
		MinMaxInfo->ptMinTrackSize.y = (long)min.Height;
		return 0;
	}
	case WM_ENTERSIZEMOVE:
	{
		if (window->IsResizing())
		{
			window->SetResizing(false);
			return 0;
		}
	}
	break;
	case WM_ERASEBKGND:
	{
		// Disable background painting during resize
		// (otherwise we get horrid white flickering).
		if (window->IsResizing())
		{
			return 0;
		}
	}
	break;
	case WM_CAPTURECHANGED:
	{
		if (window->IsResizing())
		{
			window->SetResizing(false);
			return 0;
		}
	}
	break;
	case WM_EXITSIZEMOVE:
	{
		if (window->IsResizing())
		{
			window->SetResizing(false);
			return 0;
		}
	}
	break;
	case WM_SETCURSOR:
	{
		/*MOUSEOVERSTATE mouseoverstate = MOUSEOVER_NORMAL;

		if (Component::MasterOverComponent() != nullptr)
		mouseoverstate = Component::MasterOverComponent()->MouseOverState();

		switch (mouseoverstate)
		{
		case MOUSEOVER_HAND:
		SetCursor(HandCursor);
		break;
		case MOUSEOVER_OPENHAND:
		SetCursor(OpenHandCursor);
		break;
		case MOUSEOVER_CLOSEDHAND:
		SetCursor(ClosedHandCursor);
		break;
		case MOUSEOVER_CARET:
		SetCursor(CaretCursor);
		break;
		default:
		SetCursor(ArrowCursor);
		break;
		}*/
	}
	break;
	case WM_LBUTTONDOWN:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_DOWN;
		touchAction.Index = 0;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_LBUTTONUP:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_UP;
		touchAction.Index = 0;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_RBUTTONDOWN:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_DOWN;
		touchAction.Index = 2;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_RBUTTONUP:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_UP;
		touchAction.Index = 2;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_MBUTTONDOWN:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_DOWN;
		touchAction.Index = 1;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_MBUTTONUP:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_UP;
		touchAction.Index = 1;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_MOUSELEAVE:
	{
		std::cout << "Left Client Area...\n";
		return 0;
	}
	case WM_MOUSEMOVE:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchMoveAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		/*MOUSEOVERSTATE mouseoverstate = MOUSEOVER_NORMAL;

		mouseoverstate = overComponent->MouseOverState();

		if (ButtonsDown > 0)
		{
		// Started mouse capture
		switch (mouseoverstate)
		{
		case MOUSEOVER_HAND:
		SetCursor(HandCursor);
		break;
		case MOUSEOVER_OPENHAND:
		SetCursor(OpenHandCursor);
		break;
		case MOUSEOVER_CLOSEDHAND:
		SetCursor(ClosedHandCursor);
		break;
		case MOUSEOVER_CARET:
		SetCursor(CaretCursor);
		break;
		default:
		SetCursor(ArrowCursor);
		break;
		}
		}*/

		if (!window->IsTrackingMouse())
		{
			TRACKMOUSEEVENT tme;
			tme.cbSize = sizeof(TRACKMOUSEEVENT);
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = hWindow;

			if (::_TrackMouseEvent(&tme))
			{
				window->SetTrackingMouse(true);
			}
		}

		return 0;
	}
	case WM_MOUSEWHEEL:
	{
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		int delta = GET_Y_LPARAM(wParam);

		delta /= WHEEL_DELTA;

		POINT pt = { x, y };

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_DOWN;
		touchAction.Index = 4;
		touchAction.Value = delta;
		touchAction.Position = { x, winHeight - y };
		window->OnAction(touchAction);

		return 0;
	}
	case WM_KEYDOWN:
	{
		bool repeatkey = false;

		if (VK_ESCAPE == wParam)
			PostMessage(hWindow, WM_CLOSE, 0, 0);

		if (lParam & (0x01 << 30))
			repeatkey = true;

		if (!repeatkey)
		{
			std::cout << "KeyDown " << wParam << "\n";

			KeyAction keyAction;
			keyAction.KeyChar = (unsigned int)wParam;
			keyAction.KeyActionType = KeyAction::KEY_DOWN;

			window->OnAction(keyAction);
		}

		return 0;
	}
	case WM_KEYUP:
	{
		std::cout << "KeyUp " << wParam << "\n";

		KeyAction keyAction;
		keyAction.KeyChar = (unsigned int)wParam;
		keyAction.KeyActionType = KeyAction::KEY_UP;

		window->OnAction(keyAction);

		return 0;
	}
	case WM_SYSKEYDOWN:
	{
		bool repeatkey = false;

		if (lParam & (0x01 << 30))
			repeatkey = true;

		if (!repeatkey)
		{
			std::cout << "SysKeyDown " << wParam << "\n";

			KeyAction keyAction;
			keyAction.KeyChar = (unsigned int)wParam;
			keyAction.IsSystem = true;
			keyAction.KeyActionType = KeyAction::KEY_DOWN;

			window->OnAction(keyAction);
		}
		return 0;
	}
	case WM_SYSKEYUP:
	{
		std::cout << "SysKeyUp " << wParam << "\n";

		bool repeatkey = false;

		KeyAction keyAction;
		keyAction.KeyChar = (unsigned int)wParam;
		keyAction.IsSystem = true;
		keyAction.KeyActionType = KeyAction::KEY_UP;

		window->OnAction(keyAction);
		return 0;
	}
	case WM_DESTROY:
	{
		WindowAction winAction;
		winAction.WindowEventType = WindowAction::DESTROY;

		window->OnAction(winAction);

		FreeConsole();
		PostQuitMessage(0);
		return 0;
	}
	}

	return DefWindowProc(hWindow, message, wParam, lParam);
}
