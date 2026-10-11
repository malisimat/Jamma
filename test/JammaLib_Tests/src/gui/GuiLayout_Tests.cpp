#include <type_traits>

#include "gtest/gtest.h"
#include "gui/GuiGrid.h"
#include "gui/GuiStackPanel.h"
#include "gui/GuiButton.h"
#include "gui/GuiLabel.h"
#include "gui/GuiPanel.h"
#include "gui/GuiScrollPanel.h"
#include "gui/GuiMainPanel.h"
#include "gui/GuiNumericInput.h"
#include "gui/GuiHud.h"
#include "gui/GuiPopupManager.h"
#include "engine/RigSnapshot.h"
#include "engine/Scene.h"

using base::LayoutSizing;
using base::LayoutHAlign;
using base::LayoutVAlign;
using gui::GridCellDef;
using gui::GridChildPlacement;
using gui::GuiGrid;
using gui::GuiGridParams;
using gui::GuiStackPanel;
using gui::GuiStackPanelParams;
using gui::StackDirection;
using gui::GuiButton;
using gui::GuiButtonParams;
using engine::Scene;
using engine::SceneParams;
using actions::TouchAction;

// ---------------------------------------------------------------------------
// Helper factories
// ---------------------------------------------------------------------------

static GuiButtonParams MakeButtonParams(unsigned int w = 40u, unsigned int h = 20u)
{
	GuiButtonParams p;
	p.Size    = { w, h };
	p.MinSize = { 0u, 0u };
	return p;
}

static GuiGridParams MakeGrid2x2(unsigned int totalW = 200u, unsigned int totalH = 100u)
{
	GuiGridParams gp;
	gp.Size     = { totalW, totalH };
	gp.MinSize  = { 0u, 0u };
	gp.PaddingH = 0u;
	gp.PaddingV = 0u;

	GridCellDef col;
	col.sizing  = GridCellDef::Sizing::Fill;
	col.spacing = 0u;
	gp.Cols = { col, col };

	GridCellDef row;
	row.sizing  = GridCellDef::Sizing::Fill;
	row.spacing = 0u;
	gp.Rows = { row, row };

	return gp;
}

static GridChildPlacement MakePlacement(unsigned int row, unsigned int col,
                                        LayoutHAlign ha = LayoutHAlign::Fill,
                                        LayoutVAlign va = LayoutVAlign::Fill)
{
	GridChildPlacement p;
	p.row    = row;
	p.col    = col;
	p.hAlign = ha;
	p.vAlign = va;
	return p;
}

TEST(GuiPanel, AddChildUsesBaseGuiElementSignature)
{
	using ExpectedSignature = void (gui::GuiPanel::*)(std::shared_ptr<base::GuiElement>);
	EXPECT_TRUE((std::is_same_v<decltype(&gui::GuiPanel::AddChild), ExpectedSignature>));
}

TEST(GuiElement, AddChildToNonSharedParentDoesNotThrow)
{
	GuiButtonParams parentParams;
	parentParams.Size = { 64u, 32u };
	parentParams.MinSize = { 0u, 0u };

	GuiButton parent(parentParams);
	auto child = std::make_shared<GuiButton>(parentParams);

	EXPECT_NO_THROW(parent.AddChild(child));
}

// ---------------------------------------------------------------------------
// GuiGrid: fixed-size tracks
// ---------------------------------------------------------------------------

TEST(GuiGrid, FixedColumnsProducePredictableCellSizes)
{
	GuiGridParams gp;
	gp.Size     = { 300u, 100u };
	gp.MinSize  = { 0u, 0u };
	gp.PaddingH = 0u;
	gp.PaddingV = 0u;

	GridCellDef c1, c2;
	c1.sizing    = GridCellDef::Sizing::Fixed;
	c1.fixedSize = 80u;
	c1.spacing   = 4u;
	c2.sizing    = GridCellDef::Sizing::Fixed;
	c2.fixedSize = 120u;
	c2.spacing   = 0u;
	gp.Cols = { c1, c2 };

	GridCellDef r1;
	r1.sizing    = GridCellDef::Sizing::Fixed;
	r1.fixedSize = 40u;
	r1.spacing   = 0u;
	gp.Rows = { r1 };

	auto grid = std::make_shared<GuiGrid>(gp);

	// Add a child so ComputeLayout has something to position.
	auto btn = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));

	grid->ComputeLayout();

	EXPECT_EQ(80u, grid->CellSize(0u, 0u).Width);
	EXPECT_EQ(40u, grid->CellSize(0u, 0u).Height);
	EXPECT_EQ(120u, grid->CellSize(0u, 1u).Width);

	// Column origin: col 1 starts after col 0 width + spacing.
	EXPECT_EQ(0,  grid->CellOrigin(0u, 0u).X);
	EXPECT_EQ(84, grid->CellOrigin(0u, 1u).X);  // 80 + 4
}

// ---------------------------------------------------------------------------
// GuiGrid: fill tracks divide remaining space equally
// ---------------------------------------------------------------------------

TEST(GuiGrid, TwoFillColumnsShareSpaceEvenly)
{
	auto grid = std::make_shared<GuiGrid>(MakeGrid2x2(200u, 100u));

	auto btn = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));
	grid->ComputeLayout();

	// Each of 2 fill columns should get 100px.
	EXPECT_EQ(100u, grid->CellSize(0u, 0u).Width);
	EXPECT_EQ(100u, grid->CellSize(0u, 1u).Width);
	// Each of 2 fill rows should get 50px.
	EXPECT_EQ(50u, grid->CellSize(0u, 0u).Height);
	EXPECT_EQ(50u, grid->CellSize(1u, 0u).Height);
}

