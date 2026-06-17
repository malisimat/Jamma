#pragma once

#include <functional>
#include <vector>
#include <algorithm>
#include "GuiPanel.h"

namespace gui
{
	// Definition of a single row or column track in the grid.
	struct GridCellDef
	{
		enum class Sizing : std::uint8_t
		{
			Fixed,  // fixedSize pixels wide/tall.
			Auto,   // sized to fit the largest child in this track.
			Fill    // stretches to consume remaining space.
		};

		Sizing       sizing    = Sizing::Fixed;
		unsigned int fixedSize = 0u;  // pixels (Sizing::Fixed only).
		unsigned int minSize   = 0u;  // floor for all sizing modes.
		unsigned int spacing   = 0u;  // gap in pixels after this track.
	};

	// Placement descriptor that positions a child inside the grid.
	struct GridChildPlacement
	{
		unsigned int         row     = 0u;
		unsigned int         col     = 0u;
		unsigned int         rowSpan = 1u;
		unsigned int         colSpan = 1u;
		base::LayoutHAlign   hAlign  = base::LayoutHAlign::Fill;
		base::LayoutVAlign   vAlign  = base::LayoutVAlign::Fill;
	};

	struct GuiGridParams : public base::GuiElementParams
	{
		std::vector<GridCellDef> Rows;
		std::vector<GridCellDef> Cols;
		unsigned int PaddingH = 0u;
		unsigned int PaddingV = 0u;
	};

	// A retained grid-layout container.
	// Children must be placed via AddGridChild() for explicit cell assignment,
	// or via AddChild() for sequential left-to-right, top-to-bottom placement.
	// Call InvalidateLayout() whenever anything that affects sizing changes.
	// Layout is recomputed lazily on the first Draw() after invalidation or on
	// an explicit ComputeLayout() call.
	class GuiGrid : public GuiPanel
	{
	public:
		explicit GuiGrid(GuiGridParams params);

	public:
		void SetRows(std::vector<GridCellDef> rows);
		void SetCols(std::vector<GridCellDef> cols);
		void SetPadding(unsigned int horizontal, unsigned int vertical);

		// Add a child at an explicit cell position.  Also registers the child
		// with the element tree so Draw/hit-test/resource-init finds it.
		bool AddGridChild(const std::shared_ptr<base::GuiElement>& child,
		                  GridChildPlacement placement);

		// Remove a child from both the element tree and the placement list.
		bool RemoveGridChild(const std::shared_ptr<base::GuiElement>& child);

		// Auto-place: sequential left-to-right, top-to-bottom fill.
		virtual void AddChild(std::shared_ptr<base::GuiElement> child) override;

		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;

		void InvalidateLayout();
		void ComputeLayout();

		// Initialise resources for self + children, then immediately run layout
		// so that Auto-sized tracks reflect real ContentSize() values.
		virtual void InitResources(resources::ResourceLib& resourceLib,
		                           bool forceInit) override;

		// Test accessors: origin and size of a single grid cell.
		utils::Position2d CellOrigin(unsigned int row, unsigned int col) const;
		utils::Size2d     CellSize  (unsigned int row, unsigned int col) const;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib,
		                            bool forceInit) override;

	private:
		struct PlacedChild
		{
			std::shared_ptr<base::GuiElement> element;
			GridChildPlacement                placement;
		};

		// Distribute `available` pixels among the given track definitions.
		// `padding` is the one-sided padding (applied twice).
		// `autoMeasure(i)` returns the natural content size for track i.
		static std::vector<unsigned int> _ComputeTrackSizes(
		    const std::vector<GridCellDef>& defs,
		    unsigned int available,
		    unsigned int padding,
		    const std::function<unsigned int(unsigned int)>& autoMeasure);

		utils::Size2d            _padding;
		std::vector<GridCellDef> _rows;
		std::vector<GridCellDef> _cols;
		std::vector<PlacedChild> _placedChildren;
		bool                     _layoutDirty;
		std::vector<unsigned int> _computedRowSizes;
		std::vector<unsigned int> _computedColSizes;
	};
}
