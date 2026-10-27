///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Window.h"
#include "StringUtils.h"
#include "GlDeleteQueue.h"
#include "../resources/ResourcePaths.h"
#include <thread>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

using namespace actions;
using namespace engine;
using namespace graphics;
using namespace utils;
using namespace resources;
using base::GuiElement;
using base::Action;
using base::DrawPass;

Window::Window(Scene& scene,
	ResourceLib& resourceLib) :
	_windowClass(nullptr),
	_rc(nullptr),
	_dc(nullptr),
	_wnd(nullptr),
	_scene(&scene),
	_resourceLib(resourceLib),
	_style(WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN),
	_resizing(false),
	_trackingMouse(false),
	_released(false),
	_buttonsDown(0),
	_lastHoverObjectId(0),
	_hover3dDirty(true),
	_forcePick(true),
	_cachedCursorPosition(std::nullopt),
	_cachedCursorModifiers(Action::MODIFIER_NONE),
	_pendingResize(std::nullopt),
	_restoreConfig(),
	_modifiers(Action::MODIFIER_NONE),
	_jamLoadRequested(false),
	_consoleToggleRequested(false),
	_highlightPass(ImageFullscreenParams(base::DrawableParams{""}, "blur"))
{
	_scene->InitGui();
	_InstallRelativePointerHost();

	_config.Size = { scene.GetSize().Width, scene.GetSize().Height };
	_config.Position = { scene.Position().X, scene.Position().Y};
	//_config.Position = { CW_USEDEFAULT, 0};
	_config.State = WINDOWED;
	_restoreConfig = _config;
}

Window::~Window()
{
	Release();
}

void Window::ReleaseGlResources()
{
	GlDeleteQueue::FlushPendingDeletes();

	_scene->ReleaseResources();
	_highlightPass.ReleaseResources();

	_drawContext.reset();
	_pickContext.reset();
	_textureContext.reset();
	_highlightBlurContext.reset();

	_resourceLib.ClearResources();

	GlDeleteQueue::FlushPendingDeletes();
}

void Window::LoadResources()
{
	std::ifstream inputFile;
	const auto resourceListPath = resources::ResolveResourceListPath();
	inputFile.open(resourceListPath, std::ios::in);

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
	else
	{
		std::cout << "[LOAD] failed to open resource list " << resourceListPath << std::endl;
	}

	_resourceLib.LoadFonts();

	std::cout << "[LOAD] Loaded " << _resourceLib.NumResources() << " resources" << std::endl;
}

void Window::InitScene()
{
	_scene->InitResources(_resourceLib, true);
	_highlightPass.InitResources(_resourceLib, true);
	_pickContext->Initialise();
	_textureContext->Initialise();
	_highlightBlurContext->Initialise();
	_drawContext->Initialise();
}

void Window::ShowMessage(LPCWSTR message)
{
	MessageBox(0, message, L"Window::create", MB_ICONERROR);
}

int Window::Create(HINSTANCE hInstance, int nCmdShow, unsigned int largeIconResourceId, unsigned int smallIconResourceId)
{
	_windowClass = MAKEINTATOM(Window::Register(hInstance, largeIconResourceId, smallIconResourceId));
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
	auto pos = _config.Position;
	if (WINDOWED == _config.State)
	{
		RECT desiredRect = {
			pos.X,
			pos.Y,
			pos.X + static_cast<LONG>(size.Width),
			pos.Y + static_cast<LONG>(size.Height)
		};

		RECT workArea{};
		if (GetMonitorWorkAreaForRect(desiredRect, workArea))
			ClampToWorkArea(pos, size, workArea);
	}

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
	GlDeleteQueue::SetRenderThread(std::this_thread::get_id());

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

	// Keep backing allocations valid even if creation reports an empty client.
	// Logical dimensions remain the actual client dimensions.
	const utils::Size2d backingSize{ (std::max)(1u, _config.Size.Width), (std::max)(1u, _config.Size.Height) };
	_pickContext.emplace(backingSize, base::DrawContext::ContextTarget::PICKING);
	_textureContext.emplace(backingSize, base::DrawContext::ContextTarget::TEXTURE);
	_highlightBlurContext.emplace(backingSize, base::DrawContext::ContextTarget::TEXTURE);
	_drawContext.emplace(backingSize, base::DrawContext::ContextTarget::SCREEN);

	LoadResources();
	InitScene();

	UpdateSessionTitle();
	ShowWindow(_wnd, nCmdShow);

	return 0;
}