// ---------------------------------------------------------------------------
// GuiGrid: padding shrinks available space
// ---------------------------------------------------------------------------

TEST(GuiGrid, PaddingReducesAvailableSpaceForFillTracks)
{
	GuiGridParams gp;
	gp.Size     = { 200u, 100u };
	gp.PaddingH = 10u;   // 10 on each side → 20 total horiz padding
	gp.PaddingV = 5u;    // 5 on each side  → 10 total vert padding

	GridCellDef col;
	col.sizing = GridCellDef::Sizing::Fill;
	gp.Cols = { col };

	GridCellDef row;
	row.sizing = GridCellDef::Sizing::Fill;
	gp.Rows = { row };

	auto grid = std::make_shared<GuiGrid>(gp);
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));
	grid->ComputeLayout();

	// Single fill column gets 200 - 2*10 = 180px.
	EXPECT_EQ(180u, grid->CellSize(0u, 0u).Width);
	// Single fill row gets 100 - 2*5 = 90px.
	EXPECT_EQ(90u, grid->CellSize(0u, 0u).Height);

	// Cell origin includes padding offset.
	EXPECT_EQ(10, grid->CellOrigin(0u, 0u).X);
	EXPECT_EQ(5,  grid->CellOrigin(0u, 0u).Y);
}

// ---------------------------------------------------------------------------
// GuiGrid: min-size floor is respected
// ---------------------------------------------------------------------------

TEST(GuiGrid, FillTrackRespectsMinSizeFloor)
{
	GuiGridParams gp;
	gp.Size     = { 60u, 50u };  // very narrow

	GridCellDef col1, col2;
	col1.sizing    = GridCellDef::Sizing::Fill;
	col1.minSize   = 40u;  // each fill col wants at least 40px
	col2.sizing    = GridCellDef::Sizing::Fill;
	col2.minSize   = 40u;
	gp.Cols = { col1, col2 };

	GridCellDef row;
	row.sizing = GridCellDef::Sizing::Fill;
	gp.Rows = { row };

	auto grid = std::make_shared<GuiGrid>(gp);
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));
	grid->ComputeLayout();

	// Both fill columns must be at least 40px even though 60/2 = 30.
	EXPECT_GE(grid->CellSize(0u, 0u).Width, 40u);
	EXPECT_GE(grid->CellSize(0u, 1u).Width, 40u);
}

// ---------------------------------------------------------------------------
// GuiGrid: child placement — Fill alignment uses full cell
// ---------------------------------------------------------------------------

TEST(GuiGrid, FillAlignedChildGetsFullCellSize)
{
	auto grid = std::make_shared<GuiGrid>(MakeGrid2x2(200u, 100u));
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams(20u, 10u));

	GridChildPlacement p = MakePlacement(0u, 1u, LayoutHAlign::Fill, LayoutVAlign::Fill);
	grid->AddGridChild(btn, p);
	grid->ComputeLayout();

	// Cell (0,1) is a 100×50 fill cell; Fill alignment should set child to full cell.
	EXPECT_EQ(100u, btn->GetSize().Width);
	EXPECT_EQ(50u,  btn->GetSize().Height);
}

// ---------------------------------------------------------------------------
// GuiGrid: child placement — Center alignment positions within cell
// ---------------------------------------------------------------------------

TEST(GuiGrid, CenterAlignedChildIsPositionedInsideCell)
{
	auto grid = std::make_shared<GuiGrid>(MakeGrid2x2(200u, 100u));
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams(30u, 20u));

	GridChildPlacement p = MakePlacement(1u, 0u, LayoutHAlign::Center, LayoutVAlign::Center);
	grid->AddGridChild(btn, p);
	grid->ComputeLayout();

	// Cell (1,0): origin=(0,0), size=(100,50).
	// Child 30×20 centered → posX=0+(100-30)/2=35, posY=0+(50-20)/2=15.
	auto pos = btn->Position();
	EXPECT_EQ(35,  pos.X);
	EXPECT_EQ(15,  pos.Y);
	EXPECT_EQ(30u, btn->GetSize().Width);
	EXPECT_EQ(20u, btn->GetSize().Height);
}

// ---------------------------------------------------------------------------
// GuiGrid: auto-place sequential children
// ---------------------------------------------------------------------------

TEST(GuiGrid, AutoPlaceSequentialChildren)
{
	auto grid = std::make_shared<GuiGrid>(MakeGrid2x2(200u, 100u));

	// AddChild (not AddGridChild) → auto-place row-major
	grid->AddChild(std::make_shared<GuiButton>(MakeButtonParams()));  // (0,0)
	grid->AddChild(std::make_shared<GuiButton>(MakeButtonParams()));  // (0,1)
	grid->AddChild(std::make_shared<GuiButton>(MakeButtonParams()));  // (1,0)
	grid->AddChild(std::make_shared<GuiButton>(MakeButtonParams()));  // (1,1)

	grid->ComputeLayout();

	// Just ensure it doesn't crash and produces non-zero cell sizes.
	EXPECT_GT(grid->CellSize(0u, 0u).Width,  0u);
	EXPECT_GT(grid->CellSize(1u, 1u).Height, 0u);
}

// ---------------------------------------------------------------------------
// GuiGrid: spacing reduces available fill space
// ---------------------------------------------------------------------------

