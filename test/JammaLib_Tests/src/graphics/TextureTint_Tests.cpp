#define GLEW_STATIC
#include "gtest/gtest.h"
#include "graphics/Image.h"
#include "graphics/GlDeleteQueue.h"
#include "gui/GuiButton.h"
#include "gui/GuiToggle.h"
#include "gui/GuiPanelHeader.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <thread>

class TextureTintGlTest : public testing::Test
{
protected:
	HWND _window = nullptr;
	HDC _dc = nullptr;
	HGLRC _context = nullptr;

	void SetUp() override
	{
		_window = CreateWindowA("STATIC", "Tint shader test", WS_POPUP, 0, 0, 64, 64,
			nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
		ASSERT_NE(_window, nullptr);
		_dc = GetDC(_window);
		PIXELFORMATDESCRIPTOR format{};
		format.nSize = sizeof(format); format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32;
		const auto pixelFormat = ChoosePixelFormat(_dc, &format);
		ASSERT_NE(pixelFormat, 0);
		ASSERT_TRUE(SetPixelFormat(_dc, pixelFormat, &format));
		_context = wglCreateContext(_dc);
		ASSERT_NE(_context, nullptr);
		ASSERT_TRUE(wglMakeCurrent(_dc, _context));
		if (glewInit() != GLEW_OK || !GLEW_VERSION_3_3)
			GTEST_SKIP() << "OpenGL 3.3 is required for tint integration";
		while (glGetError() != GL_NO_ERROR) {}
		graphics::GlDeleteQueue::SetRenderThread(std::this_thread::get_id());
	}

	void TearDown() override
	{
		graphics::GlDeleteQueue::FlushPendingDeletes();
		graphics::GlDeleteQueue::SetRenderThread({});
		wglMakeCurrent(nullptr, nullptr);
		if (_context) wglDeleteContext(_context);
		if (_dc) ReleaseDC(_window, _dc);
		if (_window) DestroyWindow(_window);
	}

	static bool LoadShader(resources::ResourceLib& resources)
	{
		return resources.LoadResource(resources::SHADER, "texture_tinted",
			{ "MVP", "TextureSampler", "TintColor", "Opacity", "TintMaskSampler", "HasTintMask" });
	}

	static graphics::Image MakeImage(const std::string& name, unsigned int size)
	{
		return graphics::Image(graphics::ImageParams(base::DrawableParams{ name },
			base::SizeableParams{ size, size }, "texture_tinted", false, false, false));
	}

	static void Fill(resources::ResourceLib& resources, const std::string& name,
		std::array<unsigned char, 4> rgba)
	{
		const auto resource = resources.GetResource(name);
		ASSERT_TRUE(resource.has_value());
		const auto texture = std::dynamic_pointer_cast<resources::TextureResource>(resource->lock());
		ASSERT_NE(texture, nullptr);
		std::vector<unsigned char> pixels(texture->Width() * texture->Height() * 4u);
		for (std::size_t i = 0; i < pixels.size(); i += 4u)
			std::copy(rgba.begin(), rgba.end(), pixels.begin() + i);
		glBindTexture(GL_TEXTURE_2D, texture->GetId());
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->Width(), texture->Height(), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	static std::array<unsigned char, 4> Draw(graphics::Image& image,
		graphics::GlDrawContext& context, int x = 30, int y = 30)
	{
		glClear(GL_COLOR_BUFFER_BIT);
		image.Draw(context);
		std::array<unsigned char, 4> pixel{};
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
		return pixel;
	}
};

TEST_F(TextureTintGlTest, PreservesLegacyColoursAndSupportsPartialAndFullTintRelease)
{
	resources::ResourceLib resources;
	ASSERT_TRUE(LoadShader(resources));
	for (const auto* name : { "rounded_but_on", "rounded_but_on_over", "rounded_but_on_over_tintmask" })
		ASSERT_TRUE(resources.LoadResource(resources::TEXTURE, name, {}));
	Fill(resources, "rounded_but_on", { 40, 120, 240, 128 });
	Fill(resources, "rounded_but_on_over", { 40, 120, 240, 128 });
	auto normal = MakeImage("rounded_but_on", 60);
	auto hover = MakeImage("rounded_but_on_over", 60);
	normal.InitResources(resources, false);
	hover.InitResources(resources, false);
	graphics::GlDrawContext context({ 60u, 60u }, base::DrawContext::TEXTURE);
	context.Initialise(); context.Bind();
	context.PushMvp(glm::ortho(0.0f, 60.0f, 0.0f, 60.0f, -1.0f, 1.0f));
	context.SetUniform("TextureSampler", 0);
	context.SetUniform("TintColor", glm::vec3(0.8f, 0.5f, 0.25f));
	glDisable(GL_BLEND);
	auto opacity = context.WithOpacity(0.5f);
	for (const auto release : { 0u, 128u, 255u })
	{
		Fill(resources, "rounded_but_on_over_tintmask", { static_cast<unsigned char>(release), 0, 0, 255 });
		const auto pixel = Draw(hover, context);
		const float fraction = release / 255.0f;
		EXPECT_NEAR(pixel[0], 40.0f * (0.8f + 0.2f * fraction), 1.0f);
		EXPECT_NEAR(pixel[1], 120.0f * (0.5f + 0.5f * fraction), 1.0f);
		EXPECT_NEAR(pixel[2], 240.0f * (0.25f + 0.75f * fraction), 1.0f);
		EXPECT_NEAR(pixel[3], 64, 1);
		const auto legacy = Draw(normal, context);
		EXPECT_EQ(legacy, (std::array<unsigned char, 4>{ 32, 60, 60, 64 }));
	}
	EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(TextureTintGlTest, DarkFontColourPreservesCoverageAndNextDrawDefaultsToWhite)
{
	resources::ResourceLib resources;
	ASSERT_TRUE(resources.LoadResource(resources::SHADER, "font", {"MVP","TextureSampler","Opacity","TextColor"}));
	ASSERT_TRUE(resources.LoadFonts());
	const auto selected = resources.GetFont(graphics::FontOptions::FONT_SMALL);
	ASSERT_TRUE(selected);
	const auto font = selected->lock(); ASSERT_TRUE(font);
	GLuint buffers[2]{};
	const auto vertices = font->InitVertexArray("Learn", GL_STATIC_DRAW, &buffers[0], &buffers[1]);
	graphics::GlDrawContext context({64u,64u}, base::DrawContext::TEXTURE);
	context.Initialise(); context.Bind();
	context.PushMvp(glm::ortho(0.0f,64.0f,0.0f,64.0f,-1.0f,1.0f));
	context.PushMvp(glm::translate(glm::mat4(1.0f),glm::vec3{4.0f,24.0f,0.0f}));
	glDisable(GL_BLEND);
	glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
	font->Draw(context,vertices,5u,{0.08f,0.08f,0.07f});
	std::array<unsigned char,64u * 64u * 4u> dark{}, white{};
	glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,dark.data());
	glClear(GL_COLOR_BUFFER_BIT);
	font->Draw(context,vertices,5u);
	glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,white.data());
	std::size_t strongest = 0u;
	for (std::size_t i = 0u; i < dark.size(); i += 4u)
	{
		EXPECT_EQ(dark[i + 3u],white[i + 3u]);
		if (dark[i + 3u] > dark[strongest + 3u]) strongest = i;
	}
	EXPECT_GT(dark[strongest + 3u],128);
	EXPECT_NEAR(dark[strongest],20,1);
	EXPECT_NEAR(dark[strongest + 2u],18,1);
	EXPECT_EQ(white[strongest],255);
	EXPECT_EQ(white[strongest + 1u],255);
	EXPECT_EQ(white[strongest + 2u],255);
	graphics::GlDeleteQueue::DeleteBuffers(2,buffers);
	graphics::GlDeleteQueue::DeleteVertexArrays(1,&vertices);
	EXPECT_EQ(glGetError(),GL_NO_ERROR);
}

TEST_F(TextureTintGlTest, ButtonAndToggleLabelsUseConfiguredNormalHoverAndPressedColours)
{
	resources::ResourceLib resources;
	ASSERT_TRUE(resources.LoadResource(resources::SHADER,"font",{"MVP","TextureSampler","Opacity","TextColor"}));
	ASSERT_TRUE(resources.LoadFonts());
	gui::GuiButtonParams params;
	params.Size = {60u,60u}; params.Text = "Learn";
	params.TextColor = {0.8f,0.9f,1.0f};
	params.OverTextColor = glm::vec3{0.08f,0.08f,0.07f};
	params.DownTextColor = glm::vec3{0.2f,0.2f,0.1f};
	gui::GuiToggleParams toggleParams;
	static_cast<gui::GuiButtonParams&>(toggleParams) = params;
	const std::array<std::shared_ptr<gui::GuiButton>,2> buttons{
		std::make_shared<gui::GuiButton>(params),std::make_shared<gui::GuiToggle>(toggleParams)};
	graphics::GlDrawContext context({64u,64u},base::DrawContext::TEXTURE);
	context.Initialise(); context.Bind();
	context.PushMvp(glm::ortho(0.0f,64.0f,0.0f,64.0f,-1.0f,1.0f));
	for (const auto& button : buttons)
	{
		button->InitResources(resources,false);
		button->Draw(context);
		EXPECT_EQ(std::any_cast<glm::vec3>(*context.GetUniform("TextColor")),params.TextColor);
		button->ApplyHoverPoint({30,30}); button->Draw(context);
		EXPECT_EQ(std::any_cast<glm::vec3>(*context.GetUniform("TextColor")),*params.OverTextColor);
		actions::TouchAction press; press.Position = {30,30}; press.State = actions::TouchAction::TOUCH_DOWN;
		button->OnAction(press); button->Draw(context);
		EXPECT_EQ(std::any_cast<glm::vec3>(*context.GetUniform("TextColor")),*params.DownTextColor);
		button->SetEnabled(false); button->Draw(context);
		EXPECT_EQ(std::any_cast<glm::vec3>(*context.GetUniform("TextColor")),params.TextColor);
	}
	EXPECT_EQ(glGetError(),GL_NO_ERROR);
}

TEST_F(TextureTintGlTest, ToggleSharedDrawPathKeepsOnOffTexturesAndMissingStateFallbacks)
{
	resources::ResourceLib resources;
	ASSERT_TRUE(LoadShader(resources));
	const std::array<const char*,5> textures{"rounded_but","rounded_but_over","rounded_but_on","rounded_but_on_over","rounded_but_on_down"};
	for (std::size_t i = 0; i < textures.size(); ++i)
	{
		ASSERT_TRUE(resources.LoadResource(resources::TEXTURE,textures[i],{}));
		const auto shade = static_cast<unsigned char>(30u + 40u * i);
		Fill(resources,textures[i],{shade,shade,shade,255});
	}
	gui::GuiToggleParams params;
	params.Size = {60u,60u}; params.TextureShader = "texture_tinted"; params.TintColor = {1.0f,1.0f,1.0f};
	params.Texture = textures[0]; params.OverTexture = textures[1]; params.OutTexture = textures[1];
	params.ToggledTexture = textures[2]; params.ToggledOverTexture = textures[3]; params.ToggledDownTexture = textures[4];
	auto toggle = std::make_shared<gui::GuiToggle>(params);
	toggle->InitResources(resources,false);
	graphics::GlDrawContext context({64u,64u},base::DrawContext::TEXTURE);
	context.Initialise(); context.Bind();
	context.PushMvp(glm::ortho(0.0f,64.0f,0.0f,64.0f,-1.0f,1.0f));
	glDisable(GL_BLEND); context.SetUniform("TextureSampler",0);
	for (const bool on : {false,true})
	{
		toggle->SetToggleState(on ? gui::GuiToggleParams::TOGGLE_ON : gui::GuiToggleParams::TOGGLE_OFF,true);
		toggle->ClearPointerState(); toggle->Draw(context);
		std::array<unsigned char,4> pixel{};
		glReadPixels(30,30,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
		EXPECT_EQ(pixel[0],on ? 110 : 30);
		toggle->ApplyHoverPoint({30,30}); toggle->Draw(context);
		glReadPixels(30,30,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
		EXPECT_EQ(pixel[0],on ? 150 : 70);
		actions::TouchAction press; press.Position = {30,30}; press.State = actions::TouchAction::TOUCH_DOWN;
		toggle->OnAction(press); toggle->Draw(context);
		glReadPixels(30,30,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
		EXPECT_EQ(pixel[0],on ? 190 : 30);
		toggle->ApplyHoverPoint({80,80}); toggle->Draw(context);
		glReadPixels(30,30,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
		EXPECT_EQ(pixel[0],on ? 110 : 30);
	}
	EXPECT_EQ(glGetError(),GL_NO_ERROR);
}

TEST_F(TextureTintGlTest, HoverMasksBrightenBordersAndPreserveTintedFillsAndNinePatchGaps)
{
	resources::ResourceLib resources;
	ASSERT_TRUE(LoadShader(resources));
	for (const auto* stem : { "rounded_but_over", "rounded_but_on_over", "performance_group_over",
		"performance_group_on_over", "performance_group_add_over" })
	{
		const std::string name = stem;
		const bool rounded = name.starts_with("rounded");
		const auto size = rounded ? 60u : 32u;
		const auto border = rounded ? "12" : "5";
		ASSERT_TRUE(resources.LoadResource(resources::TEXTURE, name, { "ninepatch", border, border }));
		ASSERT_TRUE(resources.LoadResource(resources::TEXTURE, name + "_tintmask", {}));
		auto image = MakeImage(name, size);
		image.InitResources(resources, false);
		graphics::GlDrawContext context({ size, size }, base::DrawContext::TEXTURE);
		context.Initialise(); context.Bind();
		context.PushMvp(glm::ortho(0.0f, float(size), 0.0f, float(size), -1.0f, 1.0f));
		context.SetUniform("TextureSampler", 0);
		context.SetUniform("TintColor", glm::vec3(1.0f, 0.7f, 0.2f));
		glDisable(GL_BLEND);
		const auto edge = Draw(image, context, size / 2, rounded ? 3 : 0);
		// The add button intentionally authors a darker border (#b4b4b4).
		const float shade = name == "performance_group_add_over" ? 180.0f : 255.0f;
		EXPECT_NEAR(edge[0], shade, 1) << name;
		EXPECT_NEAR(edge[1], shade * (0.7f + 0.3f * 128.0f / 255.0f), 1) << name;
		EXPECT_NEAR(edge[2], shade * (0.2f + 0.8f * 128.0f / 255.0f), 1) << name;
		const auto centre = Draw(image, context, size / 2, size / 2);
		if (centre[3])
		{
			EXPECT_NEAR(centre[1], centre[0] * 0.7f, 1) << name;
			EXPECT_NEAR(centre[2], centre[0] * 0.2f, 1) << name;
		}
		if (rounded)
		{
			// Both hover states share the same inset, 5px frame and 2px gap.
			EXPECT_EQ(Draw(image, context, 30, 0)[3], 0) << name;
			EXPECT_EQ(Draw(image, context, 30, 3)[3], 255) << name;
			EXPECT_EQ(Draw(image, context, 30, 7)[3], 255) << name;
			EXPECT_EQ(Draw(image, context, 30, 8)[3], 0) << name;
			EXPECT_EQ(Draw(image, context, 30, 9)[3], 0) << name;
			EXPECT_EQ(centre[3], name == "rounded_but_over" ? 128 : 255) << name;
		}
	}
	EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(TextureTintGlTest, PanelHeaderFillHasNeutralTranslucencyAndOpposingChamfers)
{
    resources::ResourceLib resources;
    ASSERT_TRUE(resources.LoadResource(resources::SHADER, "colour", {"MVP", "Color", "Opacity"}));
    auto header = std::make_shared<gui::GuiPanelHeader>("Settings");
    header->Layout({120u,80u});
    header->InitResources(resources,false);
    graphics::GlDrawContext context({120u,80u},base::DrawContext::TEXTURE);
    context.Initialise(); context.Bind();
    context.PushMvp(glm::ortho(0.0f,120.0f,0.0f,80.0f,-1.0f,1.0f));
    glDisable(GL_BLEND);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    {
        auto opacity = context.WithOpacity(0.5f);
        header->Draw(context);
    }
    const auto read = [](int x,int y) {
        std::array<unsigned char,4> pixel{};
        glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        return pixel;
    };
    const auto middle = read(60,48);
    EXPECT_NEAR(31,middle[0],1); EXPECT_EQ(middle[0],middle[1]); EXPECT_EQ(middle[1],middle[2]);
    EXPECT_NEAR(115,middle[3],1); // Shared scene opacity multiplies the 90% band.
    EXPECT_EQ(0u,read(100,60)[3]); // Top right cut.
    EXPECT_EQ(0u,read(20,22)[3]); // Bottom left cut.
    EXPECT_NE(0u,read(20,60)[3]);
    EXPECT_NE(0u,read(100,36)[3]);
    EXPECT_EQ(0u,read(10,48)[3]); // Parent border remains untouched.
    EXPECT_EQ(28,header->Position().X);
    EXPECT_EQ(32,header->Position().Y);
    const auto band = header->BandBounds();
    EXPECT_EQ(band.Top - header->Position().Y - static_cast<int>(header->GetSize().Height),
        header->Position().Y - band.Bottom);
    header->ReleaseResources();
}
