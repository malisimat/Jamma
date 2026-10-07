#define GLEW_STATIC
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "graphics/Window.h"
#include "engine/Scene.h"
#include "graphics/GlDeleteQueue.h"
#include "resources/ResourcePaths.h"
#include "gui/GuiDropDown.h"
#include "gui/GuiNumericInput.h"
#include "gui/GuiRack.h"
#include "audio/AudioMixer.h"
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include "SceneGuiBenchmark.h"

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
	static std::shared_ptr<gui::GuiHud> Hud(Scene& scene, utils::Position2d point)
	{
		actions::TouchAction down; down.Touch = actions::TouchAction::TOUCH_MOUSE;
		down.State = actions::TouchAction::TOUCH_DOWN; down.Index = 0; down.Position = point;
		auto active = scene.OnAction(down).ActiveElement.lock();
		actions::TouchMoveAction cancel; cancel.Position = point; cancel.MouseButtonsDown = 0;
		scene.OnAction(cancel);
		for (auto element = active; element; element = element->Parent())
			if (auto hud = std::dynamic_pointer_cast<gui::GuiHud>(element)) return hud;
		return nullptr;
	}
	static std::vector<std::shared_ptr<gui::GuiScrollPanel>> Scrolls(const std::shared_ptr<base::GuiElement>& element)
	{
		std::vector<std::shared_ptr<gui::GuiScrollPanel>> result;
		for (unsigned int index = 0; index < 256; ++index) {
			const auto child = element->TryGetChild(static_cast<unsigned char>(index));
			if (!child) break;
			if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(child)) result.push_back(scroll);
			const auto descendants = Scrolls(child); result.insert(result.end(), descendants.begin(), descendants.end());
		}
		return result;
	}
	static void BeginFrame(graphics::GlDrawContext& context, utils::Size2d size, float background)
	{
		context.Bind(); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glClearColor(background, background, background, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
		context.ClearMvp();
		auto projection = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.0f));
		context.PushMvp(glm::scale(projection, glm::vec3(2.0f / size.Width, 2.0f / size.Height, 1.0f)));
	}
	static size_t WhitePixels(const std::vector<unsigned char>& pixels, utils::Size2d size, utils::Rect2d region)
	{
		region = region.Intersected({ 0, 0, static_cast<int>(size.Width), static_cast<int>(size.Height) });
		size_t count = 0;
		for (int y = region.Bottom; y < region.Top; ++y)
			for (int x = region.Left; x < region.Right; ++x) {
				const auto offset = (static_cast<size_t>(y) * size.Width + x) * 4u;
				if (pixels[offset] > 210 && pixels[offset + 1] > 210 && pixels[offset + 2] > 210) ++count;
			}
		return count;
	}
private:
	HWND Wnd = nullptr;
	HDC Dc = nullptr;
	HGLRC Rc = nullptr;
	std::thread::id PreviousRenderThread{};
	bool OwnsDeletes = false;
};

class FaderEvidenceRack : public gui::GuiRack
{
public:
	FaderEvidenceRack() : GuiRack([] {
		gui::GuiRackParams params; params.Size = { 106, 320 }; return params;
	}()) {}
	using GuiRack::_GetSliderParams;
};

class FaderEvidenceSlider : public gui::GuiSlider
{
public:
	using GuiSlider::GuiSlider;
	using GuiSlider::CalcDragPos;
};