TEST(GuiGrid, ColumnSpacingReducesAvailableSpaceForFill)
{
	GuiGridParams gp;
	gp.Size     = { 200u, 50u };

	GridCellDef col;
	col.sizing  = GridCellDef::Sizing::Fill;
	col.spacing = 10u;  // 10px gap after each column (even the last, counts in usable calc)
	gp.Cols = { col, col };

	GridCellDef row;
	row.sizing = GridCellDef::Sizing::Fill;
	gp.Rows = { row };

	auto grid = std::make_shared<GuiGrid>(gp);
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));
	grid->ComputeLayout();

	// Available = 200 - 0 (padding×2) - 20 (spacing×2) = 180 → each col = 90.
	EXPECT_EQ(90u, grid->CellSize(0u, 0u).Width);
	EXPECT_EQ(90u, grid->CellSize(0u, 1u).Width);
}

// ---------------------------------------------------------------------------
// GuiGrid: InvalidateLayout causes recompute on next ComputeLayout call
// ---------------------------------------------------------------------------

TEST(GuiGrid, InvalidateLayoutTriggersRecompute)
{
	auto grid = std::make_shared<GuiGrid>(MakeGrid2x2(200u, 100u));
	auto btn  = std::make_shared<GuiButton>(MakeButtonParams());
	grid->AddGridChild(btn, MakePlacement(0u, 0u));
	grid->ComputeLayout();

	// Resize the grid and invalidate.
	grid->SetSize({ 400u, 200u });
	grid->ComputeLayout();

	// Columns should now be 200px each.
	EXPECT_EQ(200u, grid->CellSize(0u, 0u).Width);
	EXPECT_EQ(100u, grid->CellSize(0u, 0u).Height);
}

// ---------------------------------------------------------------------------
// GuiStackPanel: vertical stacking
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, VerticalStackPositionsChildrenTopToBottom)
{
	GuiStackPanelParams p;
	p.Direction = StackDirection::Vertical;
	p.Spacing   = 5u;
	p.Size      = { 100u, 200u };

	auto stack = std::make_shared<GuiStackPanel>(p);

	auto btn1 = std::make_shared<GuiButton>(MakeButtonParams(80u, 30u));
	auto btn2 = std::make_shared<GuiButton>(MakeButtonParams(80u, 40u));
	stack->AddChild(btn1);
	stack->AddChild(btn2);
	stack->ComputeLayout();

	// btn1 is anchored to the top edge, btn2 stacks below it.
	EXPECT_EQ(170, btn1->Position().Y);  // 200 - 30
	EXPECT_EQ(125, btn2->Position().Y);  // 170 - 30 - 5 - 40
}

// ---------------------------------------------------------------------------
// GuiStackPanel: horizontal stacking
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, HorizontalStackPositionsChildrenLeftToRight)
{
	GuiStackPanelParams p;
	p.Direction = StackDirection::Horizontal;
	p.Spacing   = 8u;
	p.Size      = { 300u, 50u };

	auto stack = std::make_shared<GuiStackPanel>(p);

	auto btn1 = std::make_shared<GuiButton>(MakeButtonParams(60u, 30u));
	auto btn2 = std::make_shared<GuiButton>(MakeButtonParams(70u, 30u));
	stack->AddChild(btn1);
	stack->AddChild(btn2);
	stack->ComputeLayout();

	EXPECT_EQ(0,  btn1->Position().X);
	EXPECT_EQ(68, btn2->Position().X);  // 60 + 8
}

// ---------------------------------------------------------------------------
// GuiStackPanel: Fill children stretch to remaining space
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, FillChildStretchesToRemainingWidth)
{
	GuiStackPanelParams p;
	p.Direction = StackDirection::Horizontal;
	p.Spacing   = 0u;
	p.Size      = { 200u, 40u };

	auto stack = std::make_shared<GuiStackPanel>(p);

	auto btnFixed = std::make_shared<GuiButton>(MakeButtonParams(60u, 30u));
	// btnFill has LayoutSizing::Fill on the horizontal axis.
	GuiButtonParams fillParams = MakeButtonParams(60u, 30u);
	fillParams.HorizSizing = LayoutSizing::Fill;
	auto btnFill = std::make_shared<GuiButton>(fillParams);

	stack->AddChild(btnFixed);
	stack->AddChild(btnFill);
	stack->ComputeLayout();

	// btnFill should get 200 - 60 = 140px.
	EXPECT_EQ(140u, btnFill->GetSize().Width);
}

// ---------------------------------------------------------------------------
// GuiStackPanel: padding insets child positions
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, PaddingInsetFirstChild)
{
	GuiStackPanelParams p;
	p.Direction = StackDirection::Vertical;
	p.PaddingH  = 12u;
	p.PaddingV  = 8u;
	p.Spacing   = 0u;
	p.Size      = { 100u, 100u };

	auto stack = std::make_shared<GuiStackPanel>(p);
	auto btn   = std::make_shared<GuiButton>(MakeButtonParams(60u, 20u));
	stack->AddChild(btn);
	stack->ComputeLayout();

	EXPECT_EQ(12, btn->Position().X);
	EXPECT_EQ(72, btn->Position().Y);  // 100 - 8 - 20
}

