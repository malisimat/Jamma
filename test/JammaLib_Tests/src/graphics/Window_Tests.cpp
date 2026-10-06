#define GLEW_STATIC
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "graphics/Window.h"
#include "engine/Scene.h"
#include "graphics/GlDeleteQueue.h"
#include "resources/ResourcePaths.h"
#include <filesystem>
#include <cstdlib>

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

// Opt-in GPU evidence; ordinary native runs stay independent of a desktop/driver.
// This owns a hidden test window, not another application's UI.
class NativeGuiRenderEvidence
{
public:
	~NativeGuiRenderEvidence()
	{
		if (Rc) {
			wglMakeCurrent(Dc, Rc);
			if (OwnsDeletes) graphics::GlDeleteQueue::FlushPendingDeletes();
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(Rc);
		}
		if (OwnsDeletes) graphics::GlDeleteQueue::SetRenderThread(PreviousRenderThread);
		if (Dc && Wnd) ReleaseDC(Wnd, Dc);
		if (Wnd) DestroyWindow(Wnd);
	}
	bool Initialize()
	{
		// Reject pre-existing GL work: names from a different context must never
		// be deleted in this temporary context. Run this opt-in test in isolation.
		if (wglGetCurrentContext()) return false;
		{
			std::lock_guard<std::mutex> lock(graphics::GlDeleteQueue::_Mutex());
			if (!graphics::GlDeleteQueue::_Queue().empty()) return false;
			PreviousRenderThread = graphics::GlDeleteQueue::_RenderThreadId();
		}
		Wnd = CreateWindowExW(0, L"STATIC", L"Jamma render evidence", WS_POPUP,
			0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!Wnd) return false;
		Dc = GetDC(Wnd);
		PIXELFORMATDESCRIPTOR format{};
		format.nSize = sizeof(format); format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32; format.cAlphaBits = 8; format.cDepthBits = 24;
		const auto index = ChoosePixelFormat(Dc, &format);
		if (!index || !SetPixelFormat(Dc, index, &format)) return false;
		Rc = wglCreateContext(Dc);
		if (!Rc || !wglMakeCurrent(Dc, Rc)) return false;
		if (glewInit() != GLEW_OK) return false;
		std::cout << "Evidence GL renderer: " << glGetString(GL_RENDERER) << "\nEvidence GL version: " << glGetString(GL_VERSION) << '\n';
		while (glGetError() != GL_NO_ERROR) {}
		graphics::GlDeleteQueue::SetRenderThread(std::this_thread::get_id());
		OwnsDeletes = true;
		return GLEW_VERSION_3_3 != 0;
	}
	static bool LoadUiResources(resources::ResourceLib& resources)
	{
		std::ifstream list(resources::ResolveResourceListPath());
		if (!list) return false;
		bool success = true;
		std::string line;
		while (std::getline(list, line)) {
			std::stringstream stream(line);
			int type = 0; std::string name, argument;
			if (!(stream >> type >> name)) continue;
			const bool uiShader = name == "texture" || name == "texture_tinted" || name == "font" ||
				name == "vu" || name == "cable" || name == "colour" || name == "ctrl_handle";
			if (type != resources::TEXTURE && !(type == resources::SHADER && uiShader)) continue;
			std::vector<std::string> arguments;
			while (stream >> argument) arguments.push_back(argument);
			success = resources.LoadResource(static_cast<resources::Type>(type), name, arguments) && success;
		}
		return resources.LoadFonts() && success;
	}
	static bool SaveBmp(const std::filesystem::path& path, utils::Size2d size, std::vector<unsigned char> pixels)
	{
		for (size_t index = 0; index < pixels.size(); index += 4) std::swap(pixels[index], pixels[index + 2]);
		BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
		file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
		BITMAPINFOHEADER bitmap{}; bitmap.biSize = sizeof(bitmap); bitmap.biWidth = size.Width;
		bitmap.biHeight = size.Height; bitmap.biPlanes = 1; bitmap.biBitCount = 32; bitmap.biCompression = BI_RGB;
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(&file), sizeof(file));
		output.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
		output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
		return output.good();
	}
	static std::shared_ptr<gui::GuiMainPanel> Settings(Scene& scene, utils::Position2d point)
	{
		actions::TouchAction down;
		down.Touch = actions::TouchAction::TOUCH_MOUSE; down.Index = 0;
		down.State = actions::TouchAction::TOUCH_DOWN; down.Position = point;
		auto active = scene.OnAction(down).ActiveElement.lock();
		actions::TouchMoveAction cancel; cancel.Position = point; cancel.MouseButtonsDown = 0;
		scene.OnAction(cancel);
		return active ? std::dynamic_pointer_cast<gui::GuiMainPanel>(active->Parent()) : nullptr;
	}
	static bool SameRegion(const std::vector<unsigned char>& first, const std::vector<unsigned char>& second,
		utils::Size2d size, utils::Rect2d region)
	{
		if (first.size() != second.size()) return false;
		for (int y = region.Bottom; y < region.Top; ++y)
			for (int x = region.Left; x < region.Right; ++x)
				for (size_t channel = 0; channel < 4; ++channel) {
					const auto offset = (static_cast<size_t>(y) * size.Width + x) * 4u + channel;
					if (first[offset] != second[offset]) return false;
				}
		return true;
	}
private:
	HWND Wnd = nullptr;
	HDC Dc = nullptr;
	HGLRC Rc = nullptr;
	std::thread::id PreviousRenderThread{};
	bool OwnsDeletes = false;
};

