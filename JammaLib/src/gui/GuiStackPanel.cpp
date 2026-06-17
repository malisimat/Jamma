#include "GuiStackPanel.h"

#include <algorithm>

using namespace base;
using namespace gui;
using namespace utils;
using namespace resources;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GuiStackPanel::GuiStackPanel(GuiStackPanelParams params)
    : GuiPanel(params)
    , _direction(params.Direction)
    , _spacing(params.Spacing)
    , _padding{ params.PaddingH, params.PaddingV }
    , _wrapContent(params.WrapContent)
    , _layoutDirty(true)
{}

// ---------------------------------------------------------------------------
// Configuration setters
// ---------------------------------------------------------------------------

void GuiStackPanel::SetDirection(StackDirection direction)
{
    _direction = direction;
    InvalidateLayout();
}

void GuiStackPanel::SetSpacing(unsigned int spacing)
{
    _spacing = spacing;
    InvalidateLayout();
}

void GuiStackPanel::SetPadding(unsigned int horizontal, unsigned int vertical)
{
    _padding = { horizontal, vertical };
    InvalidateLayout();
}

void GuiStackPanel::SetWrapContent(bool wrap)
{
    _wrapContent = wrap;
    InvalidateLayout();
}

// ---------------------------------------------------------------------------
// Child / size management
// ---------------------------------------------------------------------------

void GuiStackPanel::AddChild(std::shared_ptr<base::GuiElement> child)
{
    GuiElement::AddChild(child);
    _layoutDirty = true;
}

void GuiStackPanel::SetSize(utils::Size2d size)
{
    GuiElement::SetSize(size);
    InvalidateLayout();
}

void GuiStackPanel::Draw(base::DrawContext& ctx)
{
    ComputeLayout();
    GuiElement::Draw(ctx);
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

void GuiStackPanel::InitResources(ResourceLib& resourceLib, bool forceInit)
{
    GuiElement::InitResources(resourceLib, forceInit);
    _layoutDirty = true;
    ComputeLayout();
}

void GuiStackPanel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
    GuiPanel::_InitResources(resourceLib, forceInit);
}

// ---------------------------------------------------------------------------
// Layout invalidation
// ---------------------------------------------------------------------------

void GuiStackPanel::InvalidateLayout()
{
    _layoutDirty = true;
}

// ---------------------------------------------------------------------------
// Layout computation entry point
// ---------------------------------------------------------------------------

void GuiStackPanel::ComputeLayout()
{
    if (!_layoutDirty)
        return;

    _layoutDirty = false;

    if (_children.empty())
        return;

    if (_direction == StackDirection::Horizontal && _wrapContent)
        _ComputeHorizontalWrapped();
    else if (_direction == StackDirection::Horizontal)
        _ComputeHorizontal();
    else
        _ComputeVertical();
}

// ---------------------------------------------------------------------------
// Vertical stacking
// ---------------------------------------------------------------------------

void GuiStackPanel::_ComputeVertical()
{
    const unsigned int panelW = GetSize().Width;
    const unsigned int panelH = GetSize().Height;
    const unsigned int padH   = _padding.Width;
    const unsigned int padV   = _padding.Height;

    // Available height after padding.
    unsigned int available = (panelH > 2u * padV) ? panelH - 2u * padV : 0u;

    // Account for spacing between children.
    const unsigned int n = static_cast<unsigned int>(_children.size());
    unsigned int totalSpacing = (n > 1u) ? _spacing * (n - 1u) : 0u;
    if (available > totalSpacing)
        available -= totalSpacing;
    else
        available = 0u;

    // First pass: sum fixed+auto children heights; count Fill children.
    unsigned int fixedH    = 0u;
    unsigned int fillCount = 0u;

    for (const auto& child : _children)
    {
        if (child->GetVertSizing() == LayoutSizing::Fill)
        {
            ++fillCount;
        }
        else
        {
            unsigned int h = (child->GetVertSizing() == LayoutSizing::Auto)
                ? child->ContentSize().Height
                : child->GetSize().Height;
            fixedH += h;
        }
    }

    // Distribute remaining height to Fill children.
    unsigned int fillH   = 0u;
    unsigned int fillExtra = 0u;
    if (fillCount > 0u && available > fixedH)
    {
        unsigned int remaining = available - fixedH;
        fillH     = remaining / fillCount;
        fillExtra = remaining % fillCount;
    }

    // Second pass: assign positions and sizes.
    int y = static_cast<int>(padV);
    unsigned int fillSeen = 0u;

    for (const auto& child : _children)
    {
        // Width: fill panel width or use element's own width.
        unsigned int childW;
        if (child->GetHorizSizing() == LayoutSizing::Fill)
        {
            childW = (panelW > 2u * padH) ? panelW - 2u * padH : 0u;
        }
        else if (child->GetHorizSizing() == LayoutSizing::Auto)
        {
            childW = std::min(child->ContentSize().Width,
                              (panelW > 2u * padH) ? panelW - 2u * padH : 0u);
        }
        else
        {
            childW = child->GetSize().Width;
        }

        // Height: fixed, auto, or fill.
        unsigned int childH;
        if (child->GetVertSizing() == LayoutSizing::Fill)
        {
            childH = fillH + (fillSeen < fillExtra ? 1u : 0u);
            ++fillSeen;
        }
        else if (child->GetVertSizing() == LayoutSizing::Auto)
        {
            childH = child->ContentSize().Height;
        }
        else
        {
            childH = child->GetSize().Height;
        }

        child->SetPosition({ static_cast<int>(padH), y });
        child->SetSize({ childW, childH });
        y += static_cast<int>(childH) + static_cast<int>(_spacing);
    }
}

