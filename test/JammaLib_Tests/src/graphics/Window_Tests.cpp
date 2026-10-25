#define GLEW_STATIC
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "graphics/Window.h"
#include "engine/Scene.h"
#include "engine/Station.h"
#include "gui/GuiScrollPanel.h"

using resources::ResourceLib;
using graphics::Window;
using engine::Scene;
using engine::SceneParams;

namespace graphics
{
	class CameraPresentationTestAccess
	{
	public:
		static void ArmIdle(Camera& camera, Time start, const engine::CameraRecordingObservation& observation, bool automatic)
		{
			camera._unattendedCamera.HumanActivity(start);
			camera._unattendedCamera.Update(start, true, false, observation, {});
			if (automatic) camera._unattendedCamera.Update(start + std::chrono::seconds(8), true, false, observation, {});
		}
		static bool AutomaticAt(Camera& camera, Time now, const engine::CameraRecordingObservation& observation)
		{
			return camera._unattendedCamera.Update(now, camera._windowEligible && !camera._heldInteraction,
				camera._windowEligible && camera._heldInteraction, observation, {}).IsAutomatic();
		}
		static bool Automatic(const Camera& camera) { return camera._unattendedCamera.IsAutomatic(); }
		static std::uint64_t ActivityRevision(const Camera& camera) { return camera._humanActivityRevision; }
	};
}

class CameraActivityScene final : public Scene
{
public:
	CameraActivityScene() : Scene(SceneParams(base::DrawableParams(), base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u })), {}) {}
	void ArmIdle(bool automatic)
	{
		graphics::Camera::Pose pose;
		pose.Eye = { 0.0f, 0.0f, 420.0f };
		_workspace->Camera().SetViewTarget(graphics::Camera::View::Front, pose);
		for (unsigned int tick = 0u; tick < 8u; ++tick) _workspace->Camera().TickBackgroundDrag(0.05f);
		_idleStart = utils::Timer::GetTime() - std::chrono::milliseconds(7900);
		const auto observation = _cameraRecordings->Snapshot();
		graphics::CameraPresentationTestAccess::ArmIdle(_workspace->Camera(), _idleStart, *observation, automatic);
	}
	bool AutomaticAtOriginalDeadline()
	{
		return graphics::CameraPresentationTestAccess::AutomaticAt(_workspace->Camera(),
			_idleStart + std::chrono::milliseconds(8100), *_cameraRecordings->Snapshot());
	}
	bool Automatic() const { return graphics::CameraPresentationTestAccess::Automatic(_workspace->Camera()); }
	std::uint64_t ActivityRevision() const { return graphics::CameraPresentationTestAccess::ActivityRevision(_workspace->Camera()); }
	void AddStation(const std::shared_ptr<engine::Station>& station) { _AddStation(station); }
	std::shared_ptr<gui::GuiPerformanceGroupRow> GroupRow(std::size_t row)
	{
		std::vector<std::shared_ptr<gui::GuiPerformanceGroupRow>> rows;
		CollectRows(_workspace->SelectionPanel(), rows);
		return row < rows.size() ? rows[row] : nullptr;
	}
private:
	static void CollectRows(const std::shared_ptr<base::GuiElement>& element,
		std::vector<std::shared_ptr<gui::GuiPerformanceGroupRow>>& rows)
	{
		if (auto row = std::dynamic_pointer_cast<gui::GuiPerformanceGroupRow>(element)) rows.push_back(row);
		if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(element))
			if (scroll->Content()) CollectRows(scroll->Content(), rows);
		for (unsigned int index = 0u; index < 256u; ++index)
		{
			auto child = element->TryGetChild(static_cast<unsigned char>(index));
			if (!child) break;
			CollectRows(child, rows);
		}
	}
	Time _idleStart;
};

class CameraActivityWindow final : public Window
{
public:
	CameraActivityWindow(Scene& scene, ResourceLib& resources) : Window(scene, resources) {}
	std::vector<unsigned char> PressPath;
	std::bitset<256u> NativeKeys = ~std::bitset<256u>{};
	using Window::_ReconcileKeyboardState;
protected:
	bool _IsNativeKeyDown(unsigned int key, bool) const override { return NativeKeys.test(key); }
	std::optional<unsigned int> _PickPointerPress(utils::Position2d) override
	{
		if (PressPath.empty()) return 0u;
		unsigned int objectId = 0u;
		for (unsigned int segment = 0u; segment < 3u; ++segment)
			objectId |= (segment < PressPath.size() ? PressPath[segment] : 0xffu) << (16u - 8u * segment);
		return objectId;
	}
};

TEST(CameraIdle, PerformanceKeysAndNativeRepeatsPreserveDeadlineAndActiveWander)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	for (const bool automatic : { false, true })
		for (const bool system : { false, true })
			for (const auto key : { VK_F1, VK_F2, VK_F9, VK_BACK, VK_SPACE, VK_OEM_3, VK_SHIFT, VK_CONTROL, VK_MENU })
			{
				SCOPED_TRACE(key);
				scene.ArmIdle(automatic);
				const auto revision = scene.ActivityRevision();
				for (unsigned int press = 0u; press < 3u; ++press)
				{
					Window::WindowProcedure(host, system ? WM_SYSKEYDOWN : WM_KEYDOWN, key, 0);
					Window::WindowProcedure(host, system ? WM_SYSKEYDOWN : WM_KEYDOWN, key, static_cast<LPARAM>(1u << 30u));
					EXPECT_EQ(automatic, scene.Automatic());
					Window::WindowProcedure(host, system ? WM_SYSKEYUP : WM_KEYUP, key, 0);
				}
				EXPECT_EQ(revision, scene.ActivityRevision());
				EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
			}
	DestroyWindow(host);
}