// ---------------------------------------------------------------------------
// GuiStackPanel: horizontal wrap drops items to next row when too wide
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, HorizontalWrapBreaksToNextRow)
{
	GuiStackPanelParams p;
	p.Direction   = StackDirection::Horizontal;
	p.WrapContent = true;
	p.Spacing     = 0u;
	p.Size        = { 100u, 80u };  // narrow: only fits ~1 child per row

	auto stack = std::make_shared<GuiStackPanel>(p);

	// Each button is 60px wide.  Two won't fit in 100px → second wraps.
	auto btn1 = std::make_shared<GuiButton>(MakeButtonParams(60u, 20u));
	auto btn2 = std::make_shared<GuiButton>(MakeButtonParams(60u, 20u));
	stack->AddChild(btn1);
	stack->AddChild(btn2);
	stack->ComputeLayout();

	// Row 0 is anchored to the top edge, row 1 appears below it.
	EXPECT_EQ(60, btn1->Position().Y);
	EXPECT_EQ(40, btn2->Position().Y);
}

// ---------------------------------------------------------------------------
// GuiStackPanel: three fill children in horizontal stack split evenly
// ---------------------------------------------------------------------------

TEST(GuiStackPanel, ThreeFillChildrenSplitWidthEvenly)
{
	GuiStackPanelParams p;
	p.Direction = StackDirection::Horizontal;
	p.Spacing   = 0u;
	p.Size      = { 300u, 40u };

	auto stack = std::make_shared<GuiStackPanel>(p);

	std::vector<std::shared_ptr<GuiButton>> buttons;
	for (int i = 0; i < 3; ++i)
	{
		GuiButtonParams bp = MakeButtonParams(50u, 30u);
		bp.HorizSizing = LayoutSizing::Fill;
		auto btn = std::make_shared<GuiButton>(bp);
		buttons.push_back(btn);
		stack->AddChild(btn);
	}
	stack->ComputeLayout();

	// 300px / 3 = 100px each.
	for (const auto& btn : buttons)
		EXPECT_EQ(100u, btn->GetSize().Width);
}

TEST(Scene, TouchActionReachesChildGuiPanel)
{
	SceneParams sceneParams({ "" }, {}, { 320u, 240u });
	io::UserConfig userConfig{};
	Scene scene(sceneParams, userConfig);

	GuiStackPanelParams panelParams;
	panelParams.Size = { 100u, 40u };
	panelParams.Position = { 10, 10 };
	panelParams.MinSize = { 0u, 0u };
	panelParams.GuiPassThrough = false;
	auto panel = std::make_shared<GuiStackPanel>(panelParams);
	panel->AddChild(std::make_shared<GuiButton>(MakeButtonParams(80u, 20u)));
	scene.AddChild(panel);

	TouchAction action;
	action.State = TouchAction::TOUCH_DOWN;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.Index = 0;
	action.Position = { 20, 20 };

	auto res = scene.OnAction(action);

	EXPECT_TRUE(res.IsEaten);
}

template <typename T>
static std::shared_ptr<T> FindGuiElement(base::GuiElement& root)
{
	// Scroll content is hosted separately from the ordinary child list.
	if (auto scroll = dynamic_cast<gui::GuiScrollPanel*>(&root))
	{
		if (auto content = scroll->Content())
		{
			if (auto match = std::dynamic_pointer_cast<T>(content)) return match;
			if (auto match = FindGuiElement<T>(*content)) return match;
		}
	}
	for (unsigned int index = 0; index <= 255u; ++index)
	{
		auto child = root.TryGetChild(static_cast<unsigned char>(index));
		if (!child) continue;
		if (auto match = std::dynamic_pointer_cast<T>(child)) return match;
		if (auto match = FindGuiElement<T>(*child)) return match;
	}
	return nullptr;
}

class SettingsTestScene : public Scene
{
public:
	SettingsTestScene(SceneParams params, io::UserConfig user) : Scene(std::move(params), std::move(user)) {}
	std::shared_ptr<gui::GuiMainPanel> SettingsPanel() const { return _mainPanel; }
};

static utils::Position2d SettingsHandleCenter(const std::shared_ptr<gui::GuiMainPanel>& panel)
{
	const auto handle = panel->TryGetChild(1u);
	const auto position = handle->GlobalPosition();
	const auto size = handle->GetSize();
	return position + utils::Position2d{ static_cast<int>(size.Width / 2u), static_cast<int>(size.Height / 2u) };
}

TEST(GuiStackPanel, HiddenOrDisabledParentRejectsVisibleChildren)
{
	GuiStackPanelParams params;
	params.Size = { 100u, 50u };
	auto stack = std::make_shared<GuiStackPanel>(params);
	stack->AddChild(std::make_shared<GuiButton>(MakeButtonParams()));
	stack->ComputeLayout();
	const auto point = utils::Position2d{ 10, 40 };
	ASSERT_TRUE(stack->RouteHitTest(point));
	stack->SetVisible(false);
	EXPECT_FALSE(stack->RouteHitTest(point));
	EXPECT_EQ(nullptr, stack->FindTopmostDescendant(point));
	stack->SetVisible(true);
	stack->SetEnabled(false);
	EXPECT_FALSE(stack->RouteHitTest(point));
}

TEST(GuiScrollPanel, ContentRectExcludesPaddingAndScrollBars)
{
	gui::GuiScrollPanelParams params;
	params.Size = { 100u, 60u };
	params.ScrollBarWidth = 12u;
	auto vertical = std::make_shared<gui::GuiScrollPanel>(params);
	vertical->SetContent(std::make_shared<GuiButton>(MakeButtonParams(80u, 200u)));
	const auto verticalRect = vertical->ContentRect();
	EXPECT_EQ(2, verticalRect.Left);
	EXPECT_EQ(2, verticalRect.Bottom);
	EXPECT_EQ(86, verticalRect.Right);
	EXPECT_EQ(58, verticalRect.Top);
	params.Orientation = gui::GuiScrollOrientation::Horizontal;
	auto horizontal = std::make_shared<gui::GuiScrollPanel>(params);
	horizontal->SetContent(std::make_shared<GuiButton>(MakeButtonParams(200u, 40u)));
	const auto horizontalRect = horizontal->ContentRect();
	EXPECT_EQ(2, horizontalRect.Left);
	EXPECT_EQ(14, horizontalRect.Bottom);
	EXPECT_EQ(98, horizontalRect.Right);
	EXPECT_EQ(58, horizontalRect.Top);
	EXPECT_NE(horizontal->Content().get(), horizontal->FindTopmostDescendant({ 1, 30 }).get());
	EXPECT_EQ(horizontal->Content().get(), horizontal->FindTopmostDescendant({ 10, 30 }).get());
}

