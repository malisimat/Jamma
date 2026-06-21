#include "GuiGrid.h"

#include <algorithm>
#include <numeric>

using namespace base;
using namespace gui;
using namespace utils;
using namespace resources;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GuiGrid::GuiGrid(GuiGridParams params)
    : GuiPanel(params)
    , _padding{ params.PaddingH, params.PaddingV }
    , _rows(std::move(params.Rows))
    , _cols(std::move(params.Cols))
    , _layoutDirty(true)
{}

// ---------------------------------------------------------------------------
// Public configuration
// ---------------------------------------------------------------------------

void GuiGrid::SetRows(std::vector<GridCellDef> rows)
{
    _rows = std::move(rows);
    InvalidateLayout();
}

void GuiGrid::SetCols(std::vector<GridCellDef> cols)
{
    _cols = std::move(cols);
    InvalidateLayout();
}

void GuiGrid::SetPadding(unsigned int horizontal, unsigned int vertical)
{
    _padding = { horizontal, vertical };
    InvalidateLayout();
}

// ---------------------------------------------------------------------------
// Child management
// ---------------------------------------------------------------------------

bool GuiGrid::AddGridChild(const std::shared_ptr<base::GuiElement>& child,
                           GridChildPlacement placement)
{
    // Guard against duplicate placement entries (base _children already deduplicates).
    for (const auto& pc : _placedChildren)
    {
        if (pc.element == child)
            return false;
    }

    // Use GuiElement::AddChild (base, non-virtual qualified) for dedup + Init().
    GuiElement::AddChild(child);
    _placedChildren.push_back({ child, placement });
    _layoutDirty = true;
    return true;
}

void GuiGrid::AddChild(std::shared_ptr<base::GuiElement> child)
{
    // Guard against duplicate placement entries.
    for (const auto& pc : _placedChildren)
    {
        if (pc.element == child)
            return;
    }

    // Auto-place: sequential left-to-right, top-to-bottom.
    unsigned int cols = _cols.empty() ? 1u : static_cast<unsigned int>(_cols.size());
    unsigned int idx  = static_cast<unsigned int>(_placedChildren.size());

    GridChildPlacement p;
    p.row = idx / cols;
    p.col = idx % cols;

    // Call base directly to avoid recursion through virtual dispatch.
    GuiElement::AddChild(child);
    _placedChildren.push_back({ child, p });
    _layoutDirty = true;
}

bool GuiGrid::RemoveGridChild(const std::shared_ptr<base::GuiElement>& child)
{
    auto it = std::find_if(_placedChildren.begin(), _placedChildren.end(),
        [&child](const PlacedChild& pc) { return pc.element == child; });

    if (it == _placedChildren.end())
        return false;

    _placedChildren.erase(it);
    GuiPanel::RemoveChild(child);  // also removes from _children
    _layoutDirty = true;
    return true;
}

void GuiGrid::SetSize(utils::Size2d size)
{
    GuiElement::SetSize(size);
    InvalidateLayout();
}

void GuiGrid::Draw(base::DrawContext& ctx)
{
    ComputeLayout();
    GuiElement::Draw(ctx);
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

void GuiGrid::InitResources(ResourceLib& resourceLib, bool forceInit)
{
    // Base class initialises own textures and cascades to all children.
    GuiElement::InitResources(resourceLib, forceInit);
    // Now children have loaded their fonts/textures, so ContentSize() is valid.
    _layoutDirty = true;
    ComputeLayout();
}

void GuiGrid::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
    GuiPanel::_InitResources(resourceLib, forceInit);
}

// ---------------------------------------------------------------------------
// Layout invalidation
// ---------------------------------------------------------------------------

void GuiGrid::InvalidateLayout()
{
    _layoutDirty = true;
}

// ---------------------------------------------------------------------------
// Core layout algorithm
// ---------------------------------------------------------------------------