TEST(GuiRenderEvidence, RackFaderScalesStatesHeightsAndResize)
{
	const char* directory = std::getenv("JAMMA_RENDER_EVIDENCE_DIR");
	if (!directory || !*directory) GTEST_SKIP() << "Set JAMMA_RENDER_EVIDENCE_DIR for GPU evidence.";
	const std::filesystem::path output(directory);
	std::filesystem::create_directories(output);
	NativeGuiRenderEvidence gpu;
	ASSERT_TRUE(gpu.Initialize());
	resources::ResourceLib resources;
	ASSERT_TRUE(NativeGuiRenderEvidence::LoadUiResources(resources));
	FaderEvidenceRack rack;
	const utils::Size2d viewport{ 500, 820 };
	std::vector<std::shared_ptr<gui::GuiSlider>> sliders;
	std::vector<gui::GuiSliderParams> sliderParams;
	std::vector<std::shared_ptr<audio::AudioMixer>> mixers;
	for (unsigned int column = 0; column < 6; ++column) {
		auto params = rack._GetSliderParams(column < 3 ? 0 : 1, { 106, 320 });
		params.Position = { 20 + static_cast<int>(column) * 78, 20 };
		auto slider = std::make_shared<FaderEvidenceSlider>(params);
		auto mixer = std::make_shared<audio::AudioMixer>(audio::AudioMixerParams{});
		mixer->UpdateVu(0.6f, 256);
		slider->SetMixer(mixer);
		slider->Init(); slider->InitResources(resources, false);
		sliders.push_back(slider); mixers.push_back(mixer);
		sliderParams.push_back(params);
	}
	for (const auto height : { 100u, 260u, 746u }) {
		for (unsigned int column = 0; column < sliders.size(); ++column) {
			auto& slider = sliders[column];
			slider->ClearPointerState();
			slider->SetSize({ slider->GetSize().Width, height });
			slider->SetValue(column % 3 == 0 ? 0.0 : column % 3 == 1 ? 1.0 : std::pow(10.0, 16.0 / 20.0), true);
		}
		for (const bool handle : { false, true })
		for (const auto state : { "normal", "over", "down" }) {
			graphics::GlDrawContext context(viewport, base::DrawContext::TEXTURE);
			context.Initialise(); NativeGuiRenderEvidence::BeginFrame(context, viewport, 0.12f);
			for (unsigned int column = 0; column < sliders.size(); ++column) {
				auto& slider = sliders[column];
				slider->ClearPointerState();
				if (std::string(state) != "normal") {
					actions::TouchMoveAction hover;
					const auto drag = FaderEvidenceSlider::CalcDragPos(sliderParams[column], slider->GetSize(), slider->Value());
					hover.Position = handle ? utils::Position2d{ static_cast<int>(slider->GetSize().Width / 2),
						drag.Y + static_cast<int>(sliderParams[column].DragControlSize.Height / 2) } : utils::Position2d{ 1, 2 };
					slider->OnAction(hover);
					EXPECT_EQ(handle, slider->DragHandleIsOverForTest());
					if (std::string(state) == "down") {
						actions::TouchAction down;
						down.Touch = actions::TouchAction::TOUCH_MOUSE; down.State = actions::TouchAction::TOUCH_DOWN;
						down.Position = hover.Position; slider->OnAction(down);
					}
				}
				const glm::vec3 sentinelTint(0.2f, 0.4f, 0.6f);
				if (column > 0) context.SetUniform("TintColor", sentinelTint);
				{
					auto inheritedOpacity = context.WithOpacity(column == 5 ? 0.5f : 1.0f);
					slider->Draw(context);
				}
				EXPECT_FLOAT_EQ(1.0f, context.Opacity());
				const auto restoredTint = context.GetUniform("TintColor");
				ASSERT_TRUE(restoredTint.has_value());
				EXPECT_EQ(column == 0 ? glm::vec3(1.0f) : sentinelTint,
					std::any_cast<glm::vec3>(*restoredTint));
			}
			glFinish(); EXPECT_EQ(GL_NO_ERROR, glGetError());
			const auto pixels = context.GetPixels();
			if (!handle && std::string(state) == "normal") {
				const auto marks = gui::GuiSlider::BuildScaleMarks(sliderParams[0], sliders[0]->GetSize());
				const auto unity = std::find_if(marks.begin(), marks.end(), [](const auto& mark) {
					return mark.Kind == gui::GuiSlider::ScaleMarkKind::Unity;
				});
				ASSERT_NE(marks.end(), unity);
				const auto pixel = [&](int x, int y, unsigned int channel) {
					return pixels[(static_cast<size_t>(20 + y) * viewport.Width + 20 + x) * 4u + channel];
				};
				// Gain zero leaves unity unobscured. These pixel checks also catch stale
				// mark geometry after each resize, and an accidental screen-Y reversal.
				EXPECT_GT(pixel(10, unity->CentreY, 0), pixel(10, unity->CentreY, 1));
				EXPECT_GT(pixel(10, unity->CentreY, 1), pixel(10, unity->CentreY, 2));
				EXPECT_GT(pixel(10, unity->CentreY, 0), 170);
				const auto track = gui::GuiSlider::BuildScaleTrackBounds(sliderParams[0], sliders[0]->GetSize());
				ASSERT_TRUE(track.Valid);
				int trackY = 40;
				while (std::any_of(marks.begin(), marks.end(), [trackY](const auto& mark) {
					return std::abs(mark.CentreY - trackY) <= 2;
				})) ++trackY;
				const int centreX = (track.Bounds.Left + track.Bounds.Right) / 2;
				for (int x = centreX - 2; x < centreX + 2; ++x)
					EXPECT_LE(pixel(x, trackY, 0), 1); // Allow driver blend rounding.
				EXPECT_GT(pixel(centreX - 3, trackY, 0), 0);
				EXPECT_GT(pixel(centreX + 2, trackY, 0), 0);
			}
			EXPECT_TRUE(NativeGuiRenderEvidence::SaveBmp(output / ("faders-" + std::to_string(height) +
				(handle ? "-handle-" : "-panel-") + state + ".bmp"), viewport, pixels));
		}
	}
}

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
	routing.Graph.Triggers.front().Sources.push_back({ io::RigFileRouting::SourceKind::Midi, 0u,
		"Unavailable Agjpq MIDI identity", false });
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
		NativeGuiRenderEvidence::BeginFrame(context, size, 0.85f);
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
	size = { 400, 240 };
	settings->SetViewportSize(size); selection->SetViewportSize(size); hud->SetSize(size);
	settings->SetExpanded(false); selection->SetExpanded(false);
	for (int frame = 0; frame < 5; ++frame) { settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f); }
	const auto compactHud = render("compact-hud-two-categories");
	unsigned int sourceViewports = 0;
	for (const auto& scroll : NativeGuiRenderEvidence::Scrolls(hud)) {
		const auto card = scroll->Content() ? scroll->Content()->TryGetChild(0) : nullptr;
		if (!card || card->GetSize().Height == 100u) continue;
		++sourceViewports; EXPECT_GE(scroll->GetSize().Width, 84u);
		EXPECT_EQ(80u, card->GetSize().Width);
		const auto position = card->GlobalPosition(); const auto cardSize = card->GetSize();
		EXPECT_GE(position.X, scroll->GlobalPosition().X);
		EXPECT_LE(position.X + static_cast<int>(cardSize.Width), scroll->GlobalPosition().X + static_cast<int>(scroll->GetSize().Width));
		EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(compactHud, size, { position.X, position.Y,
			position.X + static_cast<int>(cardSize.Width), position.Y + static_cast<int>(cardSize.Height) }), 5u);
	}
	EXPECT_EQ(2u, sourceViewports);
	settings->SetExpanded(true); selection->SetExpanded(true);
	for (int frame = 0; frame < 5; ++frame) { settings->AdvanceAnimation(0.05f); selection->AdvanceAnimation(0.05f); }
	size = { 1280, 720 };
	settings->SetViewportSize(size); selection->SetViewportSize(size); hud->SetSize(size);
	const auto restored = render("large-restored");
	EXPECT_TRUE(NativeGuiRenderEvidence::SameRegion(expanded, restored, size, { 700, 200, 900, 236 }));
	probe->ReleaseResources(); settings->ReleaseResources(); selection->ReleaseResources(); hud->ReleaseResources();
	scene.Shutdown();
}

class EvidenceTextBox final : public gui::GuiTextBox
{
public:
	using GuiTextBox::GuiTextBox;
	std::shared_ptr<gui::GuiLabel> Label() const { return _label; }
};