TEST(CameraIdle, FocusLossClearsShortcutHoldWithoutResettingVisibleCameraDeadline)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	for (const bool automatic : { false, true })
	{
		Window::WindowProcedure(host, WM_SETFOCUS, 0, 0);
		Window::WindowProcedure(host, WM_KEYDOWN, VK_LWIN, 0);
		Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 0);
		scene.ArmIdle(automatic);
		Window::WindowProcedure(host, WM_ACTIVATE, WA_INACTIVE, 0);
		EXPECT_TRUE(window.ProcessDeferredInputLoss());
		// Shortcut releases are lost to Game Bar; Jamma remains unfocused.
		EXPECT_EQ(base::Action::MODIFIER_NONE, window.Modifiers());
		EXPECT_EQ(automatic, scene.Automatic());
		EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
	}
	DestroyWindow(host);
}

TEST(CameraIdle, LostWinAndModifierReleasesRecoverWithoutAnyFocusNotification)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	for (const auto winKey : { VK_LWIN, VK_RWIN })
	{
		actions::KeyAction key;
		key.KeyActionType = actions::KeyAction::KEY_DOWN;
		for (const auto code : { winKey, VK_MENU, VK_CONTROL, VK_SHIFT })
		{
			window.NativeKeys.set(code);
			key.KeyChar = code;
			window.OnAction(key);
		}
		scene.ArmIdle(false);
		const auto revision = scene.ActivityRevision();
		window._ReconcileKeyboardState();
		EXPECT_FALSE(scene.AutomaticAtOriginalDeadline()); // Win is genuinely held.
		window.NativeKeys.reset(VK_MENU);
		window._ReconcileKeyboardState();
		EXPECT_EQ(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT, window.Modifiers());
		for (const auto code : { winKey, VK_CONTROL, VK_SHIFT }) window.NativeKeys.reset(code);
		window._ReconcileKeyboardState();
		EXPECT_EQ(base::Action::MODIFIER_NONE, window.Modifiers());
		EXPECT_EQ(revision, scene.ActivityRevision());
		EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
	}
}

TEST(CameraIdle, PointerSelectionDeselectMuteAndBackgroundClicksPreserveDeadline)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	engine::StationParams params;
	params.Name = "camera-selection";
	params.Size = { 100u, 100u };
	audio::MergeMixBehaviourParams merge;
	auto station = std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
	scene.AddStation(station);
	for (const bool automatic : { false, true })
		for (const int button : { 0, 1 })
		{
			scene.ArmIdle(automatic);
			const auto revision = scene.ActivityRevision();
			for (unsigned int press = 0u; press < 3u; ++press)
			{
				window.PressPath.clear();
				for (const auto index : station->GlobalId()) window.PressPath.push_back(static_cast<unsigned char>(index + 1u));
				actions::TouchAction action;
				action.Touch = actions::TouchAction::TOUCH_MOUSE;
				action.Index = button;
				action.Position = { 800, 500 };
				action.State = actions::TouchAction::TOUCH_DOWN;
				window.OnAction(action);
				// Render-side policy may run while a selection/mute press is held.
				scene.AutomaticAtOriginalDeadline();
				action.State = actions::TouchAction::TOUCH_UP;
				window.OnAction(action);
				if (button == 0) EXPECT_TRUE(station->IsSelected());
				window.PressPath.clear();
				action.Index = 0;
				action.State = actions::TouchAction::TOUCH_DOWN;
				window.OnAction(action);
				action.State = actions::TouchAction::TOUCH_UP;
				window.OnAction(action);
				EXPECT_FALSE(station->IsSelected());
			}
			EXPECT_EQ(revision, scene.ActivityRevision());
			EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
		}
}

TEST(CameraIdle, GroupButtonsPreserveDeadlineFromPressThroughRelease)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	for (const bool automatic : { false, true })
		for (std::size_t rowIndex = 0u; rowIndex < 2u; ++rowIndex)
		{
			auto row = scene.GroupRow(rowIndex);
			ASSERT_TRUE(row);
			scene.ArmIdle(automatic);
			const auto revision = scene.ActivityRevision();
			for (const auto slot : { 0u, 1u, 1u })
			{
				row->AdvanceAnimation(1.0f);
				auto button = row->Button(slot);
				ASSERT_TRUE(button->IsVisible());
				actions::TouchAction action;
				action.Touch = actions::TouchAction::TOUCH_MOUSE;
				action.Index = 0;
				action.Position = button->GlobalPosition() + utils::Position2d{ 4, 4 };
				action.State = actions::TouchAction::TOUCH_DOWN;
				EXPECT_TRUE(window.OnAction(action).IsEaten);
				scene.AutomaticAtOriginalDeadline();
				action.State = actions::TouchAction::TOUCH_UP;
				EXPECT_TRUE(window.OnAction(action).IsEaten);
			}
			EXPECT_EQ(revision, scene.ActivityRevision());
			EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
		}
}