// ---------------------------------------------------------------------------
// Horizontal stacking (non-wrapping)
// ---------------------------------------------------------------------------

void GuiStackPanel::_ComputeHorizontal()
{
    const unsigned int panelW = GetSize().Width;
    const unsigned int panelH = GetSize().Height;
    const unsigned int padH   = _padding.Width;
    const unsigned int padV   = _padding.Height;

    unsigned int available = (panelW > 2u * padH) ? panelW - 2u * padH : 0u;
    const unsigned int n   = static_cast<unsigned int>(_children.size());
    unsigned int totalSpacing = (n > 1u) ? _spacing * (n - 1u) : 0u;
    if (available > totalSpacing)
        available -= totalSpacing;
    else
        available = 0u;

    unsigned int fixedW    = 0u;
    unsigned int fillCount = 0u;

    for (const auto& child : _children)
    {
        if (child->GetHorizSizing() == LayoutSizing::Fill)
        {
            ++fillCount;
        }
        else
        {
            unsigned int w = (child->GetHorizSizing() == LayoutSizing::Auto)
                ? child->ContentSize().Width
                : child->GetSize().Width;
            fixedW += w;
        }
    }

    unsigned int fillW     = 0u;
    unsigned int fillExtra = 0u;
    if (fillCount > 0u && available > fixedW)
    {
        unsigned int remaining = available - fixedW;
        fillW     = remaining / fillCount;
        fillExtra = remaining % fillCount;
    }

    int x = static_cast<int>(padH);
    unsigned int fillSeen = 0u;

    for (const auto& child : _children)
    {
        unsigned int childH;
        if (child->GetVertSizing() == LayoutSizing::Fill)
        {
            childH = (panelH > 2u * padV) ? panelH - 2u * padV : 0u;
        }
        else if (child->GetVertSizing() == LayoutSizing::Auto)
        {
            childH = std::min(child->ContentSize().Height,
                              (panelH > 2u * padV) ? panelH - 2u * padV : 0u);
        }
        else
        {
            childH = child->GetSize().Height;
        }

        unsigned int childW;
        if (child->GetHorizSizing() == LayoutSizing::Fill)
        {
            childW = fillW + (fillSeen < fillExtra ? 1u : 0u);
            ++fillSeen;
        }
        else if (child->GetHorizSizing() == LayoutSizing::Auto)
        {
            childW = child->ContentSize().Width;
        }
        else
        {
            childW = child->GetSize().Width;
        }

        child->SetPosition({ x, static_cast<int>(padV) });
        child->SetSize({ childW, childH });
        x += static_cast<int>(childW) + static_cast<int>(_spacing);
    }
}

// ---------------------------------------------------------------------------
// Horizontal stacking with row-wrapping (responsive narrow-width behaviour)
// ---------------------------------------------------------------------------

void GuiStackPanel::_ComputeHorizontalWrapped()
{
    const unsigned int panelW = GetSize().Width;
    const unsigned int panelH = GetSize().Height;
    const unsigned int padH   = _padding.Width;
    const unsigned int padV   = _padding.Height;
    const unsigned int innerW = (panelW > 2u * padH) ? panelW - 2u * padH : 0u;

    // Build rows: each row is a list of child indices.
    std::vector<std::vector<unsigned int>> rows;
    std::vector<unsigned int> currentRow;
    unsigned int rowWidth = 0u;

    for (unsigned int i = 0u; i < static_cast<unsigned int>(_children.size()); ++i)
    {
        const auto& child = _children[i];
        unsigned int childW = (child->GetHorizSizing() == LayoutSizing::Auto)
            ? child->ContentSize().Width
            : child->GetSize().Width;

        if (!currentRow.empty() && rowWidth + _spacing + childW > innerW)
        {
            rows.push_back(currentRow);
            currentRow.clear();
            rowWidth = 0u;
        }

        if (!currentRow.empty())
            rowWidth += _spacing;
        currentRow.push_back(i);
        rowWidth += childW;
    }
    if (!currentRow.empty())
        rows.push_back(currentRow);

    // Lay out each row.
    int y = static_cast<int>(padV);
    for (const auto& row : rows)
    {
        // Compute row height = max child height in this row.
        unsigned int rowH = 0u;
        for (unsigned int idx : row)
        {
            unsigned int childH = (_children[idx]->GetVertSizing() == LayoutSizing::Auto)
                ? _children[idx]->ContentSize().Height
                : _children[idx]->GetSize().Height;
            rowH = std::max(rowH, childH);
        }

        int x = static_cast<int>(padH);
        for (unsigned int idx : row)
        {
            const auto& child = _children[idx];
            unsigned int childW = (child->GetHorizSizing() == LayoutSizing::Auto)
                ? child->ContentSize().Width
                : child->GetSize().Width;
            // Fill children get the full row height; other sizing modes are capped.
            unsigned int childH;
            if (child->GetVertSizing() == LayoutSizing::Fill)
            {
                childH = rowH;
            }
            else
            {
                unsigned int naturalH = (child->GetVertSizing() == LayoutSizing::Auto)
                    ? child->ContentSize().Height
                    : child->GetSize().Height;
                childH = std::min(naturalH, rowH);
            }

            child->SetPosition({ x, y });
            child->SetSize({ childW, childH });
            x += static_cast<int>(childW) + static_cast<int>(_spacing);
        }

        y += static_cast<int>(rowH) + static_cast<int>(_spacing);
        (void)panelH;  // available for future clip/scroll use
    }
}