TEST(GuiScrollPanel, EmptyAndTinyContentRectsStayWithinPanel)
{
	gui::GuiScrollPanelParams params;
	params.Size = { 100u, 60u };
	params.Orientation = gui::GuiScrollOrientation::Horizontal;
	auto scroll = std::make_shared<gui::GuiScrollPanel>(params);
	scroll->SetContent(std::make_shared<GuiButton>(MakeButtonParams(200u, 40u)));
	for (const auto size : { utils::Size2d{ 0u, 0u }, { 1u, 1u }, { 3u, 3u }, { 100u, 10u } })
	{
		scroll->SetSize(size);
		const auto rect = scroll->ContentRect();
		EXPECT_TRUE(rect.IsEmpty());
		EXPECT_GE(rect.Left, 0);
		EXPECT_GE(rect.Bottom, 0);
		EXPECT_LE(rect.Right, static_cast<int>(size.Width));
		EXPECT_LE(rect.Top, static_cast<int>(size.Height));
		EXPECT_FALSE(rect.Contains({ rect.Left, rect.Bottom }));
	}
}

TEST(GuiScrollPanel, EmptyViewportSkipsGraphicsWork)
{
	gui::GuiScrollPanelParams params;
	auto scroll = std::make_shared<gui::GuiScrollPanel>(params);
	base::DrawContext context({ 0u, 0u }, base::DrawContext::SCREEN);
	for (const auto size : { utils::Size2d{ 0u, 50u }, { 50u, 0u }, { 0u, 0u } })
	{
		scroll->SetSize(size);
		EXPECT_NO_THROW(scroll->Draw(context));
	}
}

TEST(GuiScrollPanel, EffectiveRectIncludesNestedScrollTransformsAndWindowClip)
{
	gui::GuiScrollPanelParams params;
	params.Size = { 100u, 60u };
	params.Position = { 10, 20 };
	params.ScrollBarWidth = 12u;
	auto outer = std::make_shared<gui::GuiScrollPanel>(params);
	base::GuiElementParams contentParams;
	contentParams.Size = { 80u, 120u };
	auto content = std::make_shared<base::GuiElement>(contentParams);
	params.Position = { 0, 60 };
	auto inner = std::make_shared<gui::GuiScrollPanel>(params);
	content->AddChild(inner);
	outer->SetContent(content);
	outer->SetScrollOffset(20);
	const auto rect = inner->EffectiveContentRect({ 0, 0, 70, 70 });
	EXPECT_EQ(12, rect.Left);
	EXPECT_EQ(42, rect.Bottom);
	EXPECT_EQ(70, rect.Right);
	EXPECT_EQ(70, rect.Top);
	outer->SetScrollOffset(60);
	EXPECT_TRUE(inner->EffectiveContentRect({ 0, 0, 70, 70 }).IsEmpty());
	outer->SetScrollOffset(0);
	outer->SetVisible(false);
	EXPECT_TRUE(inner->EffectiveContentRect({ 0, 0, 70, 70 }).IsEmpty());
}

TEST(Rect2d, DisjointIntersectionAndHalfOpenEdges)
{
	const utils::Rect2d rect{ 2, 3, 12, 13 };
	EXPECT_TRUE(rect.Contains({ 2, 3 }));
	EXPECT_FALSE(rect.Contains({ 12, 5 }));
	EXPECT_FALSE(rect.Contains({ 5, 13 }));
	EXPECT_TRUE(rect.Intersected({ 20, 20, 30, 30 }).IsEmpty());
	EXPECT_TRUE(rect.Intersected({ 0, 0, 0, 0 }).IsEmpty());
}

TEST(GuiHudLayout, StatusBarReservesTempoBeforeMessagesWithoutOverlappingColumns)
{
	for (const int viewportWidth : { 0, 80, 160, 240, 320, 500, 800, 1200, 1600 })
	{
		const int bar = gui::GuiStyle::StatusBarWidth(viewportWidth);
		const int tempo = gui::GuiStyle::TempoColumnWidth(bar);
		const int version = gui::GuiStyle::VersionColumnWidth(bar);
		const int status = gui::GuiStyle::StatusColumnWidth(bar);
		const int message = gui::GuiStyle::MessageColumnWidth(bar);
		EXPECT_GE(tempo, 0);
		EXPECT_GE(status, 0);
		EXPECT_GE(message, 0);
		EXPECT_EQ(viewportWidth, bar);
		EXPECT_EQ(bar, tempo + status + message + version);
		EXPECT_EQ(std::min(gui::GuiStyle::TempoColumnMaxWidth, bar), tempo);
		if (bar >= 320) EXPECT_EQ(160, version);
		if (bar <= gui::GuiStyle::TempoColumnMaxWidth) EXPECT_EQ(0, status + message + version);
	}
}