TEST(CameraIdle, CameraDragWheelAndViewKeysRestartIdle)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	for (const bool automatic : { false, true })
	{
		scene.ArmIdle(automatic);
		actions::TouchAction action;
		action.Touch = actions::TouchAction::TOUCH_MOUSE;
		action.Index = 0;
		action.Position = { 800, 500 };
		action.State = actions::TouchAction::TOUCH_DOWN;
		window.OnAction(action);
		actions::TouchMoveAction move;
		move.Touch = actions::TouchAction::TOUCH_MOUSE;
		move.MouseButtonsDown = 1u;
		move.Position = { 840, 520 };
		window.OnAction(move);
		action.State = actions::TouchAction::TOUCH_UP;
		window.OnAction(action);
		EXPECT_FALSE(scene.AutomaticAtOriginalDeadline());

		scene.ArmIdle(automatic);
		action.Index = 4;
		action.State = actions::TouchAction::TOUCH_DOWN;
		action.Value = -1;
		window.OnAction(action);
		EXPECT_FALSE(scene.AutomaticAtOriginalDeadline());

		scene.ArmIdle(automatic);
		actions::KeyAction key;
		key.KeyChar = VK_TAB;
		key.KeyActionType = actions::KeyAction::KEY_UP;
		window.OnAction(key);
		EXPECT_FALSE(scene.AutomaticAtOriginalDeadline());
	}
}

TEST(CameraIdle, RepeatedMouseMoveAtSamePixelPreservesDeadline)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	actions::TouchMoveAction move;
	move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.Position = { 800, 500 };
	window.OnAction(move);
	for (const bool automatic : { false, true })
	{
		scene.ArmIdle(automatic);
		const auto revision = scene.ActivityRevision();
		for (unsigned int repeat = 0u; repeat < 3u; ++repeat) window.OnAction(move);
		EXPECT_EQ(revision, scene.ActivityRevision());
		EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
	}
	move.Position = { 801, 500 };
	window.OnAction(move);
	EXPECT_FALSE(scene.AutomaticAtOriginalDeadline());
}

TEST(CameraFrontZoom, FitsLargeOneSidedStationRowsAtWideAndNarrowAspectRatios)
{
	for (const auto viewport : { utils::Size2d{ 1600u, 900u }, utils::Size2d{ 450u, 1000u } })
	{
		graphics::Camera camera(graphics::CameraParams(base::MoveableParams(), 0u));
		graphics::CameraBounds bounds;
		// Thirty stations, including fitted take/core/waveform extents rather
		// than just their origins. A one-sided row also exercises panned framing.
		bounds.Include(utils::Position3d{ -300.0f, -235.0f, -300.0f });
		bounds.Include(utils::Position3d{ 29.0f * 600.0f + 300.0f, 235.0f, 300.0f });
		camera.SetFrontVisibleBounds(bounds);
		const utils::Position2d centre{ static_cast<int>(viewport.Width / 2u), static_cast<int>(viewport.Height / 2u) };
		const auto settle = [&camera]() { for (unsigned int tick = 0u; tick < 8u; ++tick) camera.TickBackgroundDrag(0.05f); };
		for (unsigned int notch = 0u; notch < 40u; ++notch)
		{
			camera.HandleWheel(-1, centre, viewport.Width, viewport.Height, {});
			settle();
		}
		const auto distantPose = camera.CurrentPose();
		EXPECT_GT(distantPose.Eye.Z, 1350.0f);
		const auto matrix = camera.Projection(static_cast<float>(viewport.Width) / viewport.Height, {}) * camera.ViewMatrix();
		for (const auto x : { bounds.Min.X, bounds.Max.X })
			for (const auto y : { bounds.Min.Y, bounds.Max.Y })
				for (const auto z : { bounds.Min.Z, bounds.Max.Z })
				{
					const auto clip = matrix * glm::vec4(x, y, z, 1.0f);
					ASSERT_GT(clip.w, 0.0f);
					const auto ndc = glm::vec3(clip) / clip.w;
					EXPECT_LT(std::abs(ndc.x), 0.90f);
					EXPECT_LT(std::abs(ndc.y), 0.90f);
					EXPECT_GT(ndc.z, -1.0f);
					EXPECT_LT(ndc.z, 1.0f); // Far-plane coverage shared by render/picking passes.
				}

		// Off-centre cursor zoom must retain the same world-plane hit even when
		// far-plane homogeneous W is smaller than the old rejection threshold.
		const utils::Position2d cursor{ static_cast<int>(viewport.Width * 3u / 4u), static_cast<int>(viewport.Height / 3u) };
		const auto focus = camera.FocusPointAtCursor(cursor, viewport.Width, viewport.Height, {});
		EXPECT_GT(std::abs(focus.X), 1000.0f);
		camera.HandleWheel(1, cursor, viewport.Width, viewport.Height, {});
		for (unsigned int tick = 0u; tick < 4u; ++tick)
		{
			camera.TickBackgroundDrag(0.05f);
			const auto clip = camera.Projection(static_cast<float>(viewport.Width) / viewport.Height, {})
				* camera.ViewMatrix() * glm::vec4(focus.X, focus.Y, focus.Z, 1.0f);
			EXPECT_NEAR(cursor.X, (clip.x / clip.w + 1.0f) * viewport.Width * 0.5f, 1.0f);
			EXPECT_NEAR(cursor.Y, (clip.y / clip.w + 1.0f) * viewport.Height * 0.5f, 1.0f);
		}
		const auto manualPose = camera.CurrentPose();
		for (unsigned int view = 0u; view < 3u; ++view)
		{
			camera.CycleView({}, {}, {}, {}, {}, false);
			settle();
		}
		EXPECT_EQ(graphics::Camera::View::Front, camera.CurrentView());
		EXPECT_FLOAT_EQ(manualPose.Eye.Z, camera.CurrentPose().Eye.Z);
		EXPECT_FLOAT_EQ(manualPose.Eye.X, camera.CurrentPose().Eye.X);
		for (unsigned int notch = 0u; notch < 40u; ++notch)
		{
			camera.HandleWheel(1, centre, viewport.Width, viewport.Height, {});
			settle();
		}
		EXPECT_FLOAT_EQ(80.0f, camera.CurrentPose().Eye.Z);
	}
}