void Window::SetWindowHandle(HWND wnd)
{
	_wnd = wnd;
	_titleSceneIdentity = 0u;
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

Action::Modifiers Window::Modifiers() const
{
	return _modifiers;
}

void Window::ClearModifiers()
{
	SceneDispatchScope dispatch(*this);
	// Dispatch synthetic key-up events for any modifier we still believe is
	// held. Routing them through the normal action path resets both the
	// modifier bitmask and any scene state keyed off those releases (e.g. the
	// Ctrl quantisation overlay), instead of leaving them stuck.
	for (const auto& mk : ModifierKeys)
	{
		if (_modifiers & mk.Flag)
		{
			KeyAction keyAction;
			keyAction.KeyChar = mk.KeyChar;
			keyAction.KeyActionType = KeyAction::KEY_UP;
			keyAction.IsSynthetic = true;
			OnAction(keyAction);
		}
	}

	_modifiers = Action::MODIFIER_NONE;
	_heldKeys.reset();
	_cachedCursorModifiers = _modifiers;
	_hover3dDirty = _forcePick = true;
}

bool Window::_IsNativeKeyDown(unsigned int key, bool queuedInput) const
{
	// Mouse gestures use their queued key state, not a later physical state.
	return ((queuedInput ? GetKeyState(static_cast<int>(key))
		: GetAsyncKeyState(static_cast<int>(key))) & 0x8000) != 0;
}

void Window::_ReconcileKeyboardState(bool queuedInput)
{
	if (_modifiers == Action::MODIFIER_NONE && _heldKeys.none()) return;
	SceneDispatchScope dispatch(*this);
	// System shortcuts can swallow releases without changing focus. Only modifiers
	// get synthetic releases; other tracked keys only lose their camera hold.
	for (const auto& modifier : ModifierKeys)
		if ((_modifiers & modifier.Flag) && !_IsNativeKeyDown(modifier.KeyChar, queuedInput))
		{
			KeyAction release;
			release.KeyChar = modifier.KeyChar;
			release.KeyActionType = KeyAction::KEY_UP;
			release.IsSynthetic = true;
			OnAction(release);
		}
	for (unsigned int key = 0u; key < _heldKeys.size(); ++key)
		if (_heldKeys.test(key) && !_IsNativeKeyDown(key, queuedInput)) _heldKeys.reset(key);
	_UpdateCameraEligibility();
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
	SceneDispatchScope dispatch(*this);
	if (HasRelativePointer() || size.Width == 0 || size.Height == 0) CancelMouseCapture();

	_config.Size = size;
	_UpdateCameraEligibility();
	if (_config.State == WINDOWED && size.Width > 0 && size.Height > 0)
		_restoreConfig.Size = size;
	_scene->SetSize(size);
	_lastHoverObjectId = 0;
	_hover3dDirty = true;
	_forcePick = true;
	_pendingResize = size;

	if (_scene->IsUiVerbose())
	{
		std::cout << "[WINDOW] resize size=" << size.Width << "x" << size.Height
			<< " state=" << _config.State << std::endl;
	}
}

void Window::ApplyPendingResize()
{
	if (!_pendingResize.has_value())
		return;
	// Retain the last valid backing contexts while the client is empty. The
	// next nonempty resize replaces this pending size before drawing resumes.
	if (_pendingResize->Width == 0 || _pendingResize->Height == 0)
		return;

	if (!GlDeleteQueue::IsRenderThread())
		return;

	if (!_pickContext.has_value() || !_textureContext.has_value() || !_highlightBlurContext.has_value() || !_drawContext.has_value())
		return;

	auto size = _pendingResize.value();
	_pickContext.emplace(size, base::DrawContext::ContextTarget::PICKING);
	_textureContext.emplace(size, base::DrawContext::ContextTarget::TEXTURE);
	_highlightBlurContext.emplace(size, base::DrawContext::ContextTarget::TEXTURE);
	_drawContext.emplace(size, base::DrawContext::ContextTarget::SCREEN);

	_pickContext->Initialise();
	_textureContext->Initialise();
	_highlightBlurContext->Initialise();
	_drawContext->Initialise();

	_forcePick = true;
	_pendingResize.reset();
}

void Window::SetWindowState(WindowState state)
{
	if (HasRelativePointer()) CancelMouseCapture();
	_config.State = state;
}

Window::Config Window::GetRestoreConfig() const
{
	return _restoreConfig;
}

Size2d Window::GetSize()
{
	return _config.Size;
}

void Window::_ReleaseOwnedMouseCapture()
{
	_releasingOwnedMouseCapture = true;
	ReleaseCapture();
	_releasingOwnedMouseCapture = false;
}

void Window::_UpdateCameraEligibility()
{
	if (!_scene) return;
	const bool visible = !_wnd || (IsWindowVisible(_wnd) && IsWindowEnabled(_wnd) && !IsIconic(_wnd));
	const bool validViewport = _config.Size.Width > 0u && _config.Size.Height > 0u;
	const bool held = _buttonsDown != 0u || _heldKeys.any() || _heldTouches.any()
		|| HasRelativePointer() || (_wnd && GetCapture() == _wnd) || _resizing || _windowSizeMove;
	// Keep camera inactivity timing active while Jamma is visible but unfocused
	// (for example while Game Bar owns focus).
	_scene->SetCameraInteractionEligibility(visible && validViewport, held);
}

void Window::Render()
{
	SceneDispatchScope dispatch(*this);
	UpdateSessionTitle();
	ProcessDeferredInputLoss();
	_ReconcileKeyboardState();
	GlDeleteQueue::FlushPendingDeletes();
	ApplyPendingResize();

	_UpdateCameraEligibility();
	_scene->CommitChanges();
	if (_nativeDestroyPending) return;
	if (_config.Size.Width == 0 || _config.Size.Height == 0) return;
	_scene->UpdateCamera();
	if (_scene->AdvanceUiAnimations()) _hover3dDirty = _forcePick = true;
	_scene->InitResources(_resourceLib, false);
	if (_nativeDestroyPending) return;
	// A plugin state call can synchronously send WM_KILLFOCUS while Scene's
	// non-audio lock is held. Drain only after that call and its lock unwind.
	ProcessDeferredInputLoss();

	const bool needsPick = (_hover3dDirty || _forcePick) && _cachedCursorPosition.has_value();
	if (needsPick)
	{
		_pickContext->Bind();

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		_scene->Draw3d(*_pickContext, 1, DrawPass::PASS_PICKER);

		// Forced reads must belong to the current position, never to the
		// previous asynchronous hover request.
		const auto pick = _forcePick
			? GlDrawContext::PixelReadback{ true, _pickContext->GetPixel(_cachedCursorPosition.value()) }
			: _pickContext->GetPixelAsync(_cachedCursorPosition.value());

		if (pick.HasValue)
		{
			if (_forcePick || pick.ObjectId != _lastHoverObjectId)
			{
				auto path = utils::IdToVec(pick.ObjectId);
				_scene->SetHover3d(path, _cachedCursorModifiers);
				_lastHoverObjectId = pick.ObjectId;
			}

			_hover3dDirty = false;
			_forcePick = false;
		}
	}

	_scene->ApplyDeferredHoverUpdates();

	// Save the picker render to bmp:
	// std::vector<unsigned char> data = _pickContext->GetTexture();
	// stbi_write_bmp("picker.bmp", _config.Size.Width, _config.Size.Height, 4, data.data());

	// An empty selection has no halo; avoid both full-screen blur passes.
	const bool hasSelection = _scene->HasSelection();
	if (hasSelection)
	{
		_textureContext->Bind();
		glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		_scene->Draw3d(*_textureContext, 1, DrawPass::PASS_HIGHLIGHT);

		// Separable X/Y blur gives a full 2D halo with two linear sampling passes.
		_highlightBlurContext->Bind();
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND); // Preserve RGBA without applying alpha a second time.
		_highlightPass.SetTexture(_textureContext->GetTexture());
		_highlightPass.SetBlurDirection(1.0f, 0.0f);
		_highlightPass.Draw3d(*_highlightBlurContext, 1, DrawPass::PASS_SCENE);
		glEnable(GL_BLEND);
	}

	_drawContext->Bind();

	glClearColor(0.029f, 0.186f, 0.249f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Composite the halo over the skybox, then let scene geometry cover its interior.
	_scene->DrawBackground(*_drawContext);
	if (hasSelection)
	{
		glDisable(GL_DEPTH_TEST);
		_highlightPass.SetTexture(_highlightBlurContext->GetTexture());
		_highlightPass.SetBlurDirection(0.0f, 1.0f);
		_highlightPass.Draw3d(*_drawContext, 1, DrawPass::PASS_SCENE);
	}

	_scene->Draw3d(*_drawContext, 1, DrawPass::PASS_SCENE);
	_scene->Draw(*_drawContext);
}

void Window::Swap()
{
	if (_config.Size.Width == 0 || _config.Size.Height == 0) return;
	SwapBuffers(_dc);
}

void Window::Release()
{
	if (_released)
		return;

	CancelMouseCapture();
	// Release is called while the borrowed Scene is still alive. The Window
	// destructor may run after that Scene has been destroyed.
	_scene->SetRelativePointerHost({}, {});
	_released = true;

	if (_dc && _rc)
	{
		wglMakeCurrent(_dc, _rc);
		ReleaseGlResources();
		wglMakeCurrent(nullptr, nullptr);
	}

	if (_rc)
	{
		wglDeleteContext(_rc);
		_rc = nullptr;
	}

	if (_dc && _wnd)
	{
		ReleaseDC(_wnd, _dc);
		_dc = nullptr;
	}

	if (_wnd)
	{
		DestroyWindow(_wnd);
		_wnd = nullptr;
	}
	_scene = nullptr;
}

ActionResult Window::OnAction(WindowAction winAction)
{
	SceneDispatchScope dispatch(*this);
	auto isEaten = false;

	switch (winAction.WindowEventType)
	{
	case WindowAction::SIZE:
		if (!IsFullscreen()) SetWindowState(Window::WINDOWED);
		Resize(winAction.Size);
		isEaten = true;
		break;
	case WindowAction::SIZE_MINIMISE:
		// Fullscreen must survive minimize/restore; empty client dimensions are
		// enough to suspend drawing without replacing its window mode.
		if (!IsFullscreen()) SetWindowState(Window::MINIMISED);
		Resize(winAction.Size);
		isEaten = true;
		break;
	case WindowAction::SIZE_MAXIMISE:
		if (!IsFullscreen()) SetWindowState(Window::MAXIMISED);
		Resize(winAction.Size);
		isEaten = true;
		break;
	case WindowAction::DESTROY:
		break;
	}

	return { isEaten };
}

std::optional<unsigned int> Window::_PickPointerPress(utils::Position2d position)
{
	if (!_rc || wglGetCurrentContext() != _rc || !GlDeleteQueue::IsRenderThread() || !_pickContext
		|| _pickContext->GetTexture() == 0u)
		return std::nullopt;
	ApplyPendingResize();
	// Input runs on the render owner. A fresh picker pass only is enough; a
	// full frame here would pump unrelated structural/plugin/UI work.
	GLint drawFramebuffer = 0, readFramebuffer = 0, packBuffer = 0;
	GLint program = 0, vertexArray = 0, activeTexture = 0;
	std::array<GLint, 3> textures{};
	std::array<GLfloat, 4> clearColor{};
	std::array<GLint, 4> viewport{}, scissorBox{};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
	glGetIntegerv(GL_VIEWPORT, viewport.data());
	glGetIntegerv(GL_SCISSOR_BOX, scissorBox.data());
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
	for (unsigned int unit = 0; unit < textures.size(); ++unit)
	{
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[unit]);
	}
	glActiveTexture(GL_TEXTURE0);
	const auto scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	const auto depthEnabled = glIsEnabled(GL_DEPTH_TEST);
	_pickContext->Bind();
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	_scene->Draw3d(*_pickContext, 1, DrawPass::PASS_PICKER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	const auto objectId = _pickContext->GetPixel(position);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
	glUseProgram(program);
	glBindVertexArray(vertexArray);
	for (unsigned int unit = 0; unit < textures.size(); ++unit)
	{
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, textures[unit]);
	}
	glActiveTexture(activeTexture);
	glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
	if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (depthEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	return objectId;
}

ActionResult Window::OnAction(TouchAction touchAction)
{
	SceneDispatchScope dispatch(*this);
	if (touchAction.Touch == TouchAction::TOUCH_FINGER && touchAction.Index >= 0 && touchAction.Index < 32)
		_heldTouches.set(static_cast<size_t>(touchAction.Index), touchAction.State == TouchAction::TOUCH_DOWN);
	_UpdateCameraEligibility();
	if (_config.Size.Width == 0 || _config.Size.Height == 0) return ActionResult::NoAction();
	if (HasRelativePointer() && touchAction.Touch == TouchAction::TOUCH_MOUSE
		&& touchAction.State == TouchAction::TOUCH_UP && touchAction.Index == _relativeButton)
	{
		if (!_RelativePointerValid())
			CancelMouseCapture();
	}
	_cachedCursorPosition = touchAction.Position;
	_cachedCursorModifiers = touchAction.Modifiers;
	bool pressPicked = false;
	if (touchAction.Touch == TouchAction::TOUCH_MOUSE && touchAction.State == TouchAction::TOUCH_DOWN
		&& touchAction.Index != 4 && _buttonsDown == 0u)
	{
		if (const auto objectId = _PickPointerPress(touchAction.Position))
		{
			_scene->SetHover3d(utils::IdToVec(*objectId), touchAction.Modifiers, touchAction.Position);
			_lastHoverObjectId = *objectId;
			_hover3dDirty = false;
			pressPicked = true;
		}
	}
	switch (touchAction.Touch)
	{
	case TouchAction::TOUCH_MOUSE:
		if (4 == touchAction.Index)
		{
			touchAction.MouseButtonsDown = _buttonsDown;
			return _scene->OnAction(touchAction);
		}

		if (0 == _buttonsDown && touchAction.State == TouchAction::TOUCH_DOWN && _wnd)
			SetCapture(_wnd);

		switch (touchAction.State)
		{
		case TouchAction::TOUCH_DOWN:
			// A delayed notification belongs to the preceding pointer gesture.
			_captureLossPending = false;
			_buttonsDown |= (1 << touchAction.Index);
			_forcePick = !pressPicked;
			break;
		case TouchAction::TOUCH_UP:
			_buttonsDown &= ~(1 << touchAction.Index);
			touchAction.Value = (1 << touchAction.Index);
			_forcePick = true;

			break;
		}
		break;
	}

	touchAction.MouseButtonsDown = _buttonsDown;

	const auto result = _scene->OnAction(touchAction);
	if (touchAction.Touch == TouchAction::TOUCH_MOUSE && touchAction.State == TouchAction::TOUCH_UP)
	{
		EndRelativePointer(touchAction.Index);
		if (_buttonsDown == 0u && _wnd && GetCapture() == _wnd) _ReleaseOwnedMouseCapture();
	}
	// Publish held state at input time: a press and release can both precede Render.
	_UpdateCameraEligibility();
	return result;
}

ActionResult Window::OnAction(TouchMoveAction touchAction)
{
	SceneDispatchScope dispatch(*this);
	if (_config.Size.Width == 0 || _config.Size.Height == 0) return ActionResult::NoAction();
	// Windows can redeliver WM_MOUSEMOVE at the same pixel without physical
	// pointer movement. Keep hover updates, but do not restart camera idle.
	if (!touchAction.IsRelative && touchAction.Origin == TouchMoveAction::Provenance::DeliveredPointer
		&& _cachedCursorPosition && touchAction.Position == *_cachedCursorPosition)
		touchAction.Origin = TouchMoveAction::Provenance::PointerRefresh;
	_cachedCursorPosition = touchAction.Position;
	_cachedCursorModifiers = touchAction.Modifiers;
	if (!touchAction.IsRelative) _hover3dDirty = true;

	return _scene->OnAction(touchAction);
}

bool Window::CancelMouseCapture()
{
	SceneDispatchScope dispatch(*this);
	if (_buttonsDown == 0u && !HasRelativePointer() && !_heldTouches.any()) return false;
	_heldTouches.reset();
	EndRelativePointer(_relativeButton);
	_buttonsDown = 0u;
	TouchMoveAction cancelled;
	cancelled.Touch = TouchAction::TOUCH_MOUSE;
	cancelled.Index = 0;
	cancelled.Position = _cachedCursorPosition.value_or(utils::Position2d{});
	cancelled.MouseButtonsDown = 0u;
	cancelled.Modifiers = _modifiers;
	cancelled.Origin = TouchMoveAction::Provenance::CaptureCancellation;
	OnAction(cancelled);
	if (_wnd && GetCapture() == _wnd)
		_ReleaseOwnedMouseCapture();
	return true;
}

bool Window::ProcessDeferredInputLoss()
{
	SceneDispatchScope dispatch(*this);
	const bool focusLost = _focusLossPending;
	const bool captureLost = _captureLossPending;
	_focusLossPending = _captureLossPending = false;
	if (!focusLost && !captureLost) return false;
	_windowSizeMove = false;
	SetResizing(false);
	CancelMouseCapture();
	_heldTouches.reset();
	ClearModifiers();
	if (_scene) _scene->OnInputFocusLost();
	_UpdateCameraEligibility();
	return true;
}

ActionResult Window::OnAction(KeyAction keyAction)
{
	SceneDispatchScope dispatch(*this);
	if (!keyAction.IsSynthetic)
	{
		const bool cameraTrigger = _scene->IsCameraTriggerKey(keyAction.KeyChar);
		const bool preservesIdle = _scene->PreservesCameraIdleForKey(keyAction.KeyChar);
		if (!preservesIdle)
		{
			if (!cameraTrigger) _scene->OnHumanActivity();
			else _scene->OnCameraTriggerActivity();
		}
		// Trigger holds are performance gestures, not manual camera ownership.
		if (keyAction.KeyChar < _heldKeys.size())
			_heldKeys.set(keyAction.KeyChar, !cameraTrigger && !preservesIdle && keyAction.KeyActionType == KeyAction::KEY_DOWN);
		_UpdateCameraEligibility();
	}
	// Handle modifiers
	const auto previousModifiers = _modifiers;
	int modifiers = _modifiers;

	switch (keyAction.KeyChar)
	{
	case 16:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)Action::MODIFIER_SHIFT);
		else
			modifiers &= ~((int)Action::MODIFIER_SHIFT);

		break;
	case 17:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)Action::MODIFIER_CTRL);
		else
			modifiers &= ~((int)Action::MODIFIER_CTRL);

		break;
	case 18:
		if (KeyAction::KEY_DOWN == keyAction.KeyActionType)
			modifiers |= ((int)Action::MODIFIER_ALT);
		else
			modifiers &= ~((int)Action::MODIFIER_ALT);

		break;
	}

	_modifiers = (Action::Modifiers)modifiers;
	keyAction.Modifiers = _modifiers;
	if (_modifiers != previousModifiers
		|| (VK_CONTROL == keyAction.KeyChar && KeyAction::KEY_UP == keyAction.KeyActionType))
	{
		// Modifier changes affect hover even without pointer motion. Ctrl release
		// also unfreezes Scene's hover when its picked object has not changed.
		_cachedCursorModifiers = _modifiers;
		_hover3dDirty = true;
		_forcePick = true;
	}
	// Contextual input owns shortcuts after physical modifiers have been reconciled.
	if (_scene->CapturesContextualKeyboard(keyAction.KeyChar)) return _scene->OnAction(keyAction);
	if ((76u == keyAction.KeyChar)
		&& (KeyAction::KEY_UP == keyAction.KeyActionType)
		&& (Action::MODIFIER_CTRL & keyAction.Modifiers)
		&& !(Action::MODIFIER_SHIFT & keyAction.Modifiers))
	{
		_jamLoadRequested = true;
		return { true, "window", "load-jam", ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if ((192u == keyAction.KeyChar)
		&& (Action::MODIFIER_CTRL & keyAction.Modifiers)
		&& !(Action::MODIFIER_SHIFT & keyAction.Modifiers))
	{
		if (KeyAction::KEY_UP == keyAction.KeyActionType)
			_consoleToggleRequested = true;
		return { true, "window", "toggle-console", ACTIONRESULT_DEFAULT, nullptr, {} };
	}

	return _scene->OnAction(keyAction);
}

void Window::ReplaceScene(Scene& scene)
{
	SceneDispatchScope dispatch(*this);
	ProcessDeferredInputLoss();
	CancelMouseCapture();
	_scene->SetRelativePointerHost({}, {});
	_scene->ReleaseResources();
	_scene = &scene;
	_InstallRelativePointerHost();
	_scene->InitGui();
	_scene->SetSize(_config.Size);
	_scene->InitResources(_resourceLib, true);
	_hover3dDirty = true;
	_forcePick = true;
	_cachedCursorPosition.reset();
	UpdateSessionTitle();
}

void Window::UpdateSessionTitle()
{
	if (_released || !_wnd || !_scene) return;
	const auto identity = _scene->Identity();
	const auto& path = _scene->Controls().CurrentJamPath();
	if (_titleSceneIdentity == identity && _titleJamPath == path) return;

	std::wstring title = L"Jamma";
	if (!path.empty())
	{
		const auto separator = path.find_last_of(L"/\\");
		const auto filename = separator == std::wstring::npos ? 0u : separator + 1u;
		if (filename < path.size())
		{
			title += L" - ";
			title.append(path, filename, std::wstring::npos);
		}
	}
	if (SetWindowTextW(_wnd, title.c_str()))
	{
		_titleJamPath = path;
		_titleSceneIdentity = identity;
	}
}

bool Window::ConsumeJamLoadRequest() noexcept
{
	const bool requested = _jamLoadRequested;
	_jamLoadRequested = false;
	return requested;
}

bool Window::ConsumeConsoleToggleRequest() noexcept
{
	const bool requested = _consoleToggleRequested;
	_consoleToggleRequested = false;
	return requested;
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
		std::cerr << (type == GL_DEBUG_TYPE_ERROR ? "[ERROR]" : "[WARNING]")
			<< "[OpenGL] callback type=" << type << " severity=" << severity
			<< " id=" << id << " message=" << (message ? message : "<none>") << std::endl;
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

bool Window::GetMonitorWorkArea(HMONITOR monitor, RECT& workArea) noexcept
{
	if (!monitor)
		return false;

	MONITORINFO monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfo(monitor, &monitorInfo))
		return false;

	workArea = monitorInfo.rcWork;
	return true;
}

bool Window::GetMonitorWorkAreaForRect(const RECT& rect, RECT& workArea) noexcept
{
	const auto monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	return GetMonitorWorkArea(monitor, workArea);
}

void Window::ClampToWorkArea(Position2d& position, Size2d& size, const RECT& workArea) noexcept
{
	const auto workWidth = static_cast<unsigned int>(std::max<LONG>(1, workArea.right - workArea.left));
	const auto workHeight = static_cast<unsigned int>(std::max<LONG>(1, workArea.bottom - workArea.top));

	if (size.Width > workWidth)
		size.Width = workWidth;
	if (size.Height > workHeight)
		size.Height = workHeight;

	const auto maxX = std::max<int>(workArea.left, workArea.right - static_cast<LONG>(size.Width));
	const auto maxY = std::max<int>(workArea.top, workArea.bottom - static_cast<LONG>(size.Height));

	position.X = std::clamp(position.X, static_cast<int>(workArea.left), maxX);
	position.Y = std::clamp(position.Y, static_cast<int>(workArea.top), maxY);
}

Position2d Window::Center(Size2d size)
{
	RECT primaryDisplaySize;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &primaryDisplaySize, 0);

	return {
		(primaryDisplaySize.right - (int)size.Width) / 2,
		(primaryDisplaySize.bottom - (int)size.Height) / 2 };
}