TEST(GuiHudLayout, AdaptiveCardWidthsUseAvailableSpaceAndRetainMinimumForScrolling)
{
	EXPECT_EQ(0u, gui::GuiHud::SourceCardWidth(500u, 0u));
	EXPECT_EQ(160u, gui::GuiHud::SourceCardWidth(500u, 1u));
	EXPECT_EQ(100u, gui::GuiHud::SourceCardWidth(210u, 2u));
	EXPECT_EQ(80u, gui::GuiHud::SourceCardWidth(169u, 2u));
	EXPECT_EQ(80u, gui::GuiHud::SourceCardWidth(0u, 100u));
}

class HudResponsiveLayoutTests : public ::testing::Test
{
protected:
	static std::vector<std::shared_ptr<gui::GuiScrollPanel>> Scrolls(const std::shared_ptr<base::GuiElement>& element)
	{
		std::vector<std::shared_ptr<gui::GuiScrollPanel>> result;
		for (unsigned int index = 0; index < 256; ++index)
		{
			const auto child = element->TryGetChild(static_cast<unsigned char>(index));
			if (!child) break;
			if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(child)) result.push_back(scroll);
			const auto nested = Scrolls(child);
			result.insert(result.end(), nested.begin(), nested.end());
		}
		return result;
	}
	static engine::RigSnapshot Routing()
	{
		engine::RigSnapshot snapshot;
		snapshot.Revision = 1u;
		for (size_t index = 0; index < 12; ++index)
		{
			io::RigFileRouting::TriggerResolution trigger;
			trigger.TriggerIndex = index;
			trigger.TriggerName = "Trigger " + std::to_string(index);
			snapshot.Graph.Triggers.push_back(trigger);
		}
		return snapshot;
	}
};

TEST_F(HudResponsiveLayoutTests, ResizeKeepsScrollViewportsInsideActualWindowAndCardsInOneRow)
{
	gui::GuiHudParams params;
	params.Size = { 1600u, 900u };
	auto hud = std::make_shared<gui::GuiHud>(params);
	hud->SetRoutingConfig(8u, { "A very long MIDI device identity with ascenders and descenders 123456789" }, Routing());
	const auto scrolls = Scrolls(hud);
	ASSERT_EQ(3u, scrolls.size());
	const auto sourceCard = scrolls[0]->Content()->TryGetChild(0);
	ASSERT_NE(nullptr, sourceCard);
	const auto largeWidth = sourceCard->GetSize().Width;
	for (const auto size : { utils::Size2d{ 500u, 300u }, { 80u, 60u }, { 1u, 1u }, { 0u, 0u }, { 1600u, 900u } })
	{
		hud->SetSize(size);
		for (const auto& scroll : scrolls)
		{
			const auto pos = scroll->GlobalPosition();
			if (scroll->IsVisible())
			{
				EXPECT_GE(pos.X, 0);
				EXPECT_GE(pos.Y, 0);
				EXPECT_LE(pos.X + static_cast<int>(scroll->GetSize().Width), static_cast<int>(size.Width));
				EXPECT_LE(pos.Y + static_cast<int>(scroll->GetSize().Height), static_cast<int>(size.Height));
			}
			EXPECT_GE(scroll->ScrollOffset(), 0);
			EXPECT_LE(scroll->ScrollOffset(), scroll->MaxScrollOffset());
		}
		for (unsigned char index = 0; index < 8; ++index)
		{
			const auto card = scrolls[0]->Content()->TryGetChild(index);
			ASSERT_NE(nullptr, card);
			EXPECT_EQ(gui::GuiStyle::ControlHeight, card->GetSize().Height);
			EXPECT_GE(card->GetSize().Width, 80u);
			EXPECT_LE(card->GetSize().Width, 160u);
			EXPECT_EQ(2, card->Position().Y);
			const auto socket = card->TryGetChild(2);
			ASSERT_NE(nullptr, socket);
			EXPECT_EQ(static_cast<int>(card->GetSize().Width / 2u), socket->Position().X + 12);
			EXPECT_LE(card->TryGetChild(0)->Position().X + static_cast<int>(card->TryGetChild(0)->GetSize().Width),
				static_cast<int>(card->GetSize().Width) - 13);
		}
		for (unsigned char index = 0; index < 12; ++index)
			EXPECT_EQ(100u, scrolls[2]->Content()->TryGetChild(index)->GetSize().Height);
	}
	EXPECT_EQ(sourceCard.get(), scrolls[0]->Content()->TryGetChild(0).get());
	EXPECT_EQ(largeWidth, sourceCard->GetSize().Width);
}

TEST_F(HudResponsiveLayoutTests, ResizeClampsExistingScrollOffsetWithoutRebuildingCards)
{
	gui::GuiHudParams params;
	params.Size = { 600u, 300u };
	auto hud = std::make_shared<gui::GuiHud>(params);
	hud->SetRoutingConfig(12u, {}, Routing());
	const auto scrolls = Scrolls(hud);
	ASSERT_EQ(2u, scrolls.size());
	const auto card = scrolls[0]->Content()->TryGetChild(0);
	for (const auto& scroll : scrolls) scroll->SetScrollOffset(scroll->MaxScrollOffset());
	hud->SetSize({ 700u, 350u });
	EXPECT_GT(scrolls[0]->ScrollOffset(), 0);
	EXPECT_LE(scrolls[0]->ScrollOffset(), scrolls[0]->MaxScrollOffset());
	hud->SetSize({ 3000u, 2000u });
	EXPECT_EQ(0, scrolls[0]->ScrollOffset());
	EXPECT_EQ(0, scrolls[1]->ScrollOffset());
	EXPECT_EQ(card.get(), scrolls[0]->Content()->TryGetChild(0).get());
}