TEST(Window, ReleasedWindowMayOutliveScene)
{
	ResourceLib resources;
	auto scene = std::make_unique<CameraActivityScene>();
	auto window = std::make_unique<Window>(*scene, resources);
	window->Release();
	scene.reset();
	// Match application shutdown: release native resources, destroy the Scene,
	// then destroy the stack-owned Window without consulting its former Scene.
	window->Release();
	window.reset();
}

TEST(Window, IsInitiallyWindowed) {
	auto sceneParams = SceneParams(base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams());
	io::UserConfig userConfig = {};
	auto scene = Scene(sceneParams, userConfig);

	ResourceLib resourceLib;
	auto win = Window(scene, resourceLib);

	ASSERT_EQ(Window::WindowState::WINDOWED, win.GetConfig().State);
}

class CaptureLossScene final : public Scene
{
public:
	CaptureLossScene() : Scene(SceneParams(base::DrawableParams(),
		base::MoveableParams(), base::SizeableParams({ 640u, 480u })), {}) {}

	actions::ActionResult OnAction(actions::TouchAction action) override
	{
		LastTouch = action;
		return actions::ActionResult::NoAction();
	}
	actions::ActionResult OnAction(actions::TouchMoveAction action) override
	{
		LastMove = action;
		return actions::ActionResult::NoAction();
	}

	std::optional<actions::TouchMoveAction> LastMove;
	std::optional<actions::TouchAction> LastTouch;
};

class FunctionKeyScene final : public Scene
{
public:
    FunctionKeyScene() : Scene(SceneParams(base::DrawableParams(),
        base::MoveableParams(), base::SizeableParams({ 640u, 480u })), {}) {}
    actions::ActionResult OnAction(actions::KeyAction action) override
    {
        Keys.push_back(action);
        return actions::ActionResult::NoAction();
    }
    std::vector<actions::KeyAction> Keys;
};

TEST(Window, FunctionVirtualKeysAndSystemTransitionsReachSceneUnchanged)
{
    FunctionKeyScene scene;
    ResourceLib resourceLib;
    Window window(scene, resourceLib);
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    for (unsigned int key = VK_F1; key <= VK_F9; ++key)
        for (const bool system : { false, true })
        {
            scene.Keys.clear();
            Window::WindowProcedure(host, system ? WM_SYSKEYDOWN : WM_KEYDOWN, key, 0);
            Window::WindowProcedure(host, system ? WM_SYSKEYDOWN : WM_KEYDOWN, key,
                static_cast<LPARAM>(1u << 30u));
            Window::WindowProcedure(host, system ? WM_SYSKEYUP : WM_KEYUP, key, 0);
            ASSERT_EQ(scene.Keys.size(), 2u);
            EXPECT_EQ(scene.Keys[0].KeyChar, key);
            EXPECT_EQ(scene.Keys[1].KeyChar, key);
            EXPECT_EQ(scene.Keys[0].KeyActionType, actions::KeyAction::KEY_DOWN);
            EXPECT_EQ(scene.Keys[1].KeyActionType, actions::KeyAction::KEY_UP);
            EXPECT_EQ(scene.Keys[0].IsSystem, system);
            EXPECT_EQ(scene.Keys[1].IsSystem, system);
        }
    DestroyWindow(host);
}

TEST(Window, LostCaptureSendsZeroButtonMoveExactlyOnce)
{
	CaptureLossScene scene;
	ResourceLib resourceLib;
	Window window(scene, resourceLib);
	actions::TouchAction down;
	down.Touch = actions::TouchAction::TOUCH_MOUSE;
	down.State = actions::TouchAction::TOUCH_DOWN;
	down.Index = 0;
	down.Position = { 320, 240 };
	window.OnAction(down);
	EXPECT_TRUE(window.CancelMouseCapture());
	ASSERT_TRUE(scene.LastMove);
	EXPECT_EQ(actions::TouchAction::TOUCH_MOUSE, scene.LastMove->Touch);
	EXPECT_EQ(0u, scene.LastMove->MouseButtonsDown);
	EXPECT_EQ(320, scene.LastMove->Position.X);
	EXPECT_FALSE(window.CancelMouseCapture());
}

