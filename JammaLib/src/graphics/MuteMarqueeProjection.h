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
            Polygon triangle; triangle.Count = 3u;
            triangle.Vertices[0] = _matrix * glm::vec4(a, 1.0f);
            triangle.Vertices[1] = _matrix * glm::vec4(b, 1.0f);
            triangle.Vertices[2] = _matrix * glm::vec4(c, 1.0f);
            for (std::size_t i = 0u; i < triangle.Count; ++i)
                for (unsigned int component = 0u; component < 4u; ++component)
                    if (!std::isfinite(triangle.Vertices[i][component])) return false;
            for (const auto plane : {glm::vec4{1,0,0,1}, glm::vec4{-1,0,0,1}, glm::vec4{0,1,0,1},
                glm::vec4{0,-1,0,1}, glm::vec4{0,0,1,1}, glm::vec4{0,0,-1,1}})
                triangle = Clip(triangle, plane);
            if (!triangle.Count) return false;
            for (const auto& visible : _visible)
            {
                const auto left = (std::max)(_rectangle.Left, visible.Left), right = (std::min)(_rectangle.Right, visible.Right);
                const auto bottom = (std::max)(_rectangle.Bottom, visible.Bottom), top = (std::min)(_rectangle.Top, visible.Top);
                if (left > right || bottom > top) continue;
                auto polygon = triangle;
                const auto x0 = 2.0f * static_cast<float>(left) / static_cast<float>(_viewport.Width) - 1.0f;
                const auto x1 = 2.0f * static_cast<float>(right) / static_cast<float>(_viewport.Width) - 1.0f;
                const auto y0 = 2.0f * static_cast<float>(bottom) / static_cast<float>(_viewport.Height) - 1.0f;
                const auto y1 = 2.0f * static_cast<float>(top) / static_cast<float>(_viewport.Height) - 1.0f;
                for (const auto plane : {glm::vec4{1,0,0,-x0}, glm::vec4{-1,0,0,x1}, glm::vec4{0,1,0,-y0}, glm::vec4{0,-1,0,y1}})
                    polygon = Clip(polygon, plane);
                if (polygon.Count && std::any_of(polygon.Vertices.begin(), polygon.Vertices.begin() + polygon.Count,
                    [](const auto& point) { return point.w > 0.000001f; })) return true;
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
