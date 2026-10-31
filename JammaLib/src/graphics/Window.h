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
#include <array>
#include <bitset>
#include <memory>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>
#include "glew/glew.h"
#include "gl/gl.h"
#include "gl/glext.h"
#include "gl/wglext.h"
#include "Action.h"
#include "ActionReceiver.h"
#include "CommonTypes.h"
#include "../../include/Constants.h"
#include "../engine/Scene.h"
#include "../base/DrawContext.h"
#include "../utils/VecUtils.h"
#include "../resources/Resource.h"
#include "../resources/ResourceLib.h"
#include "../actions/KeyAction.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../actions/CursorAction.h"
#include "../actions/WindowAction.h"
#include "ImageFullscreen.h"

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
		HWND GetWindowHandle() const noexcept { return _wnd; }
		void ShowMessage(LPCWSTR message);
		// Optional large/small icon resources belong to the application instance; Window owns presentation.
		int Create(HINSTANCE hInstance, int nCmdShow, unsigned int largeIconResourceId = 0u, unsigned int smallIconResourceId = 0u);
		Config GetConfig() const;
		utils::Size2d GetMinSize() const;
		bool IsFullscreen() const;
		bool IsResizing() const;
		void SetResizing(bool resizing);
		base::Action::Modifiers Modifiers() const;
		void ClearModifiers();
		// Propagate OS capture/focus loss so editor pointer transactions cancel.
		bool CancelMouseCapture();
		// UI-frame owner only, after external plugin calls have returned. Native
		// focus/capture notifications only latch requests and never enter Scene.
		bool ProcessDeferredInputLoss();
		bool BeginRelativePointer(int button, utils::Position2d anchor);
		void EndRelativePointer(int button);
		bool HasRelativePointer() const noexcept { return _relativeButton >= 0; }
		bool IsTrackingMouse() const;
		void SetTrackingMouse(bool resizing);
		void Resize(utils::Size2d size);
		void SetWindowState(WindowState state);
		Config GetRestoreConfig() const;
		utils::Size2d GetSize();
		void Render();
		void Swap();
		void Release();
		void ReplaceScene(engine::Scene& scene);
		// UI owner only: mirror the current Scene committed destination, never a pending save.
		void UpdateSessionTitle();
		bool ConsumeJamLoadRequest() noexcept;
		bool ConsumeConsoleToggleRequest() noexcept;

		virtual actions::ActionResult OnAction(actions::WindowAction winAction) override;
		virtual actions::ActionResult OnAction(actions::TouchAction touchAction) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction touchAction) override;
		virtual actions::ActionResult OnAction(actions::KeyAction touchAction) override;

		static utils::Size2d AdjustSize(utils::Size2d size, DWORD style);
		static utils::Position2d Center(utils::Size2d size);
		static bool GetMonitorWorkArea(HMONITOR monitor, RECT& workArea) noexcept;
		static bool GetMonitorWorkAreaForRect(const RECT& rect, RECT& workArea) noexcept;
		static void ClampToWorkArea(utils::Position2d& position, utils::Size2d& size, const RECT& workArea) noexcept;
		static ATOM Register(HINSTANCE hInstance, unsigned int largeIconResourceId = 0u, unsigned int smallIconResourceId = 0u);
		static LRESULT CALLBACK WindowProcedure(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	protected:
		virtual bool _IsNativeKeyDown(unsigned int key, bool queuedInput) const;
		void _ReconcileKeyboardState(bool queuedInput = false);
		// Resolve the current press, not a delayed asynchronous hover readback.
		virtual std::optional<unsigned int> _PickPointerPress(utils::Position2d position);

	private:
		struct ModifierKey { base::Action::Modifiers Flag; unsigned int KeyChar; };
		static constexpr std::array<ModifierKey, 3u> ModifierKeys{{
			{ base::Action::MODIFIER_SHIFT, VK_SHIFT },
			{ base::Action::MODIFIER_CTRL, VK_CONTROL },
			{ base::Action::MODIFIER_ALT, VK_MENU }
		}};
		class SceneDispatchScope
		{
		public:
			explicit SceneDispatchScope(Window& window) noexcept : _window(window) { ++_window._sceneDispatchDepth; }
			~SceneDispatchScope() noexcept;
		private:
			Window& _window;
		};
		struct DeferredNativeMessage { HWND WindowHandle; UINT Message; WPARAM WParam; LPARAM LParam; };
		bool _DeferNativeSceneMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
		unsigned int _sceneDispatchDepth = 0u;
		std::array<DeferredNativeMessage, 256u> _deferredNativeMessages{};
		std::size_t _deferredNativeMessageCount = 0u;
		bool _deferredNativeOverflow = false;
		bool _nativeDestroyPending = false;
		void _InstallRelativePointerHost();
		void _UpdateCameraEligibility();
		void _UpdatePointerCursor();
		void _ReleaseOwnedMouseCapture();
		static constexpr UINT PointerRefreshMessage = WM_APP + 0x341u;
		bool _RelativePointerValid() const;
		void _RestoreRawInput();
		void _ResamplePointer();
		void _HandleRawInput(HRAWINPUT input);
		void LoadResources();
		void InitScene();
		void ApplyPendingResize();
		void ReleaseGlResources();

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
		bool _released;
		unsigned int _buttonsDown;
		// Window owner only; cleared by existing focus/capture-loss messages.
		std::bitset<256u> _heldKeys;
		std::bitset<32u> _heldTouches;
		bool _cameraWindowActive = true;
		bool _releasingOwnedMouseCapture = false;
		bool _windowSizeMove = false;
		int _relativeButton = -1;
		POINT _relativeAnchorClient{};
		POINT _relativeAnchorScreen{};
		utils::Position2d _relativeAnchor{};
		std::optional<RAWINPUTDEVICE> _previousRawMouse;
		unsigned int _lastHoverObjectId;
		bool _hover3dDirty;
		bool _forcePick;
		std::optional<utils::Position2d> _cachedCursorPosition;
		base::Action::Modifiers _cachedCursorModifiers;
		std::optional<utils::Size2d> _pendingResize;
		Config _restoreConfig;

		std::optional<GlDrawContext> _drawContext;
		std::optional<GlDrawContext> _pickContext;
		std::optional<GlDrawContext> _textureContext;
		std::optional<GlDrawContext> _highlightBlurContext;
		engine::Scene* _scene;
		// UI-owned cache; Scene identity prevents reuse across session replacement.
		std::uint64_t _titleSceneIdentity = 0u;
		std::wstring _titleJamPath;
		resources::ResourceLib& _resourceLib;
		base::Action::Modifiers _modifiers;
		bool _jamLoadRequested;
		bool _consoleToggleRequested;
		// WindowProcedure and Render share the UI thread. No audio reader; these
		// coalesced requests die with the Window and drain before Scene replacement.
		bool _focusLossPending = false;
		bool _captureLossPending = false;

		ImageFullscreen _highlightPass;
	};
}