TEST(Window, EmptyClientKeepsActualLayoutAndRejectsPointerInput)
{
	CaptureLossScene scene; ResourceLib resources; Window window(scene, resources);
	for (const auto empty : { utils::Size2d{ 0, 0 }, utils::Size2d{ 0, 480 }, utils::Size2d{ 640, 0 } }) {
		window.Resize({ 640, 480 });
		actions::TouchAction down; down.Touch = actions::TouchAction::TOUCH_MOUSE;
		down.State = actions::TouchAction::TOUCH_DOWN; down.Index = 0; down.Position = { 320, 240 };
		window.OnAction(down); scene.LastMove.reset();
		window.Resize(empty);
		EXPECT_EQ(empty.Width, window.GetSize().Width); EXPECT_EQ(empty.Height, window.GetSize().Height);
		EXPECT_EQ(empty.Width, scene.GetSize().Width); EXPECT_EQ(empty.Height, scene.GetSize().Height);
		EXPECT_EQ(640u, window.GetRestoreConfig().Size.Width); EXPECT_EQ(480u, window.GetRestoreConfig().Size.Height);
		ASSERT_TRUE(scene.LastMove); EXPECT_EQ(0u, scene.LastMove->MouseButtonsDown);
		EXPECT_FALSE(window.CancelMouseCapture());
		scene.LastTouch.reset(); scene.LastMove.reset();
		EXPECT_FALSE(window.OnAction(down).IsEaten);
		actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE;
		move.Position = down.Position; EXPECT_FALSE(window.OnAction(move).IsEaten);
		EXPECT_FALSE(scene.LastTouch); EXPECT_FALSE(scene.LastMove);
	}
	window.Resize({ 640, 480 });
	actions::TouchAction down; down.State = actions::TouchAction::TOUCH_DOWN;
	window.OnAction(down); EXPECT_TRUE(scene.LastTouch);
	window.CancelMouseCapture();
}

TEST(Window, MinimizeAndRestorePreserveFullscreenModeAndWindowedRestoreSize)
{
	CaptureLossScene scene; ResourceLib resources; Window window(scene, resources);
	for (const auto mode : { Window::WINDOWED, Window::FULLSCREEN }) {
		window.SetWindowState(Window::WINDOWED); window.Resize({ 640, 480 });
		window.SetWindowState(mode);
		actions::WindowAction size; size.WindowEventType = actions::WindowAction::SIZE_MINIMISE; size.Size = { 0, 0 };
		EXPECT_TRUE(window.OnAction(size).IsEaten);
		EXPECT_EQ(mode == Window::FULLSCREEN ? Window::FULLSCREEN : Window::MINIMISED, window.GetConfig().State);
		EXPECT_EQ(0u, window.GetSize().Width); EXPECT_EQ(0u, scene.GetSize().Height);
		size.WindowEventType = actions::WindowAction::SIZE; size.Size = { 640, 480 };
		EXPECT_TRUE(window.OnAction(size).IsEaten); EXPECT_EQ(mode, window.GetConfig().State);
		EXPECT_EQ(640u, window.GetRestoreConfig().Size.Width); EXPECT_EQ(480u, window.GetRestoreConfig().Size.Height);
	}
}

TEST(Window, RelativePointerRequiresNativeForegroundCapture)
{
	CaptureLossScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	EXPECT_FALSE(window.BeginRelativePointer(2, { 320, 240 }));
	actions::TouchAction down;
	down.Index = 2;
	down.State = actions::TouchAction::TOUCH_DOWN;
	window.OnAction(down);
	EXPECT_FALSE(window.BeginRelativePointer(2, { 320, 240 }));
	EXPECT_FALSE(window.HasRelativePointer());
	window.EndRelativePointer(0);
	EXPECT_TRUE(window.CancelMouseCapture());
	EXPECT_FALSE(window.CancelMouseCapture());
}

TEST(Window, ButtonReleaseDispatchPreservesOtherButtons)
{
	CaptureLossScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	actions::TouchAction button;
	button.State = actions::TouchAction::TOUCH_DOWN;
	button.Index = 0;
	window.OnAction(button);
	button.Index = 2;
	window.OnAction(button);
	button.State = actions::TouchAction::TOUCH_UP;
	window.OnAction(button);
	ASSERT_TRUE(scene.LastTouch);
	EXPECT_EQ(1u, scene.LastTouch->MouseButtonsDown);
	EXPECT_EQ(2, scene.LastTouch->Index);
	EXPECT_FALSE(scene.LastMove);
	EXPECT_TRUE(window.CancelMouseCapture());
}

TEST(Window, ConsoleToggleShortcutIsConsumedOnce) {
	auto sceneParams = SceneParams(base::DrawableParams(),
		base::MoveableParams(), base::SizeableParams());
	io::UserConfig userConfig = {};
	Scene scene(sceneParams, userConfig);
	ResourceLib resourceLib;
	Window window(scene, resourceLib);
	actions::KeyAction key;
	key.KeyActionType = actions::KeyAction::KEY_DOWN;
	key.KeyChar = 17;
	window.OnAction(key);
	key.KeyChar = 192;
	window.OnAction(key);
	EXPECT_FALSE(window.ConsumeConsoleToggleRequest());
	key.KeyActionType = actions::KeyAction::KEY_UP;
	window.OnAction(key);
	EXPECT_TRUE(window.ConsumeConsoleToggleRequest());
	EXPECT_FALSE(window.ConsumeConsoleToggleRequest());
}

