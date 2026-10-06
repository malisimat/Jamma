#define GLEW_STATIC
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "graphics/Window.h"
#include "engine/Scene.h"
#include "graphics/GlDeleteQueue.h"
#include "resources/ResourcePaths.h"
#include "gui/GuiDropDown.h"
#include "gui/GuiNumericInput.h"
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
	dropdown->Close(); ordinary->ReleaseResources(); root->ReleaseResources();
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

TEST(GuiRenderEvidence, ProductionWindowSceneResizeAndPanelInput)
{
	const char* directory = std::getenv("JAMMA_RENDER_EVIDENCE_DIR");
	if (!directory || !*directory || !std::getenv("JAMMA_WINDOW_RENDER_EVIDENCE"))
		GTEST_SKIP() << "Set JAMMA_RENDER_EVIDENCE_DIR and JAMMA_WINDOW_RENDER_EVIDENCE for the isolated production-window pass.";
	const std::filesystem::path output(directory);
	std::filesystem::create_directories(output);
	{
		NativeGuiRenderEvidence preflight;
		ASSERT_TRUE(preflight.Initialize());
		ASSERT_TRUE(GLEW_VERSION_4_0);
	}
	resources::ResourceLib resources;
	io::JamFile jam{}; io::RigFile rig{};
	for (unsigned int index = 0; index < 8; ++index) {
		io::JamFile::Station station{}; station.Name = "Station " + std::to_string(index);
		jam.Stations.push_back(station);
	}
	for (unsigned int index = 0; index < 16; ++index) {
		io::RigFile::Trigger trigger{}; trigger.Name = "Agjpq trigger " + std::to_string(index);
		trigger.StationTarget = jam.Stations[index % jam.Stations.size()].Name;
		trigger.InputChannels = { index % 8 }; rig.Triggers.push_back(trigger);
	}
	const auto loaded = Scene::FromFile(SceneParams({ "" }, {}, base::SizeableParams{ { 1000, 650 }, {} }), jam, rig, L"");
	ASSERT_TRUE(loaded); auto scene = *loaded;
	auto take = scene->SnapshotStations().front()->AddTake();
	ASSERT_TRUE(take);
	Window window(*scene, resources);
	ProductionWindowEvidenceCleanup cleanup(window, *scene);
	// Window::Create can show a modal error on unsupported production formats.
	// Run this test in its own process with an external timeout.
	ASSERT_EQ(0, window.Create(GetModuleHandleW(nullptr), SW_HIDE));
	const auto ownedWindow = WindowFromDC(wglGetCurrentDC()); ASSERT_NE(nullptr, ownedWindow);
	EXPECT_FALSE(IsWindowVisible(ownedWindow));
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
}