ATOM Window::Register(HINSTANCE hInstance, unsigned int largeIconResourceId, unsigned int smallIconResourceId)
{
	WNDCLASSEX wcex;
	ZeroMemory(&wcex, sizeof(wcex));
	wcex.cbSize = sizeof(wcex);
	InitStyle(wcex);
	wcex.lpfnWndProc = WindowProcedure;
	wcex.hInstance = hInstance;
	// LR_SHARED keeps resource handles owned by Windows for the class lifetime.
	// Do not DestroyIcon them during Window teardown or Scene replacement.
	// Distinct group IDs keep the LR_SHARED resource-name cache from returning
	// the large icon for the small-icon request regardless of requested size.
	if (largeIconResourceId)
		wcex.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(largeIconResourceId), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
	if (smallIconResourceId)
		wcex.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(smallIconResourceId), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
	if (!wcex.hIcon) wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	if (!wcex.hIconSm) wcex.hIconSm = wcex.hIcon;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.lpszClassName = TEXT("JammaWindow");

	return RegisterClassEx(&wcex);
}

void Window::InitStyle(WNDCLASSEX& wcex) noexcept
{
	//wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
}

Window::SceneDispatchScope::~SceneDispatchScope() noexcept
{
	if (--_window._sceneDispatchDepth != 0u) return;
	if (_window._nativeDestroyPending)
	{
		_window._nativeDestroyPending = false;
		_window._deferredNativeMessageCount = 0u;
		_window.CancelMouseCapture();
		WindowAction action;
		action.WindowEventType = WindowAction::DESTROY;
		_window.OnAction(action);
		PostQuitMessage(0);
		return;
	}
	// Plugins can pump the native queue while Scene owns a non-recursive lock.
	// Repost only after the outer call and all its local locks have unwound.
	const auto count = _window._deferredNativeMessageCount;
	_window._deferredNativeMessageCount = 0u;
	_window._deferredNativeOverflow = false;
	for (std::size_t index = 0u; index < count; ++index)
	{
		const auto& pending = _window._deferredNativeMessages[index];
		if (IsWindow(pending.WindowHandle))
			PostMessageW(pending.WindowHandle, pending.Message, pending.WParam, pending.LParam);
	}
}