TEST(Window, DeferredCaptureLossDoesNotCancelANewerMousePress)
{
    CaptureLossScene scene;
    ResourceLib resources;
    Window window(scene, resources);
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    actions::TouchAction touch;
    touch.Touch = actions::TouchAction::TOUCH_MOUSE;
    touch.Index = 0;
    touch.State = actions::TouchAction::TOUCH_DOWN;
    window.OnAction(touch);
    touch.State = actions::TouchAction::TOUCH_UP;
    window.OnAction(touch);
    // Normal ReleaseCapture notifies after the physical release is complete.
    Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, 0);
    EXPECT_FALSE(window.ProcessDeferredInputLoss());
    touch.State = actions::TouchAction::TOUCH_DOWN;
    window.OnAction(touch);
    Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, 0);
    window.OnAction(touch); // A new gesture supersedes that capture-loss request.
    EXPECT_FALSE(window.ProcessDeferredInputLoss());
    EXPECT_TRUE(window.CancelMouseCapture());
    DestroyWindow(host);
}

TEST(Window, FocusLossClearsModifiersAndFinishesOldGestureBeforeReturningClick)
{
	CaptureLossScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	for (const auto loss : { WM_KILLFOCUS, WM_ACTIVATE, WM_SHOWWINDOW, WM_ACTIVATEAPP, WM_CANCELMODE })
		for (const auto modifier : { VK_SHIFT, VK_CONTROL, VK_MENU })
		{
			SCOPED_TRACE(loss);
			SCOPED_TRACE(modifier);
			Window::WindowProcedure(host, WM_KEYDOWN, modifier, 0);
			Window::WindowProcedure(host, WM_LBUTTONDOWN, 0, MAKELPARAM(320, 240));
			scene.LastMove.reset();
			Window::WindowProcedure(host, loss, 0, 0);
			EXPECT_EQ(base::Action::MODIFIER_NONE, window.Modifiers());
			EXPECT_FALSE(scene.LastMove); // Scene cleanup is still deferred.
			Window::WindowProcedure(host, WM_SETFOCUS, 0, 0);
			Window::WindowProcedure(host, WM_LBUTTONDOWN, 0, MAKELPARAM(320, 240));
			ASSERT_TRUE(scene.LastMove);
			EXPECT_EQ(0u, scene.LastMove->MouseButtonsDown);
			ASSERT_TRUE(scene.LastTouch);
			EXPECT_EQ(base::Action::MODIFIER_NONE, scene.LastTouch->Modifiers);
			EXPECT_EQ(1u, scene.LastTouch->MouseButtonsDown);
			EXPECT_FALSE(window.ProcessDeferredInputLoss());
			EXPECT_TRUE(window.CancelMouseCapture()); // The fresh press survived.
			// A late release from the old focus must remain harmless.
			Window::WindowProcedure(host, WM_KEYUP, modifier, 0);
			EXPECT_EQ(base::Action::MODIFIER_NONE, window.Modifiers());
		}
	// A fresh modifier press before Render must survive deferred cleanup too.
	Window::WindowProcedure(host, WM_KILLFOCUS, 0, 0);
	Window::WindowProcedure(host, WM_SETFOCUS, 0, 0);
	Window::WindowProcedure(host, WM_KEYDOWN, VK_CONTROL, 0);
	EXPECT_FALSE(window.ProcessDeferredInputLoss());
	EXPECT_EQ(base::Action::MODIFIER_CTRL, window.Modifiers());
	Window::WindowProcedure(host, WM_KEYUP, VK_CONTROL, 0);
	DestroyWindow(host);
}

TEST(Window, ExternalCaptureLossClearsKeyboardStateEvenWithoutPointerGesture)
{
	CameraActivityScene scene;
	ResourceLib resources;
	CameraActivityWindow window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	Window::WindowProcedure(host, WM_KEYDOWN, VK_LWIN, 0);
	Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 0);
	scene.ArmIdle(false);
	// A delayed notification from an ordinary release must preserve real holds.
	Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, 0);
	EXPECT_EQ(base::Action::MODIFIER_ALT, window.Modifiers());
	EXPECT_FALSE(window.ProcessDeferredInputLoss());
	Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(GetDesktopWindow()));
	EXPECT_EQ(base::Action::MODIFIER_NONE, window.Modifiers());
	EXPECT_TRUE(window.ProcessDeferredInputLoss());
	EXPECT_TRUE(scene.AutomaticAtOriginalDeadline());
	DestroyWindow(host);
}