TEST(GuiRenderEvidence, ProductionPanelsHudAndFontPathsRenderOnActualGpu)
{
	const char* directory = std::getenv("JAMMA_RENDER_EVIDENCE_DIR");
	if (!directory || !*directory) GTEST_SKIP() << "Set JAMMA_RENDER_EVIDENCE_DIR to run the opt-in GPU evidence pass.";
	const std::filesystem::path output(directory);
	std::filesystem::create_directories(output);
	NativeGuiRenderEvidence gpu;
	ASSERT_TRUE(gpu.Initialize()) << "Hidden WGL context with OpenGL 3.3 is required; no desktop automation is used.";
	resources::ResourceLib resources;
	ASSERT_TRUE(NativeGuiRenderEvidence::LoadUiResources(resources));
	utils::Size2d size{ 1280, 720 };
	Scene scene(SceneParams({ "" }, {}, base::SizeableParams{ size, {} }), {});
	auto settings = NativeGuiRenderEvidence::Settings(scene, { 30, 10 });
	auto selection = NativeGuiRenderEvidence::Settings(scene, { 30, 710 });
	ASSERT_TRUE(settings); ASSERT_TRUE(selection);
	gui::GuiHudParams hudParams; hudParams.Size = size;
	auto hud = std::make_shared<gui::GuiHud>(hudParams);
	engine::RigSnapshot routing; routing.Revision = 1;
	for (size_t index = 0; index < 16; ++index) {
		io::RigFileRouting::TriggerResolution trigger;
		trigger.TriggerIndex = index; trigger.TriggerName = "Agjpq trigger " + std::to_string(index);
		trigger.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, static_cast<unsigned int>(index % 8), {}, true });
		routing.Graph.Triggers.push_back(trigger);
	}
	hud->SetRoutingConfig(8, { "Agjpq 0123 very long MIDI identity", "Offline source identity" }, routing);
	hud->SetCableRevealHeld(true);
	hud->Init();
	settings->InitResources(resources, false); selection->InitResources(resources, false); hud->InitResources(resources, false);
	auto probeParams = gui::GuiButtonParams::PanelButton(200);
	probeParams.Position = { 700, 200 }; probeParams.Size = { 200, 36 };
	probeParams.Text = "Agjpq 0123456789";
	auto probe = std::make_shared<gui::GuiButton>(probeParams);
	probe->Init();
	probe->InitResources(resources, false);
	const auto render = [&](const std::string& name) {
		graphics::GlDrawContext context(size, base::DrawContext::TEXTURE);
		context.Initialise();
		context.Bind(); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glClearColor(0.85f, 0.85f, 0.85f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
		context.ClearMvp();
		auto projection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.0f));
		projection = glm::scale(projection, glm::vec3(2.0f / size.Width, 2.0f / size.Height, 1.0f));
		context.PushMvp(projection);
		hud->Draw(context); settings->Draw(context); selection->Draw(context);
		// The same texture/font draws follow both fully opaque and fading panels.
		probe->Draw(context);
		EXPECT_FLOAT_EQ(1.0f, context.Opacity());
		glFinish();
		EXPECT_EQ(GL_NO_ERROR, glGetError());
		auto pixels = context.GetPixels();
		if (!name.empty()) EXPECT_TRUE(NativeGuiRenderEvidence::SaveBmp(output / (name + ".bmp"), size, pixels));
		return pixels;
	};
	// HUD cable reveal has its own existing frame-based fade; settle it before
	// comparing pixels so it cannot masquerade as panel opacity leakage.
	for (int frame = 0; frame < 7; ++frame) render("");
	const auto expanded = render("expanded-timing");
	// Empty interior over a bright background: 80% means 80% effective fill,
	// rather than 80% multiplied by an already translucent texture centre.
	for (int channel = 0; channel < 3; ++channel) {
		const auto expected = 255.0f * (gui::GuiStyle::Graphite()[channel] * gui::GuiStyle::PanelFillOpacity +
			0.85f * (1.0f - gui::GuiStyle::PanelFillOpacity));
		EXPECT_NEAR(expected, expanded[(110u * size.Width + 300u) * 4u + channel], 2.0f);
	}
	settings->SetPage(gui::SettingsPage::Midi); render("expanded-midi");
	settings->SetExpanded(false); selection->SetExpanded(false);
	settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f);
	const auto closing = render("closing-panels");
	EXPECT_TRUE(NativeGuiRenderEvidence::SameRegion(expanded, closing, size, { 700, 200, 900, 236 }));
	for (int frame = 0; frame < 5; ++frame) { settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f); }
	const auto collapsed = render("collapsed-panels");
	EXPECT_TRUE(NativeGuiRenderEvidence::SameRegion(expanded, collapsed, size, { 700, 200, 900, 236 }));
	settings->SetExpanded(true); selection->SetExpanded(true);
	settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f);
	const auto progress = settings->TransitionValue();
	size = { 320, 180 };
	settings->SetViewportSize(size); selection->SetViewportSize(size); hud->SetSize(size);
	EXPECT_FLOAT_EQ(progress, settings->TransitionValue());
	render("small-mid-animation");
	for (int frame = 0; frame < 5; ++frame) { settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f); }
	settings->SetPage(gui::SettingsPage::Timing); render("small-expanded");
	size = { 1280, 720 };
	settings->SetViewportSize(size); selection->SetViewportSize(size); hud->SetSize(size);
	const auto restored = render("large-restored");
	EXPECT_TRUE(NativeGuiRenderEvidence::SameRegion(expanded, restored, size, { 700, 200, 900, 236 }));
	probe->ReleaseResources(); settings->ReleaseResources(); selection->ReleaseResources(); hud->ReleaseResources();
	scene.Shutdown();
}
