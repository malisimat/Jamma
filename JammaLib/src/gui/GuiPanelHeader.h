#pragma once

#include "GuiLabel.h"
#include "../graphics/GlDrawContext.h"
#include "../graphics/GlDeleteQueue.h"
#include "../resources/ShaderResource.h"
#include <array>

namespace gui
{
    // Shared scene-panel title: text keeps the standard content inset while
    // the fill reaches the inside of panel_fill_flat's 16px decorative border.
    class GuiPanelHeader : public GuiLabel
    {
    public:
        static constexpr int TextLeft = 28;
        static constexpr int TextHeight = 22;
        static constexpr int TextTop = 26;
        static constexpr int BorderInset = 16;
        static constexpr int BandHeight = 42;
        static constexpr int ControlsGap = 12;
        static constexpr int ContentTopInset = BorderInset + BandHeight + ControlsGap;
        static constexpr int CornerCut = 12;

        explicit GuiPanelHeader(const std::string& text) : GuiLabel(_Params(text)), _text(text) {}

        void Layout(utils::Size2d panel, bool fullWidth = true)
        {
            const int width = static_cast<int>(panel.Width), height = static_cast<int>(panel.Height);
            const int left = std::min(TextLeft, width / 2);
            const int top = std::min(TextTop, height);
            const int labelHeight = std::min(TextHeight, std::max(0, height - top));
            SetPosition({left, height - top - labelHeight});
            SetSize({static_cast<unsigned int>(std::max(0, width - 2 * left)), static_cast<unsigned int>(labelHeight)});
            const int inset = std::min(BorderInset, width / 2);
            const int bandRight = fullWidth ? width - inset : std::min(width - inset,
                left + static_cast<int>(std::ceil(MeasureText(_text).value_or(52.0f))) + TextLeft - BorderInset);
            const utils::Rect2d band{inset, std::max(0, height - BorderInset - BandHeight),
                std::max(inset, bandRight), std::max(0, height - BorderInset)};
            _dirty = _dirty || band.Left != _band.Left || band.Right != _band.Right ||
                band.Top != _band.Top || band.Bottom != _band.Bottom;
            _band = band;
        }

        utils::Rect2d BandBounds() const { return _band; }

        static std::array<glm::vec2, 12> BandVertices(utils::Rect2d bounds)
        {
            const float left = static_cast<float>(bounds.Left), right = static_cast<float>(bounds.Right);
            const float bottom = static_cast<float>(bounds.Bottom), top = static_cast<float>(bounds.Top);
            const float cut = static_cast<float>(std::max(0, std::min({CornerCut,
                (bounds.Right - bounds.Left) / 2, (bounds.Top - bounds.Bottom) / 2})));
            const std::array<glm::vec2, 6> corners{{{left + cut,bottom}, {right,bottom},
                {right,top - cut}, {right - cut,top}, {left,top}, {left,bottom + cut}}};
            return {corners[0],corners[1],corners[2], corners[0],corners[2],corners[3],
                corners[0],corners[3],corners[4], corners[0],corners[4],corners[5]};
        }

        void Draw(base::DrawContext& context) override
        {
            if (!IsVisible() || !GetSize().Width || !GetSize().Height) return;
            if (auto shader = _shader.lock(); shader && _array && context.GetContextType() == base::DrawContext::OPENGL)
            {
                auto& gl = static_cast<graphics::GlDrawContext&>(context);
                if (_dirty)
                {
                    const auto vertices = BandVertices(_band);
                    glBindBuffer(GL_ARRAY_BUFFER, _buffer);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_DYNAMIC_DRAW);
                    _dirty = false;
                }
                // Vertices are in parent coordinates, like this label's Position().
                auto opacity = context.WithOpacity(0.90f);
                gl.SetUniform("Color", glm::vec4(0.12f,0.12f,0.12f,1.0f));
                glUseProgram(shader->GetId()); shader->SetUniforms(gl);
                glBindVertexArray(_array); glDrawArrays(GL_TRIANGLES,0,12); glBindVertexArray(0);
            }
            GuiLabel::Draw(context);
        }

    protected:
        void _InitResources(resources::ResourceLib& resources, bool force) override
        {
            GuiLabel::_InitResources(resources,force);
            if (const auto resource = resources.GetResource("colour"))
                _shader = std::dynamic_pointer_cast<resources::ShaderResource>(resource->lock());
            if (!_array)
            {
                glGenVertexArrays(1,&_array); glBindVertexArray(_array);
                glGenBuffers(1,&_buffer); glBindBuffer(GL_ARRAY_BUFFER,_buffer);
                glBufferData(GL_ARRAY_BUFFER,sizeof(glm::vec2) * 12,nullptr,GL_DYNAMIC_DRAW);
                glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(glm::vec2),nullptr);
                glEnableVertexAttribArray(0); glBindVertexArray(0);
            }
            _dirty = true;
        }
        void _ReleaseResources() override
        {
            graphics::GlDeleteQueue::DeleteBuffers(1,&_buffer); _buffer = 0;
            graphics::GlDeleteQueue::DeleteVertexArrays(1,&_array); _array = 0;
            _shader.reset(); GuiLabel::_ReleaseResources();
        }

    private:
        static GuiLabelParams _Params(const std::string& text)
        {
            auto params = GuiLabelParams::PanelHeader(text,320u);
            params.Ellipsize = true;
            return params;
        }
        std::string _text;
        utils::Rect2d _band{};
        std::weak_ptr<resources::ShaderResource> _shader;
        GLuint _array = 0, _buffer = 0;
        bool _dirty = true;
    };
}