TEST(GuiRenderEvidence, ControlFamiliesCaretSelectionPopupAndNestedOpacity)
{
	const char* directory = std::getenv("JAMMA_RENDER_EVIDENCE_DIR");
	if (!directory || !*directory) GTEST_SKIP() << "Set JAMMA_RENDER_EVIDENCE_DIR for GPU evidence.";
	const std::filesystem::path output(directory);
	std::filesystem::create_directories(output);
	NativeGuiRenderEvidence gpu;
	ASSERT_TRUE(gpu.Initialize());
	resources::ResourceLib resources;
	ASSERT_TRUE(NativeGuiRenderEvidence::LoadUiResources(resources));
	const utils::Size2d size{ 900, 600 };
	gui::GuiPopupManager popups;
	base::GuiElementParams rootParams;
	rootParams.Position = { 20, 50 }; rootParams.Size = { 860, 530 };
	auto root = std::make_shared<gui::GuiPanel>(rootParams);
	std::shared_ptr<EvidenceTextBox> target;
	std::shared_ptr<gui::GuiDropDown> dropdown;
	const std::array<unsigned int, 4> heights{ 17, 25, 36, 43 };
	for (size_t row = 0; row < heights.size(); ++row) {
		const int y = 440 - static_cast<int>(row) * 65;
		auto title = gui::GuiLabelParams::PanelHeader("Height " + std::to_string(heights[row]), 150);
		title.Position = { 0, y + 45 }; root->AddChild(std::make_shared<gui::GuiLabel>(title));
		auto button = gui::GuiButtonParams::PanelButton(160);
		button.Position = { 0, y }; button.Size.Height = heights[row]; button.Text = "Agjpq 0123";
		root->AddChild(std::make_shared<gui::GuiButton>(button));
		auto toggle = gui::GuiToggleParams::PanelPrimary();
		toggle.Position = { 172, y }; toggle.Size = { 160, heights[row] };
		toggle.Text = "Agjpq toggle"; toggle.InitState = gui::GuiToggleParams::TOGGLE_ON;
		root->AddChild(std::make_shared<gui::GuiToggle>(toggle));
		auto text = gui::GuiTextBoxParams::PanelInput(160);
		text.Position = { 344, y }; text.Size.Height = heights[row]; text.Text = "Agjpq 0123";
		auto box = std::make_shared<EvidenceTextBox>(text); root->AddChild(box);
		if (heights[row] == 36) target = box;
		auto numeric = gui::GuiNumericInputParams::PanelInput(160);
		numeric.Position = { 516, y }; numeric.Size.Height = heights[row]; numeric.InitValue = 0.57;
		root->AddChild(std::make_shared<gui::GuiNumericInput>(numeric));
		auto drop = gui::GuiDropDownParams::PanelInput(160);
		drop.Position = { 688, y }; drop.Size.Height = heights[row];
		drop.Items = { "Agjpq 0123", "Tall glyphs pq", "Long identity abcdefghijklmnopqrstuvwxyz" };
		auto control = std::make_shared<gui::GuiDropDown>(drop);
		control->SetPopupManager(&popups); root->AddChild(control);
		if (heights[row] == 43) dropdown = control;
	}
	// One opaque textured centre isolates the nested multiplier from glyph/fill
	// overlap. A later ordinary draw separately checks scope restoration.
	base::GuiElementParams solid;
	solid.Position = { 520, 40 }; solid.Size = { 160, 36 };
	solid.Texture = "rounded_but_on"; solid.TextureShader = "texture_tinted"; solid.TintColor = glm::vec3(1.0f);
	root->AddChild(std::make_shared<gui::GuiPanel>(solid));
	auto ordinaryParams = gui::GuiButtonParams::PanelButton(180);
	ordinaryParams.Position = { 20, 10 }; ordinaryParams.Text = "Opaque Agjpq 0123";
	auto ordinary = std::make_shared<gui::GuiButton>(ordinaryParams);
	root->Init(); ordinary->Init();
	root->InitResources(resources, false); ordinary->InitResources(resources, false);
	ASSERT_TRUE(target); ASSERT_TRUE(dropdown);
	const auto render = [&](const std::string& name, bool nested = false) {
		graphics::GlDrawContext context(size, base::DrawContext::TEXTURE); context.Initialise();
		NativeGuiRenderEvidence::BeginFrame(context, size, 0.1f);
		{
			auto parent = context.WithOpacity(nested ? 0.5f : 1.0f);
			auto child = context.WithOpacity(nested ? 0.5f : 1.0f);
			root->Draw(context);
		}
		ordinary->Draw(context); popups.Draw(context);
		EXPECT_FLOAT_EQ(1.0f, context.Opacity()); glFinish(); EXPECT_EQ(GL_NO_ERROR, glGetError());
		auto pixels = context.GetPixels();
		EXPECT_TRUE(NativeGuiRenderEvidence::SaveBmp(output / (name + ".bmp"), size, pixels));
		return pixels;
	};
	const auto normal = render("control-families");
	for (size_t row = 0; row < heights.size(); ++row)
		for (int column = 0; column < 5; ++column) {
			const int x = 20 + column * 172, y = 490 - static_cast<int>(row) * 65;
			EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(normal, size, { x, y, x + 160, y + static_cast<int>(heights[row]) }), 5u)
				<< "No visible glyphs in control column " << column << " at height " << heights[row];
		}
	const auto faded = render("nested-opacity", true);
	for (unsigned int channel = 0; channel < 3; ++channel)
		EXPECT_NEAR(255.0f * (0.25f + 0.1f * 0.75f), faded[(108u * size.Width + 620u) * 4u + channel], 2.0f);
	EXPECT_TRUE(NativeGuiRenderEvidence::SameRegion(normal, faded, size, { 20, 10, 200, 46 }));
	for (const auto height : { 36u, 17u, 25u, 43u }) {
		target->SetSize({ 160, height }); target->ClearFocus();
		const auto unfocused = render("text-unfocused-" + std::to_string(height));
		target->RequestFocus();
		actions::KeyAction key; key.KeyActionType = actions::KeyAction::KEY_DOWN;
		key.KeyChar = VK_END; key.Modifiers = base::Action::MODIFIER_NONE; target->OnAction(key);
		key.KeyChar = VK_HOME; key.Modifiers = base::Action::MODIFIER_SHIFT; target->OnAction(key);
		ASSERT_TRUE(target->HasSelection());
		const auto focused = render("text-selection-" + std::to_string(height));
		const auto label = target->Label(); const auto font = label->ResolvedFont(); ASSERT_TRUE(font);
		const auto labelPosition = label->Position(); const auto labelSize = label->GetSize();
		const utils::Rect2d frame{ labelPosition.X, labelPosition.Y,
			labelPosition.X + static_cast<int>(labelSize.Width), labelPosition.Y + static_cast<int>(labelSize.Height) };
		const auto line = label->LineFrame(); ASSERT_TRUE(line);
		auto selection = gui::GuiTextBox::ResolveTextBand(frame, *line, 0, font->MeasureString(target->Text()));
		selection.Top = (std::min)(selection.Top, selection.Bottom + 2);
		const auto caret = gui::GuiTextBox::ResolveTextBand(frame, *line, 0, 2);
		const auto global = target->GlobalPosition();
		selection = selection.Translated(global); const auto caretGlobal = caret.Translated(global);
		size_t changed = 0;
		for (int y = 0; y < static_cast<int>(size.Height); ++y)
			for (int x = 0; x < static_cast<int>(size.Width); ++x) {
				const auto index = (static_cast<size_t>(y) * size.Width + x) * 4u;
				if (std::equal(unfocused.begin() + index, unfocused.begin() + index + 3u, focused.begin() + index)) continue;
				++changed;
				EXPECT_TRUE(selection.Contains({ x, y }) || caretGlobal.Contains({ x, y })) << x << "," << y;
			}
		EXPECT_GT(changed, 0u); target->ClearFocus();
	}
	dropdown->Open(); ASSERT_TRUE(popups.IsOpen());
	const auto popupPixels = render("dropdown-popup-rows");
	const auto popupPosition = popups.Top()->GlobalPosition();
	for (int row = 0; row < 3; ++row) {
		const int y = popupPosition.Y + row * gui::GuiDropDownParams::DefaultRowHeight;
		EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(popupPixels, size, { popupPosition.X + 8, y,
			popupPosition.X + 152, y + static_cast<int>(gui::GuiDropDownParams::DefaultRowHeight) }), 5u);
	}
	dropdown->Close();
	auto confirmation = std::make_shared<gui::GuiPopup>();
	confirmation->SetTitle("Agjpq Current server tempo");
	confirmation->SetBodyLines({ "Agjpq 0123 Tempo: 120 BPM", "Agjpq 4567 Remote interval", "Agjpq 8901 Apply locally?" });
	confirmation->ConfigureButtons({ { { "Cancel", 2u }, { "Follow server", 1u } } });
	confirmation->Init(); confirmation->InitResources(resources, false);
	for (const auto viewport : { utils::Size2d{ 320, 180 }, utils::Size2d{ 184, 161 }, utils::Size2d{ 460, 210 } }) {
		confirmation->FitToViewport(viewport);
		graphics::GlDrawContext context(viewport, base::DrawContext::TEXTURE); context.Initialise();
		NativeGuiRenderEvidence::BeginFrame(context, viewport, 0.85f);
		confirmation->Draw(context); EXPECT_FLOAT_EQ(1.0f, context.Opacity()); glFinish(); EXPECT_EQ(GL_NO_ERROR, glGetError());
		const auto pixels = context.GetPixels();
		EXPECT_TRUE(NativeGuiRenderEvidence::SaveBmp(output / ("confirmation-popup-" + std::to_string(viewport.Width) + "x" +
			std::to_string(viewport.Height) + ".bmp"), viewport, pixels));
		for (unsigned char index = 0; index < 4; ++index) {
			const auto label = confirmation->TryGetChild(index); const auto pos = label->GlobalPosition(); const auto frame = label->GetSize();
			EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(pixels, viewport, { pos.X, pos.Y,
				pos.X + static_cast<int>(frame.Width), pos.Y + static_cast<int>(frame.Height) }), 5u);
		}
		if (viewport.Width >= 320) {
			const auto label = std::dynamic_pointer_cast<gui::GuiLabel>(confirmation->TryGetChild(5)->TryGetChild(0));
			ASSERT_TRUE(label); const auto width = label->MeasureText("Follow server"); ASSERT_TRUE(width);
			EXPECT_LE(*width, static_cast<float>(label->GetSize().Width));
		}
		if (viewport.Height < 210)
			for (unsigned int channel = 0; channel < 3; ++channel) {
				const auto expected = 255.0f * (gui::GuiStyle::Graphite()[channel] * gui::GuiStyle::PanelFillOpacity +
					0.85f * (1.0f - gui::GuiStyle::PanelFillOpacity));
				EXPECT_NEAR(expected, pixels[(52u * viewport.Width + viewport.Width / 2u) * 4u + channel], 2.0f);
			}
	}
	confirmation->ReleaseResources(); ordinary->ReleaseResources(); root->ReleaseResources();
}

