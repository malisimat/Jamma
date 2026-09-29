
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

	actions::ActionResult OnAction(actions::TouchAction) override
	{
		return actions::ActionResult::NoAction();
	}
	actions::ActionResult OnAction(actions::TouchMoveAction action) override
	{
		LastMove = action;
		return actions::ActionResult::NoAction();
	}

	std::optional<actions::TouchMoveAction> LastMove;
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