class NativeReentryScene final : public Scene
{
public:
	NativeReentryScene() : Scene(SceneParams(base::DrawableParams(),
		base::MoveableParams(), base::SizeableParams({ 640u, 480u })), {}) {}
	actions::ActionResult OnAction(actions::KeyAction action) override
	{
		++KeyCalls;
		if (action.KeyChar == 49u)
		{
			InsidePluginCall = true;
			if (Destroy)
				Window::WindowProcedure(Host, WM_DESTROY, 0, 0);
			else if (Overflow)
			{
				for (unsigned int index = 0u; index < 300u; ++index)
					Window::WindowProcedure(Host, WM_KEYDOWN, 50u, 0);
			}
			else
			{
				Window::WindowProcedure(Host, WM_KEYUP, 50u, 0);
				Window::WindowProcedure(Host, WM_SIZE, SIZE_RESTORED, MAKELPARAM(320, 200));
				Window::WindowProcedure(Host, WM_MOVE, 0, 0);
				// This would attempt a nested GL render and Scene lock acquisition.
				Window::WindowProcedure(Host, WM_SIZING, 0, 0);
			}
			EXPECT_EQ(1u, KeyCalls);
			EXPECT_EQ(640u, Owner->GetSize().Width);
			InsidePluginCall = false;
		}
		else EXPECT_FALSE(InsidePluginCall);
		return actions::ActionResult::NoAction();
	}
	actions::ActionResult OnAction(actions::TouchAction) override
	{
		return actions::ActionResult::NoAction();
	}
	actions::ActionResult OnAction(actions::TouchMoveAction) override
	{
		EXPECT_FALSE(InsidePluginCall);
		++CancelCalls;
		return actions::ActionResult::NoAction();
	}
	HWND Host = nullptr;
	Window* Owner = nullptr;
	bool InsidePluginCall = false;
	bool Overflow = false;
	bool Destroy = false;
	unsigned int CancelCalls = 0u;
	unsigned int KeyCalls = 0u;
};

TEST(Window, PluginPumpedInputAndResizeWaitUntilOuterSceneDispatchReturns)
{
	NativeReentryScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	scene.Host = host;
	scene.Owner = &window;
	actions::KeyAction key;
	key.KeyChar = 49u;
	key.KeyActionType = actions::KeyAction::KEY_DOWN;
	window.OnAction(key);
	EXPECT_EQ(1u, scene.KeyCalls);
	MSG pending{};
	unsigned int keysDelivered = 0u;
	unsigned int sizesDelivered = 0u;
	unsigned int movesDelivered = 0u;
	while (PeekMessageW(&pending, host, 0, 0, PM_REMOVE))
	{
		if (pending.message == WM_KEYUP) ++keysDelivered;
		if (pending.message == WM_SIZE) ++sizesDelivered;
		if (pending.message == WM_MOVE) ++movesDelivered;
		Window::WindowProcedure(host, pending.message, pending.wParam, pending.lParam);
	}
	EXPECT_EQ(1u, keysDelivered);
	EXPECT_EQ(1u, sizesDelivered);
	EXPECT_EQ(1u, movesDelivered);
	EXPECT_EQ(2u, scene.KeyCalls);
	EXPECT_EQ(320u, window.GetSize().Width);
	EXPECT_EQ(200u, scene.GetSize().Height);
	DestroyWindow(host);
}

TEST(Window, NativeReentryOverflowDropsWholeGestureAndRequestsInputReset)
{
	NativeReentryScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	scene.Host = host;
	scene.Owner = &window;
	scene.Overflow = true;
	actions::KeyAction key;
	key.KeyChar = 49u;
	key.KeyActionType = actions::KeyAction::KEY_DOWN;
	window.OnAction(key);
	MSG pending{};
	EXPECT_FALSE(PeekMessageW(&pending, host, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
	EXPECT_EQ(1u, scene.KeyCalls);
	EXPECT_TRUE(window.ProcessDeferredInputLoss());
	EXPECT_FALSE(window.ProcessDeferredInputLoss());
	DestroyWindow(host);
}

class NativeDestroyWindow final : public Window
{
public:
	NativeDestroyWindow(NativeReentryScene& scene, ResourceLib& resources) :
		Window(scene, resources), _scene(scene) {}
	using Window::OnAction;
	actions::ActionResult OnAction(actions::WindowAction action) override
	{
		if (action.WindowEventType == actions::WindowAction::DESTROY)
		{
			EXPECT_FALSE(_scene.InsidePluginCall);
			++DestroyCalls;
		}
		return Window::OnAction(action);
	}
	unsigned int DestroyCalls = 0u;
private:
	NativeReentryScene& _scene;
};

TEST(Window, PluginPumpedNativeDestroyCancelsCaptureAfterSceneDispatchReturns)
{
	NativeReentryScene scene;
	ResourceLib resources;
	NativeDestroyWindow window(scene, resources);
	const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(host, nullptr);
	SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
	scene.Host = host;
	scene.Owner = &window;
	scene.Destroy = true;
	actions::TouchAction touch;
	touch.State = actions::TouchAction::TOUCH_DOWN;
	window.OnAction(touch);
	actions::KeyAction key;
	key.KeyChar = 49u;
	key.KeyActionType = actions::KeyAction::KEY_DOWN;
	window.OnAction(key);
	EXPECT_EQ(1u, scene.CancelCalls);
	EXPECT_EQ(1u, window.DestroyCalls);
	EXPECT_FALSE(window.CancelMouseCapture());
	MSG quit{};
	// PostQuitMessage is a lazy thread-queue notification, so it need not be
	// visible while unrelated messages remain. The action/cancellation above
	// directly verify the deferred teardown contract.
	PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
	DestroyWindow(host);
}

class WindowTitleScene final : public Scene
{
public:
	WindowTitleScene() : Scene(SceneParams(base::DrawableParams(), base::MoveableParams(),
		base::SizeableParams({640u,480u})), {}) {}
	void CompleteSave(bool success, const std::wstring& path, bool asDefault = false)
	{
		_pendingSaveFolder = L"C:/isolated-jamma/export";
		_pendingSaveAsDefault = asDefault;
		_CompleteSessionSave(success, path, "Save cancelled or destination unavailable");
	}
	// Native title/replacement tests need no renderer or installed resources.
	void InitResources(ResourceLib&, bool) override {}
protected:
	void _ReleaseResources() override {}
};

class WindowTitleNativeHost
{
public:
	explicit WindowTitleNativeHost(Window& window) : _window(window)
	{
		Handle = CreateWindowExW(0, L"STATIC", L"initial caption", WS_POPUP,
			0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!Handle) return;
		SetWindowLongPtrW(Handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		_previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(Handle, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(Procedure)));
		_window.SetWindowHandle(Handle);
	}
	~WindowTitleNativeHost()
	{
		_window.SetWindowHandle(nullptr);
		if (!Handle) return;
		SetWindowLongPtrW(Handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_previous));
		SetWindowLongPtrW(Handle, GWLP_USERDATA, 0);
		DestroyWindow(Handle);
	}
	std::wstring Text() const
	{
		std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(Handle)) + 1u, L'\0');
		const auto length = GetWindowTextW(Handle, text.data(), static_cast<int>(text.size()));
		text.resize(static_cast<std::size_t>(length));
		return text;
	}
	HWND Handle = nullptr;
	unsigned int Changes = 0u;