// static
std::vector<unsigned int> GuiGrid::_ComputeTrackSizes(
    const std::vector<GridCellDef>& defs,
    unsigned int available,
    unsigned int padding,
    const std::function<unsigned int(unsigned int)>& autoMeasure)
{
    const unsigned int count = static_cast<unsigned int>(defs.size());
    if (count == 0u)
        return {};

    std::vector<unsigned int> sizes(count, 0u);

    // Total spacing = sum of all per-track spacing gaps.
    unsigned int totalSpacing = 0u;
    for (const auto& d : defs)
        totalSpacing += d.spacing;

    // Available pixels for cell content after padding (both sides) and spacing.
    unsigned int usable = 0u;
    if (available > 2u * padding + totalSpacing)
        usable = available - 2u * padding - totalSpacing;

    unsigned int fixedUsed = 0u;
    unsigned int fillCount = 0u;

    for (unsigned int i = 0u; i < count; ++i)
    {
        switch (defs[i].sizing)
        {
        case GridCellDef::Sizing::Fixed:
            sizes[i]  = std::max(defs[i].fixedSize, defs[i].minSize);
            fixedUsed += sizes[i];
            break;

        case GridCellDef::Sizing::Auto:
        {
            unsigned int measured = autoMeasure(i);
            sizes[i]  = std::max(measured, defs[i].minSize);
            fixedUsed += sizes[i];
            break;
        }

        case GridCellDef::Sizing::Fill:
            sizes[i] = defs[i].minSize;  // placeholder; resolved below
            ++fillCount;
            break;
        }
    }

    // Distribute remaining space among Fill tracks.
    if (fillCount > 0u && usable > fixedUsed)
    {
        unsigned int remaining = usable - fixedUsed;
        unsigned int perFill   = remaining / fillCount;
        unsigned int extra     = remaining % fillCount;
        unsigned int fillSeen  = 0u;

        for (unsigned int i = 0u; i < count; ++i)
        {
            if (defs[i].sizing == GridCellDef::Sizing::Fill)
            {
                unsigned int alloc = perFill + (fillSeen < extra ? 1u : 0u);
                sizes[i] = std::max(alloc, defs[i].minSize);
                ++fillSeen;
            }
        }
    }

    return sizes;
}