class ProductionWindowEvidenceCleanup
{
public:
	ProductionWindowEvidenceCleanup(Window& window, Scene& scene) : Win(window), EngineScene(scene)
	{
		std::lock_guard<std::mutex> lock(graphics::GlDeleteQueue::_Mutex());
		PreviousOwner = graphics::GlDeleteQueue::_RenderThreadId();
	}
	~ProductionWindowEvidenceCleanup()
	{
		Win.Release(); EngineScene.Shutdown();
		graphics::GlDeleteQueue::SetRenderThread(PreviousOwner);
	}
private:
	Window& Win;
	Scene& EngineScene;
	std::thread::id PreviousOwner{};
};

class PrivateGuiEvidenceDesktop final
{
public:
	PrivateGuiEvidenceDesktop() = default;
	PrivateGuiEvidenceDesktop(const PrivateGuiEvidenceDesktop&) = delete;
	PrivateGuiEvidenceDesktop& operator=(const PrivateGuiEvidenceDesktop&) = delete;
	~PrivateGuiEvidenceDesktop()
	{
		if (!Desktop) return;
		if (Attached && !SetThreadDesktop(Previous)) {
			ADD_FAILURE() << "Could not restore test thread desktop: " << GetLastError();
			return; // Windows must retain a desktop while a thread is attached.
		}
		EXPECT_TRUE(CloseDesktop(Desktop));
	}
	bool Initialize()
	{
		Previous = GetThreadDesktop(GetCurrentThreadId());
		const auto name = L"JammaGuiEvidence-" + std::to_wstring(GetCurrentProcessId());
		Desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
			DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
		if (!Desktop || !SetThreadDesktop(Desktop)) return false;
		Attached = true;
		return IsSeparateFromInput();
	}
	bool IsSeparateFromInput() const
	{
		const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
		if (!input) return false;
		std::array<wchar_t, 256> inputName{}, ownName{}; DWORD needed = 0;
		const bool names = GetUserObjectInformationW(input, UOI_NAME, inputName.data(), sizeof(inputName), &needed)
			&& GetUserObjectInformationW(Desktop, UOI_NAME, ownName.data(), sizeof(ownName), &needed);
		const bool closed = CloseDesktop(input) != FALSE;
		return names && closed && std::wstring(inputName.data()) != std::wstring(ownName.data());
	}
private:
	HDESK Previous = nullptr;
	HDESK Desktop = nullptr;
	bool Attached = false;
};

class ProductionWindowGuiEvidence final
{
public:
	static void Run();
};

