#define GLEW_STATIC
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "graphics/Window.h"
#include "engine/Scene.h"

using resources::ResourceLib;
using graphics::Window;
using engine::Scene;
using engine::SceneParams;

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