void GuiGrid::ComputeLayout()
{
    if (!_layoutDirty)
        return;

    _layoutDirty = false;

    if (_rows.empty() || _cols.empty())
        return;

    const unsigned int totalW = GetSize().Width;
    const unsigned int totalH = GetSize().Height;
    const unsigned int padH   = _padding.Width;
    const unsigned int padV   = _padding.Height;
    const unsigned int numCols = static_cast<unsigned int>(_cols.size());
    const unsigned int numRows = static_cast<unsigned int>(_rows.size());

    // Compute column widths, passing a lambda that returns the max
    // ContentSize().Width of all non-spanning children in each column.
    _computedColSizes = _ComputeTrackSizes(
        _cols, totalW, padH,
        [&](unsigned int c) -> unsigned int
        {
            unsigned int maxW = 0u;
            for (const auto& pc : _placedChildren)
            {
                if (pc.placement.col == c && pc.placement.colSpan == 1u)
                    maxW = std::max(maxW, pc.element->ContentSize().Width);
            }
            return maxW;
        });

    // Compute row heights similarly.
    _computedRowSizes = _ComputeTrackSizes(
        _rows, totalH, padV,
        [&](unsigned int r) -> unsigned int
        {
            unsigned int maxH = 0u;
            for (const auto& pc : _placedChildren)
            {
                if (pc.placement.row == r && pc.placement.rowSpan == 1u)
                    maxH = std::max(maxH, pc.element->ContentSize().Height);
            }
            return maxH;
        });

    // Build per-track origin arrays.
    std::vector<unsigned int> colOrigins(numCols, 0u);
    {
        unsigned int x = padH;
        for (unsigned int c = 0u; c < numCols; ++c)
        {
            colOrigins[c] = x;
            x += _computedColSizes[c] + _cols[c].spacing;
        }
    }

    std::vector<unsigned int> rowOrigins(numRows, 0u);
    {
        unsigned int y = totalH > padV ? totalH - padV : 0u;
        for (unsigned int r = 0u; r < numRows; ++r)
        {
            y -= _computedRowSizes[r];
            rowOrigins[r] = y;
            if (r + 1u < numRows)
                y -= _rows[r].spacing;
        }
    }

    // Place each child.
    for (auto& pc : _placedChildren)
    {
        const unsigned int r  = pc.placement.row;
        const unsigned int c  = pc.placement.col;
        const unsigned int rs = pc.placement.rowSpan;
        const unsigned int cs = pc.placement.colSpan;

        if (r >= numRows || c >= numCols)
            continue;

        // Sum column widths across the span, adding inter-cell spacing.
        unsigned int cellW = 0u;
        for (unsigned int ci = c; ci < std::min(c + cs, numCols); ++ci)
        {
            cellW += _computedColSizes[ci];
            if (ci + 1u < c + cs && ci + 1u < numCols)
                cellW += _cols[ci].spacing;
        }

        // Sum row heights across the span.
        unsigned int cellH = 0u;
        for (unsigned int ri = r; ri < std::min(r + rs, numRows); ++ri)
        {
            cellH += _computedRowSizes[ri];
            if (ri + 1u < r + rs && ri + 1u < numRows)
                cellH += _rows[ri].spacing;
        }

        const unsigned int cellX = colOrigins[c];
        const unsigned int cellY = rowOrigins[r];

        // Determine child size and position based on alignment.
        const utils::Size2d content = pc.element->ContentSize();
        unsigned int childW, childH;
        int posX = static_cast<int>(cellX);
        int posY = static_cast<int>(cellY);

        switch (pc.placement.hAlign)
        {
        case LayoutHAlign::Fill:
            childW = cellW;
            break;
        case LayoutHAlign::Left:
            childW = std::min(content.Width, cellW);
            break;
        case LayoutHAlign::Right:
            childW = std::min(content.Width, cellW);
            posX   = static_cast<int>(cellX + cellW) - static_cast<int>(childW);
            break;
        case LayoutHAlign::Center:
            childW = std::min(content.Width, cellW);
            posX  += static_cast<int>((cellW - childW) / 2u);
            break;
        default:
            childW = cellW;
            break;
        }

        switch (pc.placement.vAlign)
        {
        case LayoutVAlign::Fill:
            childH = cellH;
            break;
        case LayoutVAlign::Top:
            childH = std::min(content.Height, cellH);
            break;
        case LayoutVAlign::Bottom:
            childH = std::min(content.Height, cellH);
            posY   = static_cast<int>(cellY + cellH) - static_cast<int>(childH);
            break;
        case LayoutVAlign::Center:
            childH = std::min(content.Height, cellH);
            posY  += static_cast<int>((cellH - childH) / 2u);
            break;
        default:
            childH = cellH;
            break;
        }

        pc.element->SetPosition({ posX, posY });
        pc.element->SetSize({ childW, childH });
    }
}

// ---------------------------------------------------------------------------
// Test accessors
// ---------------------------------------------------------------------------

utils::Position2d GuiGrid::CellOrigin(unsigned int row, unsigned int col) const
{
    if (row >= _computedRowSizes.size() || col >= _computedColSizes.size())
        return { 0, 0 };

    int x = static_cast<int>(_padding.Width);
    for (unsigned int c = 0u; c < col; ++c)
        x += static_cast<int>(_computedColSizes[c] + _cols[c].spacing);

    int y = static_cast<int>(GetSize().Height) - static_cast<int>(_padding.Height);
    for (unsigned int r = 0u; r < row; ++r)
        y -= static_cast<int>(_computedRowSizes[r] + _rows[r].spacing);

    y -= static_cast<int>(_computedRowSizes[row]);

    return { x, y };
}

utils::Size2d GuiGrid::CellSize(unsigned int row, unsigned int col) const
{
    if (row >= _computedRowSizes.size() || col >= _computedColSizes.size())
        return { 0u, 0u };

    return { _computedColSizes[col], _computedRowSizes[row] };
}