private:
	static LRESULT CALLBACK Procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
	{
		auto& host = *reinterpret_cast<WindowTitleNativeHost*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
		if (message == WM_SETTEXT) ++host.Changes;
		return CallWindowProcW(host._previous, handle, message, wparam, lparam);
	}
	Window& _window;
	WNDPROC _previous = nullptr;
};

TEST(WindowTitle, UnsavedUnicodeAndNumberedDestinationsUpdateNativeCaptionOnlyWhenChanged)
{
	WindowTitleScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	WindowTitleNativeHost host(window);
	ASSERT_NE(nullptr, host.Handle);
	window.UpdateSessionTitle();
	EXPECT_EQ(L"Jamma", host.Text());
	for (unsigned int frame = 0u; frame < 8u; ++frame) window.UpdateSessionTitle();
	EXPECT_EQ(1u, host.Changes);
	scene.SetCurrentJamPath(L"C:\\sessions\\\u6f14\u594f-\u03b2.jam");
	window.UpdateSessionTitle();
	EXPECT_EQ(L"Jamma - \u6f14\u594f-\u03b2.jam", host.Text());
	EXPECT_EQ(2u, host.Changes);
	scene.CompleteSave(true, L"C:/isolated-jamma/export/session-0002.jam");
	window.UpdateSessionTitle();
	EXPECT_EQ(L"Jamma - session-0002.jam", host.Text());
	EXPECT_EQ(3u, host.Changes);
	window.UpdateSessionTitle();
	EXPECT_EQ(3u, host.Changes);
}

TEST(WindowTitle, WrittenJamStillNamesWindowWhenDefaultUpdateFailsAndFailedSavesKeepIt)
{
	WindowTitleScene scene;
	ResourceLib resources;
	Window window(scene, resources);
	WindowTitleNativeHost host(window);
	ASSERT_NE(nullptr, host.Handle);
	scene.SetCurrentJamPath(L"previous.jam");
	window.UpdateSessionTitle();
	unsigned int defaultsWrites = 0u;
	scene.SetDefaultJamSaver(L"startup.jam", [&](const auto&, bool) { ++defaultsWrites; return false; });
	scene.CompleteSave(true, L"C:/isolated-jamma/export/committed-0003.jam", true);
	window.UpdateSessionTitle();
	EXPECT_EQ(1u, defaultsWrites);
	EXPECT_EQ(L"Jamma - committed-0003.jam", host.Text());
	EXPECT_EQ(2u, host.Changes);
	for (const auto* path : {L"", L"denied.jam"})
	{
		scene.CompleteSave(false, path, true);
		window.UpdateSessionTitle();
		EXPECT_EQ(L"Jamma - committed-0003.jam", host.Text());
		EXPECT_EQ(2u, host.Changes);
	}
	EXPECT_EQ(1u, defaultsWrites);
}

TEST(WindowTitle, ReplacementResetsUnsavedCaptionAndOldSceneCompletionCannotRenameIt)
{
	WindowTitleScene outgoing, unsaved, loaded;
	ResourceLib resources;
	Window window(outgoing, resources);
	WindowTitleNativeHost host(window);
	ASSERT_NE(nullptr, host.Handle);
	outgoing.SetCurrentJamPath(L"outgoing.jam");
	window.UpdateSessionTitle();
	EXPECT_EQ(L"Jamma - outgoing.jam", host.Text());
	window.ReplaceScene(unsaved);
	EXPECT_EQ(L"Jamma", host.Text());
	EXPECT_EQ(2u, host.Changes);
	outgoing.CompleteSave(true, L"late-outgoing-save.jam");
	window.UpdateSessionTitle();
	EXPECT_EQ(L"Jamma", host.Text());
	EXPECT_EQ(2u, host.Changes);

	loaded.SetCurrentJamPath(L"C:/sessions/loaded-\u03a9.jam");
	window.ReplaceScene(loaded);
	EXPECT_EQ(L"Jamma - loaded-\u03a9.jam", host.Text());
	window.UpdateSessionTitle();
	EXPECT_EQ(3u, host.Changes);
}