TEST_F(HudResponsiveLayoutTests, SourceIdentityPopupUsesExistingCaptureAndStaysWithinResizedWindow)
{
	gui::GuiPopupManager popups;
	gui::GuiHudParams params;
	params.Size = { 1200u, 800u };
	params.PopupManager = &popups;
	auto hud = std::make_shared<gui::GuiHud>(params);
	hud->SetRoutingConfig(0u, { "Available device named (unavailable)" }, Routing());
	const auto scrolls = Scrolls(hud);
	ASSERT_EQ(2u, scrolls.size());
	const auto card = scrolls[0]->Content()->TryGetChild(0);
	actions::TouchAction touch;
	touch.Index = 0;
	touch.Touch = actions::TouchAction::TOUCH_MOUSE;
	touch.Position = { 5, 5 };
	touch.State = actions::TouchAction::TOUCH_DOWN;
	ASSERT_TRUE(card->OnAction(touch).IsEaten);
	card->ClearPointerState();
	touch.State = actions::TouchAction::TOUCH_UP;
	card->OnAction(touch);
	EXPECT_FALSE(popups.IsOpen());
	touch.State = actions::TouchAction::TOUCH_DOWN;
	ASSERT_TRUE(card->OnAction(touch).IsEaten);
	touch.State = actions::TouchAction::TOUCH_UP;
	card->OnAction(touch);
	ASSERT_TRUE(popups.IsOpen());
	const auto popup = popups.Top();
	hud->SetSize({ 240u, 150u });
	EXPECT_EQ(popup.get(), popups.Top().get());
	EXPECT_GE(popup->Position().X, 0);
	EXPECT_GE(popup->Position().Y, 0);
	EXPECT_LE(popup->Position().X + static_cast<int>(popup->GetSize().Width), 240);
	EXPECT_LE(popup->Position().Y + static_cast<int>(popup->GetSize().Height), 150);
	EXPECT_TRUE(popups.OnAction(touch).IsEaten);
	touch.State = actions::TouchAction::TOUCH_DOWN;
	touch.Position = { 0, 0 };
	EXPECT_TRUE(popups.OnAction(touch).IsEaten);
	EXPECT_FALSE(popups.IsOpen());
}

TEST(GuiLabel, FitsMeasuredWidthsWithEllipsisAndHandlesEmptyFrames)
{
	const auto measure = [](const std::string& text)
	{
		float width = 0.0f;
		for (char ch : text) width += ch == 'W' ? 12.0f : ch == '.' ? 2.0f : 5.0f;
		return width;
	};
	EXPECT_EQ("Wide", gui::GuiLabel::FitText("Wide", 27.0f, measure));
	EXPECT_EQ("W...", gui::GuiLabel::FitText("WWWW", 20.0f, measure));
	EXPECT_EQ("...", gui::GuiLabel::FitText("WWWW", 6.0f, measure));
	EXPECT_EQ(".", gui::GuiLabel::FitText("WWWW", 3.0f, measure));
	EXPECT_EQ("", gui::GuiLabel::FitText("WWWW", 0.0f, measure));
	EXPECT_EQ("", gui::GuiLabel::FitText("", 20.0f, measure));
}

TEST(Scene, SettingsOwnModifiedPressAndReleaseWithoutSceneFallthrough) {
	SceneParams sceneParams({ "" }, {}, { 800u, 600u });
	SettingsTestScene scene(sceneParams, io::UserConfig{});
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.Index = 0u;
	action.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_SHIFT | base::Action::MODIFIER_CTRL);
	action.Position = SettingsHandleCenter(scene.SettingsPanel());
	action.State = TouchAction::TOUCH_DOWN;
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
	action.Position = { 500, 300 };
	action.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
}

TEST(Scene, SettingsReceivesNextPressAndReleaseAfterWheelScroll) {
	SettingsTestScene scene(SceneParams({ "" }, {}, { 800u, 600u }), io::UserConfig{});
	auto panel = scene.SettingsPanel();
	gui::GuiScrollPanelParams params;
	params.Position = { 500, 100 }; params.Size = { 120u, 100u };
	auto scroll = std::make_shared<gui::GuiScrollPanel>(params);
	base::GuiElementParams contentParams; contentParams.Size = { 120u, 400u };
	scroll->SetContent(std::make_shared<base::GuiElement>(contentParams));
	scene.AddChild(scroll);
	TouchAction wheel; wheel.Touch = TouchAction::TOUCH_MOUSE;
	wheel.State = TouchAction::TOUCH_DOWN; wheel.Index = 4; wheel.Value = -1; wheel.Position = { 540, 150 };
	ASSERT_TRUE(scene.OnAction(wheel).IsEaten);
	ASSERT_GT(scroll->ScrollOffset(), 0);
	TouchAction press; press.Touch = TouchAction::TOUCH_MOUSE;
	press.State = TouchAction::TOUCH_DOWN; press.Index = 0; press.Position = SettingsHandleCenter(panel);
	const auto down = scene.OnAction(press);
	ASSERT_TRUE(down.IsEaten);
	auto owner = down.ActiveElement.lock();
	while (owner && !std::dynamic_pointer_cast<gui::GuiMainPanel>(owner)) owner = owner->Parent();
	ASSERT_TRUE(owner);
	ASSERT_EQ(panel, std::dynamic_pointer_cast<gui::GuiMainPanel>(owner));
	press.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(scene.OnAction(press).IsEaten);
	EXPECT_FALSE(std::dynamic_pointer_cast<gui::GuiMainPanel>(owner)->IsExpanded());
	EXPECT_FALSE(scene.HasSelection());
}

