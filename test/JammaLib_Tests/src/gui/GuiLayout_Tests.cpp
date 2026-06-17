#include "gtest/gtest.h"
#include "gui/GuiGrid.h"
#include "gui/GuiStackPanel.h"
#include "gui/GuiButton.h"
#include "gui/GuiLabel.h"

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

// ---------------------------------------------------------------------------
// Helper factories
// ---------------------------------------------------------------------------

namespace
{
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

	// Cell (1,0): origin=(0,50), size=(100,50).
	// Child 30×20 centered → posX=0+(100-30)/2=35, posY=50+(50-20)/2=65.
	auto pos = btn->Position();
	EXPECT_EQ(35,  pos.X);
	EXPECT_EQ(65,  pos.Y);
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

	// btn1 at top (y=0), btn2 below btn1 + spacing.
	EXPECT_EQ(0,  btn1->Position().Y);
	EXPECT_EQ(35, btn2->Position().Y);  // 30 + 5
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
	EXPECT_EQ(8,  btn->Position().Y);
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

	// btn1 on row 0 (y=0), btn2 wrapped to row 1 (y=20).
	EXPECT_EQ(0, btn1->Position().Y);
	EXPECT_LT(btn1->Position().Y, btn2->Position().Y);
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