bool Window::_DeferNativeSceneMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	if (_sceneDispatchDepth == 0u) return false;
	switch (message)
	{
	case WM_SIZING:
		// The normal UI frame will draw the resized client after this call returns.
		return true;
	case WM_INPUT:
		// HRAWINPUT belongs to this delivery and cannot be reposted safely.
		_captureLossPending = true;
		return true;
	case WM_DPICHANGED:
		// lParam points at a RECT whose lifetime ends with this native call.
		if (HasRelativePointer()) _captureLossPending = true;
		return true;
	case WM_DESTROY:
		_nativeDestroyPending = true;
		return true;
	case WM_SIZE: case WM_MOVE:
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
	case WM_MOUSEMOVE: case WM_MOUSEWHEEL:
	case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
		if (_deferredNativeOverflow) return true;
		if (message == WM_MOUSEMOVE && _deferredNativeMessageCount != 0u)
		{
			auto& previous = _deferredNativeMessages[_deferredNativeMessageCount - 1u];
			if (previous.Message == WM_MOUSEMOVE && previous.WindowHandle == hwnd)
			{
				previous = { hwnd, message, wParam, lParam };
				return true;
			}
		}
		if (_deferredNativeMessageCount < _deferredNativeMessages.size())
			_deferredNativeMessages[_deferredNativeMessageCount++] = { hwnd, message, wParam, lParam };
		else
		{
			// Discard the entire partial gesture; never replay an unmatched down.
			_deferredNativeMessageCount = 0u;
			_deferredNativeOverflow = true;
			_focusLossPending = true;
		}
		return true;
	default:
		return false;
	}
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
	if (window->_DeferNativeSceneMessage(hWindow, message, wParam, lParam))
		return message == WM_INPUT ? DefWindowProc(hWindow, message, wParam, lParam) : 0;
	// Finish the old gesture before accepting new input, including a focus-return
	// click before Render. Plugin-pumped messages must wait for Scene to unwind.
	const bool inputMessage = (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
		|| (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_INPUT;
	if (inputMessage && window->_sceneDispatchDepth == 0u
		&& (window->_focusLossPending || window->_captureLossPending))
		window->ProcessDeferredInputLoss();
	if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && window->_sceneDispatchDepth == 0u)
		window->_ReconcileKeyboardState(true);

	switch (message)
	{
	case WM_COMMAND:
	{
		return 0;
	}
	case WM_SIZE:
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
	case WM_MOVE:
	{
		if (window->HasRelativePointer()) window->CancelMouseCapture();
		RECT windowRect{};
		if (GetWindowRect(hWindow, &windowRect))
		{
			const utils::Position2d position{ windowRect.left, windowRect.top };
			window->_config.Position = position;
			if (window->_config.State == Window::WINDOWED)
				window->_restoreConfig.Position = position;

			if (window->_scene->IsUiVerbose())
			{
				std::cout << "[WINDOW] move pos=" << position.X << "," << position.Y
					<< " state=" << window->_config.State << std::endl;
			}
		}
		return 0;
	}
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

		RECT workArea{};
		const auto monitor = MonitorFromWindow(hWindow, MONITOR_DEFAULTTONEAREST);
		if (monitor && GetMonitorWorkArea(monitor, workArea))
		{
			MONITORINFO monitorInfo{};
			monitorInfo.cbSize = sizeof(monitorInfo);
			if (GetMonitorInfo(monitor, &monitorInfo))
			{
				const auto workWidth = workArea.right - workArea.left;
				const auto workHeight = workArea.bottom - workArea.top;
				MinMaxInfo->ptMaxPosition.x = workArea.left - monitorInfo.rcMonitor.left;
				MinMaxInfo->ptMaxPosition.y = workArea.top - monitorInfo.rcMonitor.top;
				MinMaxInfo->ptMaxSize.x = workWidth;
				MinMaxInfo->ptMaxSize.y = workHeight;
				MinMaxInfo->ptMaxTrackSize.x = workWidth;
				MinMaxInfo->ptMaxTrackSize.y = workHeight;
			}
		}
		return 0;
	}
	case WM_ENTERSIZEMOVE:
	{
		window->_windowSizeMove = true;
		window->_scene->OnHumanActivity();
		window->_UpdateCameraEligibility();
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
		// A completed own release is not external capture loss; held keys remain real.
		if (window->_releasingOwnedMouseCapture) return 0;
		window->_windowSizeMove = false;
		window->SetResizing(false);
		// ReleaseCapture after a normal mouse-up also sends this notification.
		// It must not cancel a fresh press received before the next UI frame.
		if (lParam != 0 || window->_buttonsDown != 0u || window->HasRelativePointer() || window->_heldTouches.any())
		{
			window->_captureLossPending = true;
			window->_modifiers = Action::MODIFIER_NONE;
			window->_heldKeys.reset();
		}
		return 0;
	}
	case WM_EXITSIZEMOVE:
	{
		window->_windowSizeMove = false;
		window->_scene->OnHumanActivity();
		window->_UpdateCameraEligibility();
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
		touchAction.Modifiers = window->Modifiers();

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
		touchAction.Modifiers = window->Modifiers();

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
		touchAction.Modifiers = window->Modifiers();

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
		touchAction.Modifiers = window->Modifiers();

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
		touchAction.Modifiers = window->Modifiers();

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
		touchAction.Modifiers = window->Modifiers();

		window->OnAction(touchAction);

		return 0;
	}
	case WM_MOUSELEAVE:
	{
		std::cout << "Left Client Area...\n";
		return 0;
	}
	case WM_INPUT:
	{
		if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
			window->_HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
		return DefWindowProc(hWindow, message, wParam, lParam);
	}
	case WM_DPICHANGED:
		if (window->HasRelativePointer()) window->CancelMouseCapture();
		break;
	case PointerRefreshMessage:
	case WM_MOUSEMOVE:
	{
		// Absolute messages include stale and synthetic recenter notifications.
		if (window->HasRelativePointer()) return 0;
		int winHeight = (int)window->GetSize().Height;
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		POINT pt = { x, y };

		TouchMoveAction touchAction;
		if (message == PointerRefreshMessage)
			touchAction.Origin = TouchMoveAction::Provenance::PointerRefresh;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		if (0u != (window->_buttonsDown & (1u << 2)))
			touchAction.Index = 2;
		else if (0u != (window->_buttonsDown & (1u << 0)))
			touchAction.Index = 0;
		else if (0u != (window->_buttonsDown & (1u << 1)))
			touchAction.Index = 1;
		else
			touchAction.Index = 0;
		touchAction.Position = { x, winHeight - y };
		touchAction.MouseButtonsDown = window->_buttonsDown;
		touchAction.Modifiers = window->Modifiers();

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
		int delta = GET_Y_LPARAM(wParam);

		delta /= WHEEL_DELTA;

		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hWindow, &pt);

		TouchAction touchAction;
		touchAction.Touch = TouchAction::TOUCH_MOUSE;
		touchAction.State = TouchAction::TOUCH_DOWN;
		touchAction.Index = 4;
		touchAction.Value = delta;
		touchAction.Position = { pt.x, winHeight - pt.y };
		touchAction.Modifiers = window->Modifiers();

		window->OnAction(touchAction);

		return 0;
	}
	case WM_KEYDOWN:
	{
		bool repeatkey = false;

		if (lParam & (0x01 << 30))
			repeatkey = true;

		if (repeatkey)
		{
			const auto scan = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
			const auto key = scan == 0x29u ? 192u : static_cast<unsigned int>(wParam);
			if (!window->_scene->PreservesCameraIdleForKey(key) && !window->_scene->IsCameraTriggerKey(key))
			{
				window->_scene->OnHumanActivity();
				if (key < window->_heldKeys.size()) window->_heldKeys.set(key);
			}
			else if (!window->_scene->PreservesCameraIdleForKey(key)) window->_scene->OnCameraTriggerActivity();
		}
		if (!repeatkey)
		{
			if (window->_scene->IsUiVerbose()) std::cout << "KeyDown " << wParam << "\n";
			unsigned int keyCode = static_cast<unsigned int>(wParam);
			const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
			// Normalise the physical key left of '1' (US backtick/tilde) across layouts.
			if (0x29u == scanCode)
				keyCode = 192u;

			KeyAction keyAction;
			keyAction.KeyChar = keyCode;
			keyAction.KeyActionType = KeyAction::KEY_DOWN;
			keyAction.Modifiers = window->Modifiers();

			window->OnAction(keyAction);
		}

		return 0;
	}
	case WM_KEYUP:
	{
		if (window->_scene->IsUiVerbose()) std::cout << "KeyUp " << wParam << "\n";
		unsigned int keyCode = static_cast<unsigned int>(wParam);
		const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
		if (0x29u == scanCode)
			keyCode = 192u;

		KeyAction keyAction;
		keyAction.KeyChar = keyCode;
		keyAction.KeyActionType = KeyAction::KEY_UP;
		keyAction.Modifiers = window->Modifiers();

		window->OnAction(keyAction);

		return 0;
	}
	case WM_SYSKEYDOWN:
	{
		bool repeatkey = false;

		if (lParam & (0x01 << 30))
			repeatkey = true;

		if (repeatkey)
		{
			const auto scan = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
			const auto key = scan == 0x29u ? 192u : static_cast<unsigned int>(wParam);
			if (!window->_scene->PreservesCameraIdleForKey(key) && !window->_scene->IsCameraTriggerKey(key))
			{
				window->_scene->OnHumanActivity();
				if (key < window->_heldKeys.size()) window->_heldKeys.set(key);
			}
			else if (!window->_scene->PreservesCameraIdleForKey(key)) window->_scene->OnCameraTriggerActivity();
		}
		if (!repeatkey)
		{
			if (window->_scene->IsUiVerbose()) std::cout << "SysKeyDown " << wParam << "\n";
			unsigned int keyCode = static_cast<unsigned int>(wParam);
			const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
			if (0x29u == scanCode)
				keyCode = 192u;

			KeyAction keyAction;
			keyAction.KeyChar = keyCode;
			keyAction.IsSystem = true;
			keyAction.KeyActionType = KeyAction::KEY_DOWN;
			keyAction.Modifiers = window->Modifiers();

			window->OnAction(keyAction);
		}
		return 0;
	}
	case WM_SYSKEYUP:
	{
		if (window->_scene->IsUiVerbose()) std::cout << "SysKeyUp " << wParam << "\n";
		unsigned int keyCode = static_cast<unsigned int>(wParam);
		const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16u) & 0xffu;
		if (0x29u == scanCode)
			keyCode = 192u;

		KeyAction keyAction;
		keyAction.KeyChar = keyCode;
		keyAction.IsSystem = true;
		keyAction.KeyActionType = KeyAction::KEY_UP;
		keyAction.Modifiers = window->Modifiers();

		window->OnAction(keyAction);
		return 0;
	}
	case WM_CANCELMODE:
		window->_captureLossPending = true;
		window->_modifiers = Action::MODIFIER_NONE;
		window->_heldKeys.reset();
		window->_windowSizeMove = false;
		window->SetResizing(false);
		break; // DefWindowProc also cancels native menu/scroll modes and capture.
	case WM_ACTIVATEAPP:
		if (!wParam)
		{
			window->_focusLossPending = true;
			window->_modifiers = Action::MODIFIER_NONE;
			window->_heldKeys.reset();
		}
		break;
	case WM_ACTIVATE:
	{
		window->_cameraWindowActive = LOWORD(wParam) != WA_INACTIVE;
		if (!window->_cameraWindowActive)
		{
			window->_focusLossPending = true;
			// WM_ACTIVATE can arrive without a useful modifier key-up (for
			// example when Game Bar takes focus). Neutralize input immediately so
			// a click before the next Render cannot be interpreted as Alt-click.
			window->_modifiers = Action::MODIFIER_NONE;
			window->_heldKeys.reset();
		}
		window->_UpdateCameraEligibility();
		break;
	}
	case WM_SHOWWINDOW:
	{
		if (!wParam)
		{
			window->_focusLossPending = true;
			window->_cameraWindowActive = false;
			window->_modifiers = Action::MODIFIER_NONE;
			window->_heldKeys.reset();
		}
		break;
	}
	case WM_SETFOCUS:
	{
		window->_cameraWindowActive = true;
		window->_UpdateCameraEligibility();
		break;
	}
	case WM_KILLFOCUS:
	{
		// Key-up events for held modifiers (e.g. Ctrl/Shift) are delivered to
		// whichever window has focus. When focus moves to a child or external
		// window (such as a VST editor) those key-ups never reach us, leaving
		// the modifier bitmask stuck. Clear it on focus loss so subsequent
		// clicks aren't treated as modified gestures.
		// Plugins may send this synchronously from GetState/SetState. Calling
		// Scene here would recursively acquire its non-recursive mutex, including
		// through capture cancellation or synthetic modifier-up dispatch.
		window->_focusLossPending = true;
		window->_windowSizeMove = false;
		window->_cameraWindowActive = false;
		// Keep native input state correct synchronously; Scene-owned focus
		// cleanup remains deferred to avoid re-entering its lock from plugins.
		window->_modifiers = Action::MODIFIER_NONE;
		window->_heldKeys.reset();
		return 0;
	}
	case WM_DESTROY:
	{
		window->CancelMouseCapture();
		WindowAction winAction;
		winAction.WindowEventType = WindowAction::DESTROY;

		window->OnAction(winAction);

		PostQuitMessage(0);
		return 0;
	}
	}

	return DefWindowProc(hWindow, message, wParam, lParam);
}

