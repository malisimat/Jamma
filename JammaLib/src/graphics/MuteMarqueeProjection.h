#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/geometric.hpp>
#include "../utils/CommonTypes.h"

namespace graphics
{
    // Window-owned projection of actual scene triangles. No GPU readback or framing-box acceptance.
    class MuteMarqueeProjection
    {
    public:
        MuteMarqueeProjection(glm::mat4 matrix, utils::Size2d viewport, utils::Rect2d rectangle,
            std::span<const utils::Rect2d> visible) : _matrix(matrix), _viewport(viewport), _rectangle(rectangle), _visible(visible) {}
        MuteMarqueeProjection Transformed(const glm::mat4& model) const
        { return {_matrix * model, _viewport, _rectangle, _visible}; }
        bool Intersects(glm::vec3 a, glm::vec3 b, glm::vec3 c) const
        {
            if (!_viewport.Width || !_viewport.Height) return false;
            const auto pa = _matrix * glm::vec4(a, 1.0f);
            const auto pb = _matrix * glm::vec4(b, 1.0f);
            const auto pc = _matrix * glm::vec4(c, 1.0f);
            for (const auto& point : {pa, pb, pc})
                for (unsigned int component = 0u; component < 4u; ++component)
                    if (!std::isfinite(point[component])) return false;
            for (const auto& visible : _visible)
            {
                const auto left = (std::max)(_rectangle.Left, visible.Left), right = (std::min)(_rectangle.Right, visible.Right);
                const auto bottom = (std::max)(_rectangle.Bottom, visible.Bottom), top = (std::min)(_rectangle.Top, visible.Top);
                if (left > right || bottom > top) continue;
                const auto x0 = 2.0f * static_cast<float>(left) / static_cast<float>(_viewport.Width) - 1.0f;
                const auto x1 = 2.0f * static_cast<float>(right) / static_cast<float>(_viewport.Width) - 1.0f;
                const auto y0 = 2.0f * static_cast<float>(bottom) / static_cast<float>(_viewport.Height) - 1.0f;
                const auto y1 = 2.0f * static_cast<float>(top) / static_cast<float>(_viewport.Height) - 1.0f;
                const std::array<glm::vec4, 10u> planes{{
                    {1,0,0,1}, {-1,0,0,1}, {0,1,0,1}, {0,-1,0,1}, {0,0,1,1}, {0,0,-1,1},
                    {1,0,0,-x0}, {-1,0,0,x1}, {0,1,0,-y0}, {0,-1,0,y1}}};
                // Homogeneous outcodes reject ordinary misses without constructing/copying
                // polygons. Exact clipping remains necessary when a triangle crosses a boundary.
                const auto ca = Outside(pa,x0,x1,y0,y1), cb = Outside(pb,x0,x1,y0,y1), cc = Outside(pc,x0,x1,y0,y1);
                if (ca & cb & cc) continue;
                if (!(ca | cb | cc) && (pa.w > 0.000001f || pb.w > 0.000001f || pc.w > 0.000001f)) return true;
                Polygon polygon; polygon.Count = 3u;
                polygon.Vertices[0] = pa; polygon.Vertices[1] = pb; polygon.Vertices[2] = pc;
                for (const auto& plane : planes)
                {
                    polygon = Clip(polygon, plane);
                    if (!polygon.Count) break;
                }
                if (polygon.Count && std::any_of(polygon.Vertices.begin(), polygon.Vertices.begin() + polygon.Count,
                    [](const auto& point) { return point.w > 0.000001f; })) return true;
            }
            return false;
        }
        // Conservative rejection only. Overlapping boxes still require the exact surface test.
        bool MayIntersectBounds(glm::vec3 minimum, glm::vec3 maximum) const
        {
            if (!_viewport.Width || !_viewport.Height) return false;
            std::array<glm::vec4,8u> corners;
            for (unsigned int i = 0u; i < corners.size(); ++i)
            {
                corners[i] = _matrix * glm::vec4{(i & 1u) ? maximum.x : minimum.x,
                    (i & 2u) ? maximum.y : minimum.y, (i & 4u) ? maximum.z : minimum.z, 1.0f};
                for (unsigned int component = 0u; component < 4u; ++component)
                    if (!std::isfinite(corners[i][component])) return true;
            }
            for (const auto& visible : _visible)
            {
                const auto left = (std::max)(_rectangle.Left, visible.Left), right = (std::min)(_rectangle.Right, visible.Right);
                const auto bottom = (std::max)(_rectangle.Bottom, visible.Bottom), top = (std::min)(_rectangle.Top, visible.Top);
                if (left > right || bottom > top) continue;
                const auto x0 = 2.0f * static_cast<float>(left) / static_cast<float>(_viewport.Width) - 1.0f, x1 = 2.0f * static_cast<float>(right) / static_cast<float>(_viewport.Width) - 1.0f;
                const auto y0 = 2.0f * static_cast<float>(bottom) / static_cast<float>(_viewport.Height) - 1.0f, y1 = 2.0f * static_cast<float>(top) / static_cast<float>(_viewport.Height) - 1.0f;
                unsigned int outside = 1023u;
                for (const auto& corner : corners) outside &= Outside(corner,x0,x1,y0,y1);
                if (!outside) return true;
            }
            return false;
        }
        static std::vector<utils::Rect2d> VisibleRegions(utils::Size2d viewport, std::span<const utils::Rect2d> covered)
        {
            std::vector<utils::Rect2d> regions{{0, 0, static_cast<int>(viewport.Width), static_cast<int>(viewport.Height)}};
            for (const auto& mask : covered)
            {
                if (mask.Left >= mask.Right || mask.Bottom >= mask.Top) continue;
                std::vector<utils::Rect2d> next;
                for (const auto& region : regions)
                {
                    const auto l = (std::max)(region.Left, mask.Left), r = (std::min)(region.Right, mask.Right);
                    const auto b = (std::max)(region.Bottom, mask.Bottom), t = (std::min)(region.Top, mask.Top);
                    if (l >= r || b >= t) { next.push_back(region); continue; }
                    if (region.Left < l) next.push_back({region.Left, region.Bottom, l, region.Top});
                    if (r < region.Right) next.push_back({r, region.Bottom, region.Right, region.Top});
                    if (region.Bottom < b) next.push_back({l, region.Bottom, r, b});
                    if (t < region.Top) next.push_back({l, t, r, region.Top});
                }
                regions = std::move(next);
            }
            return regions;
        }
    private:
        static unsigned int Outside(const glm::vec4& p, float x0, float x1, float y0, float y1)
        {
            constexpr auto epsilon = -0.000001f;
            return ((p.x + p.w < epsilon) ? 1u : 0u)
                | ((-p.x + p.w < epsilon) ? 2u : 0u)
                | ((p.y + p.w < epsilon) ? 4u : 0u)
                | ((-p.y + p.w < epsilon) ? 8u : 0u)
                | ((p.z + p.w < epsilon) ? 16u : 0u)
                | ((-p.z + p.w < epsilon) ? 32u : 0u)
                | ((p.x - x0 * p.w < epsilon) ? 64u : 0u)
                | ((-p.x + x1 * p.w < epsilon) ? 128u : 0u)
                | ((p.y - y0 * p.w < epsilon) ? 256u : 0u)
                | ((-p.y + y1 * p.w < epsilon) ? 512u : 0u);
        }
        // A triangle clipped by ten planes has at most13 vertices. No per-triangle allocation.
        struct Polygon { std::array<glm::vec4, 16u> Vertices{}; std::size_t Count = 0u; };
        static Polygon Clip(const Polygon& input, glm::vec4 plane)
        {
            Polygon output;
            if (!input.Count) return output;
            auto previous = input.Vertices[input.Count - 1u]; auto previousDistance = glm::dot(previous, plane);
            for (std::size_t i = 0u; i < input.Count; ++i)
            {
                const auto point = input.Vertices[i]; const auto distance = glm::dot(point, plane);
                const bool inside = distance >= -0.000001f, previousInside = previousDistance >= -0.000001f;
                if (inside != previousInside && output.Count < output.Vertices.size())
                    output.Vertices[output.Count++] = previous + (point - previous) * (previousDistance / (previousDistance - distance));
                if (inside && output.Count < output.Vertices.size()) output.Vertices[output.Count++] = point;
                previous = point; previousDistance = distance;
            }
            return output;
        }
        glm::mat4 _matrix;
        utils::Size2d _viewport;
        utils::Rect2d _rectangle;
        std::span<const utils::Rect2d> _visible;
    };
}