TEST(Scene, SettingsReceivesNextControlAfterGuiCaptureCancellation) {
	SettingsTestScene scene(SceneParams({ "" }, {}, { 800u, 600u }), io::UserConfig{});
	auto panel = scene.SettingsPanel();
	auto button = std::make_shared<GuiButton>(MakeButtonParams(80u, 30u));
	button->SetPosition({ 500, 300 }); scene.AddChild(button);
	TouchAction press; press.Touch = TouchAction::TOUCH_MOUSE;
	press.Index = 0; press.State = TouchAction::TOUCH_DOWN; press.Position = { 510, 310 };
	ASSERT_EQ(button, scene.OnAction(press).ActiveElement.lock());
	actions::TouchMoveAction cancel; cancel.Touch = TouchAction::TOUCH_MOUSE;
	cancel.Position = press.Position; cancel.MouseButtonsDown = 0;
	EXPECT_TRUE(scene.OnAction(cancel).IsEaten);
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, button->GetState());
	press.Position = SettingsHandleCenter(panel);
	const auto down = scene.OnAction(press); ASSERT_TRUE(down.IsEaten);
	auto owner = down.ActiveElement.lock();
	while (owner && !std::dynamic_pointer_cast<gui::GuiMainPanel>(owner)) owner = owner->Parent();
	ASSERT_TRUE(owner);
	ASSERT_EQ(panel, std::dynamic_pointer_cast<gui::GuiMainPanel>(owner));
	press.State = TouchAction::TOUCH_UP; EXPECT_TRUE(scene.OnAction(press).IsEaten);
	EXPECT_FALSE(panel->IsExpanded());
	EXPECT_FALSE(scene.HasSelection());
}

TEST(Scene, SettingsCaptureCancellationAllowsNextControlToReceivePress) {
	SceneParams sceneParams({ "" }, {}, { 800u, 600u });
	SettingsTestScene scene(sceneParams, io::UserConfig{});
	auto button = std::make_shared<GuiButton>(MakeButtonParams(80u, 30u));
	button->SetPosition({ 500, 300 });
	scene.AddChild(button);
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.Index = 0u;
	action.Position = SettingsHandleCenter(scene.SettingsPanel());
	action.State = TouchAction::TOUCH_DOWN;
	ASSERT_TRUE(scene.OnAction(action).IsEaten);
	actions::TouchMoveAction cancel;
	cancel.Position = action.Position;
	cancel.MouseButtonsDown = 0u;
	EXPECT_TRUE(scene.OnAction(cancel).IsEaten);
	action.Position = { 510, 310 };
	auto next = scene.OnAction(action);
	EXPECT_TRUE(next.IsEaten);
	EXPECT_EQ(button, next.ActiveElement.lock());
}

TEST(Scene, SettingsHandleCollapsesAndReopensThroughPointerDispatch) {
	SceneParams sceneParams({ "" }, {}, { 800u, 600u });
	SettingsTestScene scene(sceneParams, io::UserConfig{});
	auto panel = scene.SettingsPanel();
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.Index = 0u;
	action.Position = SettingsHandleCenter(panel);
	action.State = TouchAction::TOUCH_DOWN;
	auto down = scene.OnAction(action);
	ASSERT_TRUE(down.IsEaten);
	auto active = down.ActiveElement.lock();
	ASSERT_NE(nullptr, active);
	ASSERT_EQ(panel, active->Parent());
	action.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
	EXPECT_FALSE(panel->IsExpanded());
	for (int frame = 0; frame < 5; ++frame) panel->AdvanceAnimation(0.05f);
	EXPECT_FALSE(panel->RouteHitTest({ 30, 100 }));
	action.State = TouchAction::TOUCH_DOWN;
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
	action.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
	EXPECT_TRUE(panel->IsExpanded());
	EXPECT_EQ(gui::SettingsPage::Timing, panel->Page());
}

TEST(Scene, HidingSettingsDuringNumericCaptureConsumesTerminatingRelease) {
	SceneParams sceneParams({ "" }, {}, { 800u, 600u });
	SettingsTestScene scene(sceneParams, io::UserConfig{});
	auto panel = scene.SettingsPanel();
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE; action.Index = 0u;
	action.Position = SettingsHandleCenter(panel); action.State = TouchAction::TOUCH_DOWN;
	auto handlePress = scene.OnAction(action);
	auto active = handlePress.ActiveElement.lock();
	ASSERT_NE(nullptr, active);
	ASSERT_EQ(panel, active->Parent());
	actions::TouchMoveAction cancel;
	cancel.Position = action.Position; cancel.MouseButtonsDown = 0u;
	scene.OnAction(cancel);
	auto numeric = FindGuiElement<gui::GuiNumericInput>(*panel);
	ASSERT_NE(nullptr, numeric);
	action.Position = numeric->GlobalPosition() + utils::Position2d{ 10, 10 };
	auto inputPress = scene.OnAction(action);
	ASSERT_EQ(numeric, inputPress.ActiveElement.lock());
	numeric->SetText("0.5", false);
	panel->SetExpanded(false);
	EXPECT_FALSE(numeric->HasFocus());
	EXPECT_DOUBLE_EQ(0.5, numeric->Value());
	action.State = TouchAction::TOUCH_UP;
	action.Position = { 500, 300 };
	EXPECT_TRUE(scene.OnAction(action).IsEaten);
	EXPECT_DOUBLE_EQ(0.5, numeric->Value());
}