void Window::_InstallRelativePointerHost()
{
	_scene->SetRelativePointerHost(
		[this](int button, utils::Position2d anchor) { return BeginRelativePointer(button, anchor); },
		[this](int button) { EndRelativePointer(button); },
		[this]() {
			const auto dpi = _wnd ? GetDpiForWindow(_wnd) : 0u;
			return dpi > 0u ? static_cast<float>(dpi) / 96.0f : 1.0f;
		});
}

bool Window::_RelativePointerValid() const
{
	if (!_wnd || GetCapture() != _wnd || GetForegroundWindow() != _wnd) return false;
	RECT client{};
	POINT cursor{};
	if (!GetClientRect(_wnd, &client) || !GetCursorPos(&cursor) || !ScreenToClient(_wnd, &cursor)) return false;
	POINT screen = _relativeAnchorClient;
	return PtInRect(&client, _relativeAnchorClient) && PtInRect(&client, cursor)
		&& ClientToScreen(_wnd, &screen)
		&& screen.x == _relativeAnchorScreen.x && screen.y == _relativeAnchorScreen.y;
}

bool Window::BeginRelativePointer(int button, utils::Position2d anchor)
{
	if (HasRelativePointer() || (button != 0 && button != 2)
		|| (_buttonsDown & (1u << button)) == 0u) return false;
	_relativeAnchor = anchor;
	_relativeAnchorClient = { static_cast<LONG>(anchor.X), static_cast<LONG>(_config.Size.Height) - static_cast<LONG>(anchor.Y) };
	_relativeAnchorScreen = _relativeAnchorClient;
	if (!_wnd || !ClientToScreen(_wnd, &_relativeAnchorScreen) || !_RelativePointerValid()) return false;
	// Registration is process-wide: restore the previous mouse owner on teardown.
	UINT count = 0u;
	if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
	std::vector<RAWINPUTDEVICE> registered(count);
	if (count && GetRegisteredRawInputDevices(registered.data(), &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
	_previousRawMouse.reset();
	for (const auto& device : registered)
		if (device.usUsagePage == 1u && device.usUsage == 2u) _previousRawMouse = device;
	RAWINPUTDEVICE mouse{ 1u, 2u, 0u, _wnd };
	if (!RegisterRawInputDevices(&mouse, 1u, sizeof(mouse))) return false;
	_relativeButton = button;
	// Keep the visible cursor free while raw input supplies gesture deltas.
	// SetCursorPos(_relativeAnchorScreen.x, _relativeAnchorScreen.y);
	return true;
}

void Window::_RestoreRawInput()
{
	// A plugin may have registered a new owner meanwhile; do not remove it.
	UINT count = 0u;
	if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != UINT(-1))
	{
		std::vector<RAWINPUTDEVICE> registered(count);
		if (!count || GetRegisteredRawInputDevices(registered.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1))
			for (const auto& device : registered)
				if (device.usUsagePage == 1u && device.usUsage == 2u && device.hwndTarget == _wnd)
				{
					const RAWINPUTDEVICE restore = _previousRawMouse.value_or(RAWINPUTDEVICE{ 1u, 2u, RIDEV_REMOVE, nullptr });
					RegisterRawInputDevices(&restore, 1u, sizeof(restore));
					break;
				}
	}
	_previousRawMouse.reset();
}

void Window::_ResamplePointer()
{
	POINT cursor{};
	if (!_wnd || !GetCursorPos(&cursor) || !ScreenToClient(_wnd, &cursor)) return;
	_cachedCursorPosition = utils::Position2d{ static_cast<int>(cursor.x), static_cast<int>(_config.Size.Height) - static_cast<int>(cursor.y) };
	_hover3dDirty = true;
	_forcePick = true;
	// Resume hover at the current cursor position after editor teardown.
	MSG pending{};
	while (PeekMessage(&pending, _wnd, PointerRefreshMessage, PointerRefreshMessage, PM_REMOVE))
	{
		// WM_QUIT bypasses PeekMessage's filters. Preserve shutdown rather than
		// consuming it while discarding synthetic pointer movements.
		if (pending.message == WM_QUIT)
		{
			PostQuitMessage(static_cast<int>(pending.wParam));
			break;
		}
	}
	PostMessage(_wnd, PointerRefreshMessage, 0, MAKELPARAM(cursor.x, cursor.y));
}

void Window::EndRelativePointer(int button)
{
	if (!HasRelativePointer() || button != _relativeButton) return;
	_relativeButton = -1;
	_RestoreRawInput();
	_ResamplePointer();
}

void Window::_HandleRawInput(HRAWINPUT input)
{
	if (!HasRelativePointer()) return;
	if (!_RelativePointerValid() || (_buttonsDown & (1u << _relativeButton)) == 0u)
	{
		CancelMouseCapture();
		return;
	}
	RAWINPUT raw{};
	UINT size = sizeof(raw);
	const auto read = GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
	if (read == UINT(-1) || read < sizeof(RAWINPUTHEADER))
	{
		CancelMouseCapture();
		return;
	}
	if (raw.header.dwType != RIM_TYPEMOUSE) return;
	if (read < sizeof(RAWINPUT) || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
	{
		CancelMouseCapture();
		return;
	}
	// SetCursorPos(_relativeAnchorScreen.x, _relativeAnchorScreen.y);
	if (raw.data.mouse.lLastX == 0 && raw.data.mouse.lLastY == 0) return;
	TouchMoveAction movement;
	movement.Index = _relativeButton;
	movement.Position = _relativeAnchor;
	movement.MouseButtonsDown = _buttonsDown;
	movement.Modifiers = _modifiers;
	movement.IsRelative = true;
	const auto upward = -static_cast<std::int64_t>(raw.data.mouse.lLastY);
	movement.RelativeDelta = { static_cast<int>(raw.data.mouse.lLastX),
		static_cast<int>(std::clamp<std::int64_t>(upward, INT_MIN, INT_MAX)) };
	OnAction(movement);
}