void ProductionWindowGuiEvidence::Run()
{
	const char* directory = std::getenv("JAMMA_RENDER_EVIDENCE_DIR");
	if (!directory || !*directory || !std::getenv("JAMMA_WINDOW_RENDER_EVIDENCE"))
		GTEST_SKIP() << "Set JAMMA_RENDER_EVIDENCE_DIR and JAMMA_WINDOW_RENDER_EVIDENCE for the isolated production-window pass.";
	const std::filesystem::path output(directory);
	std::filesystem::create_directories(output);
	// Own all test windows on an undisplayed desktop. This permits real OS
	// maximize/minimize/restore without exposing them on the user's desktop.
	// Deliberately do not request DESKTOP_SWITCHDESKTOP or call SwitchDesktop.
	PrivateGuiEvidenceDesktop desktop;
	ASSERT_TRUE(desktop.Initialize()) << "Private test desktop setup failed: " << GetLastError();
	{
		NativeGuiRenderEvidence preflight;
		ASSERT_TRUE(preflight.Initialize());
		ASSERT_TRUE(GLEW_VERSION_4_0);
	}
	resources::ResourceLib resources;
	io::JamFile jam{}; io::RigFile rig{};
	const auto fixture = output / "production-fixture";
	std::filesystem::create_directories(fixture);
	constexpr unsigned int length = 48000;
	std::vector<float> samples(length);
	for (size_t index = 0; index < samples.size(); ++index)
		samples[index] = 0.0025f * static_cast<float>(std::sin(6.283185307179586 * 220.0 * index / length));
	ASSERT_TRUE(io::WavReadWriter().Write((fixture / "quiet-tone.wav").wstring(), samples, length, length));
	jam.MasterLengthSamps = length; jam.QuantiseSamps = 6000;
	for (unsigned int index = 0; index < 8; ++index) {
		io::JamFile::Station station{}; station.Name = "Station " + std::to_string(index);
		io::JamFile::LoopTake savedTake{}; savedTake.Name = station.Name + " Take";
		io::JamFile::Loop loop{}; loop.Name = "quiet-tone.wav"; loop.Id = savedTake.Name + " Audio";
		loop.Length = length; loop.Level = 0.25; loop.Speed = 1.0;
		loop.Mix.Mix = io::JamFile::LoopMix::MIX_WIRE; loop.Mix.Params = std::vector<unsigned long>{ 0, 1 };
		savedTake.Loops.push_back(loop); station.LoopTakes.push_back(savedTake);
		jam.Stations.push_back(station);
	}
	for (unsigned int index = 0; index < 16; ++index) {
		io::RigFile::Trigger trigger{}; trigger.Name = "Agjpq trigger " + std::to_string(index);
		trigger.StationTarget = jam.Stations[index % jam.Stations.size()].Name;
		trigger.InputChannels = { index % 8 }; rig.Triggers.push_back(trigger);
	}
	const auto loaded = Scene::FromFile(SceneParams({ "" }, {}, base::SizeableParams{ { 1000, 650 }, {} }), jam, rig, fixture.wstring());
	ASSERT_TRUE(loaded); auto scene = *loaded;
	scene->CommitChanges();
	ASSERT_EQ(8u, scene->SnapshotStations().size());
	for (const auto& station : scene->SnapshotStations()) {
		ASSERT_EQ(1u, station->GetLoopTakes().size());
		ASSERT_EQ(1u, station->GetLoopTakes().front()->GetLoops().size());
		EXPECT_EQ(length, station->GetLoopTakes().front()->GetLoops().front()->LoopLength());
	}
	auto take = scene->SnapshotStations().front()->GetLoopTakes().front();
	engine::LoopTake::MidiExportState midiState; midiState.LoopLengthSamps = length;
	engine::LoopTake::MidiStreamExport stream; stream.Channel = 0;
	stream.Loop.LoopLengthSamps = length; stream.Loop.EventCount = 2;
	stream.Loop.Events[0] = midi::MidiEvent::MakeNoteOn(6000u, 0u, 60u, 96u);
	stream.Loop.Events[1] = midi::MidiEvent::MakeNoteOff(18000u, 0u, 60u);
	midiState.Streams.push_back(stream);
	ASSERT_TRUE(take->RestoreMidiFromExport(midiState));
	Window window(*scene, resources);
	ProductionWindowEvidenceCleanup cleanup(window, *scene);
	// Window::Create can show a modal error on unsupported production formats.
	// Run this test in its own process with an external timeout.
	ASSERT_EQ(0, window.Create(GetModuleHandleW(nullptr), SW_HIDE));
	const auto ownedWindow = WindowFromDC(wglGetCurrentDC()); ASSERT_NE(nullptr, ownedWindow);
	EXPECT_FALSE(IsWindowVisible(ownedWindow));
	actions::KeyAction reveal; reveal.KeyChar = VK_OEM_3; reveal.KeyActionType = actions::KeyAction::KEY_DOWN;
	window.OnAction(reveal);
	for (int frame = 0; frame < 7; ++frame) { window.Render(); glFinish(); }
	const auto capture = [&](const std::string& name) {
		window.Render(); glFinish();
		const auto size = window.GetSize();
		std::vector<unsigned char> pixels(static_cast<size_t>(size.Width) * size.Height * 4u);
		glBindFramebuffer(GL_FRAMEBUFFER, 0); glReadBuffer(GL_BACK);
		glReadPixels(0, 0, size.Width, size.Height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		EXPECT_EQ(GL_NO_ERROR, glGetError());
		EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(pixels, size, { 0, 0, static_cast<int>(size.Width), static_cast<int>(size.Height) }), 100u);
		EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(pixels, size, { 20, 0, 160, 28 }), 5u);
		EXPECT_GT(NativeGuiRenderEvidence::WhitePixels(pixels, size, { 20, static_cast<int>(size.Height) - 28,
			160, static_cast<int>(size.Height) }), 5u);
		EXPECT_TRUE(NativeGuiRenderEvidence::SaveBmp(output / (name + ".bmp"), size, pixels));
		window.Swap(); EXPECT_EQ(GL_NO_ERROR, glGetError());
	};
	capture("production-window-expanded");
	auto hud = NativeGuiRenderEvidence::Hud(*scene, { 600, 582 }); ASSERT_TRUE(hud);
	const auto scrolls = NativeGuiRenderEvidence::Scrolls(hud);
	auto triggerScroll = std::find_if(scrolls.begin(), scrolls.end(), [](const auto& scroll) {
		return scroll->Content() && scroll->Content()->TryGetChild(0) &&
			scroll->Content()->TryGetChild(0)->GetSize().Height == 100u;
	});
	ASSERT_NE(scrolls.end(), triggerScroll);
	ASSERT_GT((*triggerScroll)->MaxScrollOffset(), 0);
	actions::TouchAction wheel; wheel.Touch = actions::TouchAction::TOUCH_MOUSE;
	wheel.State = actions::TouchAction::TOUCH_DOWN; wheel.Index = 4; wheel.Value = -1;
	const auto scrollPosition = (*triggerScroll)->GlobalPosition();
	wheel.Position = { scrollPosition.X + 60, scrollPosition.Y + 100 };
	const auto oldOffset = (*triggerScroll)->ScrollOffset();
	EXPECT_TRUE(window.OnAction(wheel).IsEaten);
	EXPECT_GT((*triggerScroll)->ScrollOffset(), oldOffset);
	capture("production-window-scrolled");
	(*triggerScroll)->SetScrollOffset((*triggerScroll)->MaxScrollOffset());
	capture("production-window-scroll-end");
	(*triggerScroll)->SetScrollOffset(0);
	auto settings = NativeGuiRenderEvidence::Settings(*scene, { 30, 10 }); ASSERT_TRUE(settings);
	settings->SetExpanded(false); settings->AdvanceAnimation(0.05f);
	capture("production-window-closing");
	settings->SetExpanded(true); settings->AdvanceAnimation(0.05f);
	const auto progress = settings->TransitionValue();
	const auto nativeResize = [&](utils::Size2d size) {
		const auto outer = Window::AdjustSize(size, static_cast<DWORD>(GetWindowLongPtrW(ownedWindow, GWL_STYLE)));
		ASSERT_TRUE(SetWindowPos(ownedWindow, nullptr, 0, 0, outer.Width, outer.Height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
		RECT client{}; ASSERT_TRUE(GetClientRect(ownedWindow, &client));
		EXPECT_EQ(size.Width, static_cast<unsigned int>(client.right - client.left));
		EXPECT_EQ(size.Height, static_cast<unsigned int>(client.bottom - client.top));
		EXPECT_EQ(size.Width, window.GetSize().Width); EXPECT_EQ(size.Height, window.GetSize().Height);
		EXPECT_EQ(size.Width, scene->GetSize().Width); EXPECT_EQ(size.Height, scene->GetSize().Height);
	};
	nativeResize({ 480, 320 }); EXPECT_FLOAT_EQ(progress, settings->TransitionValue());
	capture("production-window-small");
	EXPECT_GE(settings->TransitionValue(), progress);
	nativeResize({ 1000, 650 }); capture("production-window-restored");
	EXPECT_EQ(1000u, scene->GetSize().Width); EXPECT_EQ(650u, scene->GetSize().Height);
	const auto restoredClient = window.GetSize();
	ShowWindow(ownedWindow, SW_MAXIMIZE);
	ASSERT_TRUE(IsZoomed(ownedWindow)); EXPECT_TRUE(desktop.IsSeparateFromInput());
	EXPECT_EQ(Window::MAXIMISED, window.GetConfig().State);
	RECT maximizedClient{}; ASSERT_TRUE(GetClientRect(ownedWindow, &maximizedClient));
	EXPECT_EQ(static_cast<unsigned int>(maximizedClient.right), window.GetSize().Width);
	EXPECT_EQ(static_cast<unsigned int>(maximizedClient.right), scene->GetSize().Width);
	EXPECT_EQ(static_cast<unsigned int>(maximizedClient.bottom), window.GetSize().Height);
	EXPECT_EQ(static_cast<unsigned int>(maximizedClient.bottom), scene->GetSize().Height);
	capture("production-window-os-maximized");
	EXPECT_EQ(0, settings->TryGetChild(1)->GlobalPosition().Y);
	const auto topHandle = NativeGuiRenderEvidence::Settings(*scene, { 30, static_cast<int>(scene->GetSize().Height) - 10 });
	ASSERT_TRUE(topHandle);
	EXPECT_EQ(static_cast<int>(scene->GetSize().Height) - 28, topHandle->TryGetChild(1)->GlobalPosition().Y);
	ShowWindow(ownedWindow, SW_RESTORE);
	EXPECT_FALSE(IsZoomed(ownedWindow)); EXPECT_EQ(Window::WINDOWED, window.GetConfig().State);
	EXPECT_EQ(restoredClient.Width, window.GetSize().Width); EXPECT_EQ(restoredClient.Height, scene->GetSize().Height);
	EXPECT_EQ(restoredClient.Width, scene->GetSize().Width); EXPECT_EQ(restoredClient.Height, window.GetSize().Height);
	capture("production-window-os-maximize-restored");
	ShowWindow(ownedWindow, SW_MINIMIZE);
	EXPECT_TRUE(IsIconic(ownedWindow)); EXPECT_EQ(Window::MINIMISED, window.GetConfig().State);
	EXPECT_EQ(0u, window.GetSize().Width); EXPECT_EQ(0u, scene->GetSize().Height);
	EXPECT_EQ(0u, scene->GetSize().Width); EXPECT_EQ(0u, window.GetSize().Height);
	window.Render(); window.Swap(); EXPECT_EQ(GL_NO_ERROR, glGetError());
	ShowWindow(ownedWindow, SW_RESTORE);
	EXPECT_FALSE(IsIconic(ownedWindow)); EXPECT_EQ(Window::WINDOWED, window.GetConfig().State);
	EXPECT_EQ(restoredClient.Width, window.GetSize().Width); EXPECT_EQ(restoredClient.Height, scene->GetSize().Height);
	EXPECT_EQ(restoredClient.Width, scene->GetSize().Width); EXPECT_EQ(restoredClient.Height, window.GetSize().Height);
	capture("production-window-os-minimize-restored");
	ShowWindow(ownedWindow, SW_HIDE); EXPECT_FALSE(IsWindowVisible(ownedWindow)); EXPECT_TRUE(desktop.IsSeparateFromInput());
	// Dispatch native empty-size notifications. Windows enforces its outer-window
	// minimum on SetWindowPos; notifications still exercise the production handler
	// for empty/minimized clients without changing that minimum in this test.
	GLint oldFramebuffer = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
	glReadBuffer(GL_BACK); glClearColor(0.125f, 0.25f, 0.5f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
	std::array<unsigned char, 4> undrawnPixel{};
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, undrawnPixel.data());
	for (const auto empty : { utils::Size2d{ 0, 0 }, utils::Size2d{ 0, 180 }, utils::Size2d{ 320, 0 } }) {
		SendMessageW(ownedWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(empty.Width, empty.Height));
		EXPECT_EQ(empty.Width, window.GetSize().Width); EXPECT_EQ(empty.Height, scene->GetSize().Height);
		window.Render(); window.Swap();
		GLint framebuffer = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		EXPECT_EQ(oldFramebuffer, framebuffer); EXPECT_EQ(GL_NO_ERROR, glGetError());
		std::array<unsigned char, 4> pixel{}; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
		EXPECT_EQ(undrawnPixel, pixel); // Empty frames must not draw the old scene.
		EXPECT_FALSE(settings->RouteHitTest({ 30, 10 }));
	}
	window.SetWindowState(Window::FULLSCREEN);
	SendMessageW(ownedWindow, WM_SIZE, SIZE_MINIMIZED, 0);
	EXPECT_TRUE(window.IsFullscreen()); EXPECT_EQ(0u, window.GetSize().Width); EXPECT_EQ(0u, scene->GetSize().Height);
	window.Render(); window.Swap(); EXPECT_EQ(GL_NO_ERROR, glGetError());
	SendMessageW(ownedWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1000, 650));
	EXPECT_TRUE(window.IsFullscreen()); EXPECT_EQ(1000u, scene->GetSize().Width); EXPECT_EQ(650u, window.GetSize().Height);
	window.SetWindowState(Window::WINDOWED);
	nativeResize({ 1000, 650 }); capture("production-window-zero-restored");
	actions::TouchAction press; press.Touch = actions::TouchAction::TOUCH_MOUSE;
	press.State = actions::TouchAction::TOUCH_DOWN; press.Index = 0; press.Position = { 300, 100 };
	const auto result = window.OnAction(press);
	EXPECT_TRUE(result.IsEaten); EXPECT_FALSE(scene->HasSelection()); window.CancelMouseCapture();
	auto frame = settings->TryGetChild(0);
	auto pageScroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(frame->TryGetChild(1));
	ASSERT_TRUE(pageScroll);
	auto radio = std::dynamic_pointer_cast<gui::GuiRadio>(pageScroll->Content()->TryGetChild(1));
	ASSERT_TRUE(radio);
	for (const auto index : { 2u, 0u }) {
		auto toggle = radio->TryGetChild(static_cast<unsigned char>(index)); ASSERT_TRUE(toggle);
		const auto pos = toggle->GlobalPosition(); const auto size = toggle->GetSize();
		press.Position = { pos.X + static_cast<int>(size.Width / 2), pos.Y + static_cast<int>(size.Height / 2) };
		press.State = actions::TouchAction::TOUCH_DOWN; EXPECT_TRUE(window.OnAction(press).IsEaten);
		press.State = actions::TouchAction::TOUCH_UP; EXPECT_TRUE(window.OnAction(press).IsEaten);
		EXPECT_EQ(index == 2u, take->ResolvedMidiQuantisation().Enabled);
		EXPECT_FALSE(scene->HasSelection());
	}
	// Seed the pointer once while collapsed, then let actual UI frames move a
	// retained control under it. No further move event may be needed for hover.
	const auto settleSettings = [&](bool expanded) {
		settings->SetExpanded(expanded);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (settings->TransitionValue() != (expanded ? 1.0f : 0.0f) && std::chrono::steady_clock::now() < deadline) {
			window.Render(); glFinish(); window.Swap(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		EXPECT_FLOAT_EQ(expanded ? 1.0f : 0.0f, settings->TransitionValue());
	};
	settleSettings(true);
	const auto allToggle = radio->TryGetChild(2); ASSERT_TRUE(allToggle);
	const auto hoverPosition = allToggle->GlobalPosition(); const auto hoverSize = allToggle->GetSize();
	actions::TouchMoveAction stationary; stationary.Touch = actions::TouchAction::TOUCH_MOUSE;
	stationary.MouseButtonsDown = 0;
	stationary.Position = { hoverPosition.X + static_cast<int>(hoverSize.Width / 2),
		hoverPosition.Y + static_cast<int>(hoverSize.Height / 2) };
	settleSettings(false); window.OnAction(stationary); window.Render();
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, allToggle->GetState());
	settings->SetExpanded(true);
	bool sawIntermediate = false, sawHoverDuringMotion = false;
	const auto hoverDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (settings->TransitionValue() < 1.0f && std::chrono::steady_clock::now() < hoverDeadline) {
		window.Render(); glFinish(); window.Swap();
		const bool hit = allToggle->RouteHitTest(allToggle->GlobalToLocal(stationary.Position));
		EXPECT_EQ(hit ? base::GuiElement::STATE_OVER : base::GuiElement::STATE_NORMAL, allToggle->GetState())
			<< "Panel transition " << settings->TransitionValue();
		if (settings->TransitionValue() > 0.0f && settings->TransitionValue() < 1.0f) {
			sawIntermediate = true; sawHoverDuringMotion |= allToggle->GetState() == base::GuiElement::STATE_OVER;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_TRUE(sawIntermediate); EXPECT_TRUE(sawHoverDuringMotion);
	EXPECT_FLOAT_EQ(1.0f, settings->TransitionValue()); EXPECT_EQ(base::GuiElement::STATE_OVER, allToggle->GetState());
	capture("production-window-stationary-hover");
	// Exercise the compact client through the actual native resize and Scene
	// input path. Collapse selection so its body does not cover settings tabs.
	auto selection = NativeGuiRenderEvidence::Settings(*scene, { 30, 640 }); ASSERT_TRUE(selection);
	selection->SetExpanded(false);
	const auto selectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (selection->TransitionValue() > 0.0f && std::chrono::steady_clock::now() < selectionDeadline) {
		window.Render(); glFinish(); window.Swap(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_FLOAT_EQ(0.0f, selection->TransitionValue());
	nativeResize({ 320, 180 }); settleSettings(true);
	const auto clickControl = [&](const std::shared_ptr<base::GuiElement>& control) {
		const auto position = control->GlobalPosition(); const auto size = control->GetSize();
		press.Position = { position.X + static_cast<int>(size.Width / 2), position.Y + static_cast<int>(size.Height / 2) };
		press.State = actions::TouchAction::TOUCH_DOWN; EXPECT_TRUE(window.OnAction(press).IsEaten);
		press.State = actions::TouchAction::TOUCH_UP; EXPECT_TRUE(window.OnAction(press).IsEaten);
	};
	auto tabsViewport = std::dynamic_pointer_cast<gui::GuiScrollPanel>(frame->TryGetChild(2)); ASSERT_TRUE(tabsViewport);
	for (const auto index : { 0u, 1u }) {
		clickControl(tabsViewport->Content()->TryGetChild(static_cast<unsigned char>(index)));
		EXPECT_EQ(index == 0u ? gui::SettingsPage::Midi : gui::SettingsPage::Timing, settings->Page());
		capture(index == 0u ? "production-window-compact-midi" : "production-window-compact-timing");
	}
	const auto pagePosition = pageScroll->GlobalPosition(); const auto pageSize = pageScroll->GetSize();
	wheel.Position = { pagePosition.X + 10, pagePosition.Y + static_cast<int>(pageSize.Height / 2) };
	for (int attempt = 0; attempt < 12; ++attempt) {
		const auto position = allToggle->GlobalPosition(); const auto size = allToggle->GetSize();
		const int center = position.Y + static_cast<int>(size.Height / 2);
		if (center >= pagePosition.Y && center < pagePosition.Y + static_cast<int>(pageSize.Height)) break;
		EXPECT_TRUE(window.OnAction(wheel).IsEaten);
	}
	EXPECT_GT(pageScroll->ScrollOffset(), 0);
	for (const auto index : { 2u, 0u }) {
		auto toggle = radio->TryGetChild(static_cast<unsigned char>(index));
		const int center = toggle->GlobalPosition().Y + static_cast<int>(toggle->GetSize().Height / 2);
		ASSERT_GE(center, pagePosition.Y); ASSERT_LT(center, pagePosition.Y + static_cast<int>(pageSize.Height));
		clickControl(toggle); EXPECT_EQ(index == 2u, take->ResolvedMidiQuantisation().Enabled);
	}
	capture("production-window-compact-quantisation");
	clickControl(settings->TryGetChild(1)); settleSettings(false);
	capture("production-window-compact-hud");
	// The OS outer minimum is a smaller degraded profile: retain reachable
	// handles and a valid render, without claiming every label is readable.
	const auto minimum = window.GetMinSize();
	ASSERT_TRUE(SetWindowPos(ownedWindow, nullptr, 0, 0, minimum.Width, minimum.Height,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
	RECT minimumClient{}; ASSERT_TRUE(GetClientRect(ownedWindow, &minimumClient));
	EXPECT_EQ(static_cast<unsigned int>(minimumClient.right), scene->GetSize().Width);
	EXPECT_EQ(static_cast<unsigned int>(minimumClient.bottom), scene->GetSize().Height);
	capture("production-window-os-minimum");
	clickControl(settings->TryGetChild(1)); settleSettings(true);
	clickControl(settings->TryGetChild(1)); settleSettings(false);
	// Supported compact profile: use the handles to expose settings or HUD,
	// and scroll complete control frames into view before operating them.
	nativeResize({ 400, 240 }); settleSettings(true);
	const auto revealControl = [&](const std::shared_ptr<base::GuiElement>& control) {
		const auto viewport = pageScroll->GlobalPosition(); const int height = static_cast<int>(pageScroll->GetSize().Height);
		for (int attempt = 0; attempt < 20; ++attempt) {
			const int bottom = control->GlobalPosition().Y, top = bottom + static_cast<int>(control->GetSize().Height);
			if (bottom >= viewport.Y && top <= viewport.Y + height) return;
			wheel.Position = { viewport.X + 10, viewport.Y + height / 2 }; wheel.Value = bottom < viewport.Y ? -1 : 1;
			EXPECT_TRUE(window.OnAction(wheel).IsEaten);
		}
		ADD_FAILURE() << "Control did not become fully visible in compact page";
	};
	const auto dragNumeric = [&](const std::shared_ptr<gui::GuiNumericInput>& input, int delta) {
		revealControl(input); clickControl(input);
		press.State = actions::TouchAction::TOUCH_DOWN; EXPECT_TRUE(window.OnAction(press).IsEaten);
		actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE; move.MouseButtonsDown = 1;
		move.Position = press.Position + utils::Position2d{ 0, delta }; EXPECT_TRUE(window.OnAction(move).IsEaten);
		press.Position = move.Position; press.State = actions::TouchAction::TOUCH_UP; EXPECT_TRUE(window.OnAction(press).IsEaten);
	};
	clickControl(tabsViewport->Content()->TryGetChild(0)); ASSERT_EQ(gui::SettingsPage::Midi, settings->Page());
	auto channel = std::dynamic_pointer_cast<gui::GuiNumericInput>(pageScroll->Content()->TryGetChild(1)); ASSERT_TRUE(channel);
	const auto originalChannel = channel->Value(); dragNumeric(channel, 40); EXPECT_DOUBLE_EQ(originalChannel + 4.0, channel->Value());
	capture("production-window-supported-midi"); dragNumeric(channel, -40); EXPECT_DOUBLE_EQ(originalChannel, channel->Value());
	clickControl(tabsViewport->Content()->TryGetChild(1)); ASSERT_EQ(gui::SettingsPage::Timing, settings->Page());
	revealControl(radio);
	ASSERT_FALSE(take->MidiQuantisation().Enabled);
	for (const auto index : { 0u, 1u, 2u }) {
		clickControl(radio->TryGetChild(static_cast<unsigned char>(index)));
		EXPECT_EQ(index, radio->CurrentValue());
		EXPECT_EQ(index == 2u, take->ResolvedMidiQuantisation().Enabled); // This fixture's local grid is off.
	}
	capture("production-window-supported-quantisation");
	auto phase = std::dynamic_pointer_cast<gui::GuiNumericInput>(pageScroll->Content()->TryGetChild(3)); ASSERT_TRUE(phase);
	const auto originalPhase = phase->Value(); dragNumeric(phase, 10);
	EXPECT_NEAR(originalPhase + 0.05, phase->Value(), 0.000001);
	EXPECT_NEAR(phase->Value(), scene->SnapshotStations().front()->TransportOffsetLoopFrac(), 0.000001);
	capture("production-window-supported-phase"); dragNumeric(phase, -10);
	EXPECT_NEAR(originalPhase, scene->SnapshotStations().front()->TransportOffsetLoopFrac(), 0.000001);
	auto click = std::dynamic_pointer_cast<gui::GuiToggle>(pageScroll->Content()->TryGetChild(4)); ASSERT_TRUE(click);
	revealControl(click); const auto originalClick = click->GetToggleState(); clickControl(click);
	EXPECT_NE(originalClick, click->GetToggleState()); capture("production-window-supported-click");
	clickControl(click); EXPECT_EQ(originalClick, click->GetToggleState());
	clickControl(settings->TryGetChild(1)); settleSettings(false);
	(*triggerScroll)->SetScrollOffset(0); capture("production-window-supported-hud");
	EXPECT_GE((*triggerScroll)->GetSize().Height, 100u);
	const auto firstCard = (*triggerScroll)->Content()->TryGetChild(0); ASSERT_TRUE(firstCard);
	EXPECT_EQ(100u, firstCard->GetSize().Height);
	EXPECT_GE(firstCard->GlobalPosition().Y, (*triggerScroll)->GlobalPosition().Y);
	EXPECT_LE(firstCard->GlobalPosition().Y + 100, (*triggerScroll)->GlobalPosition().Y + static_cast<int>((*triggerScroll)->GetSize().Height));
	clickControl(selection->TryGetChild(1));
	const auto compactSelectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (selection->TransitionValue() < 1.0f && std::chrono::steady_clock::now() < compactSelectionDeadline) {
		window.Render(); glFinish(); window.Swap(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_FLOAT_EQ(1.0f, selection->TransitionValue());
	auto selectionViewport = std::dynamic_pointer_cast<gui::GuiScrollPanel>(selection->TryGetChild(0)->TryGetChild(1));
	ASSERT_TRUE(selectionViewport);
	auto depth = std::dynamic_pointer_cast<gui::GuiRadio>(selectionViewport->Content()->TryGetChild(0)); ASSERT_TRUE(depth);
	const auto selectionPosition = selectionViewport->GlobalPosition(); const auto selectionSize = selectionViewport->GetSize();
	for (const auto index : { 2u, 1u, 0u }) {
		auto toggle = depth->TryGetChild(static_cast<unsigned char>(index)); ASSERT_TRUE(toggle);
		const auto position = toggle->GlobalPosition(); const auto size = toggle->GetSize();
		ASSERT_GE(position.X, selectionPosition.X); ASSERT_GE(position.Y, selectionPosition.Y);
		ASSERT_LE(position.X + static_cast<int>(size.Width), selectionPosition.X + static_cast<int>(selectionSize.Width));
		ASSERT_LE(position.Y + static_cast<int>(size.Height), selectionPosition.Y + static_cast<int>(selectionSize.Height));
		clickControl(toggle); EXPECT_EQ(index, depth->CurrentValue()); EXPECT_FALSE(scene->HasSelection());
	}
	capture("production-window-supported-selection");
	wheel.Value = -1;
	nativeResize({ 1000, 650 }); pageScroll->SetScrollOffset(0); settleSettings(true);
	selection->SetExpanded(true); selection->AdvanceAnimation(0.22f);
	take->Select();
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	const auto midiLoop = take->GetMidiLoops().front();
	ASSERT_TRUE(scene->OpenLoopGridEditor(take, {}, midiLoop));
	const auto openDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!scene->LoopGridEditorReady() && std::chrono::steady_clock::now() < openDeadline) {
		window.Render(); glFinish(); window.Swap();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_TRUE(scene->LoopGridEditorReady());
	midi::MidiLoop::EditState before; ASSERT_TRUE(midiLoop->SnapshotForEdit(before));
	ASSERT_EQ(2u, before.EventCount);
	ASSERT_TRUE(midiLoop->Model());
	const auto bottomPitch = midiLoop->Model()->EditorBottomPitch();
	const auto visibleRows = midiLoop->Model()->EditorVisibleRows();
	capture("production-window-midi-editor");
	press.Position = { 300, 100 };
	press.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_SHIFT | base::Action::MODIFIER_CTRL);
	press.State = actions::TouchAction::TOUCH_DOWN; EXPECT_TRUE(window.OnAction(press).IsEaten);
	press.State = actions::TouchAction::TOUCH_UP; EXPECT_TRUE(window.OnAction(press).IsEaten);
	wheel.Position = press.Position; wheel.Modifiers = press.Modifiers;
	EXPECT_TRUE(window.OnAction(wheel).IsEaten);
	// The engaged editor uses a separate HUD input path. Scroll there, then
	// click real relocated controls with modifiers rather than just the backdrop.
	(*triggerScroll)->SetScrollOffset(0);
	wheel.Position = { scrollPosition.X + 60, scrollPosition.Y + 100 };
	EXPECT_TRUE(window.OnAction(wheel).IsEaten);
	EXPECT_GT((*triggerScroll)->ScrollOffset(), 0);
	for (const auto index : { 2u, 0u }) {
		const auto toggle = radio->TryGetChild(static_cast<unsigned char>(index));
		const auto pos = toggle->GlobalPosition(); const auto size = toggle->GetSize();
		press.Position = { pos.X + static_cast<int>(size.Width / 2), pos.Y + static_cast<int>(size.Height / 2) };
		press.State = actions::TouchAction::TOUCH_DOWN; EXPECT_TRUE(window.OnAction(press).IsEaten);
		press.State = actions::TouchAction::TOUCH_UP; EXPECT_TRUE(window.OnAction(press).IsEaten);
		EXPECT_EQ(index == 2u, take->ResolvedMidiQuantisation().Enabled);
	}
	EXPECT_TRUE(scene->IsLoopGridEditorOpen()); EXPECT_EQ(midiLoop, scene->LoopGridEditorMidiLoop());
	EXPECT_EQ(bottomPitch, midiLoop->Model()->EditorBottomPitch());
	EXPECT_EQ(visibleRows, midiLoop->Model()->EditorVisibleRows());
	midi::MidiLoop::EditState after; ASSERT_TRUE(midiLoop->SnapshotForEdit(after));
	EXPECT_EQ(before.Revision, after.Revision); EXPECT_EQ(before.EventCount, after.EventCount);
	EXPECT_EQ(before.Events[0].sampleOffset, after.Events[0].sampleOffset);
	EXPECT_EQ(before.Events[1].sampleOffset, after.Events[1].sampleOffset);
	scene->CloseLoopGridEditor();
	const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (scene->LoopGridEditorMorph() > 0.0f && std::chrono::steady_clock::now() < closeDeadline) {
		window.Render(); glFinish(); window.Swap();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_FLOAT_EQ(0.0f, scene->LoopGridEditorMorph());
	EXPECT_FALSE(scene->IsLoopGridEditorOpen()); EXPECT_EQ(nullptr, scene->LoopGridEditorMidiLoop());
	capture("production-window-editor-restored");
	reveal.KeyActionType = actions::KeyAction::KEY_UP; window.OnAction(reveal);
}

TEST(GuiRenderEvidence, ProductionWindowSceneResizeAndPanelInput)
{
	if (!std::getenv("JAMMA_RENDER_EVIDENCE_DIR") || !std::getenv("JAMMA_WINDOW_RENDER_EVIDENCE"))
		GTEST_SKIP() << "Set both render-evidence variables for the isolated production-window pass.";
	// The test runner's main thread may already own windows/hooks. A fresh UI
	// thread can attach before creating any HWND, then owns the whole GL lifetime.
	std::thread worker(&ProductionWindowGuiEvidence::Run);
	worker.join();
}
