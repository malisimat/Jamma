#pragma once

#include "GuiButton.h"
#include "GuiScrollPanel.h"
#include "../io/TriggerBindingLearn.h"
#include <sstream>
#include <utility>

namespace gui
{
    // UI owns every draft row. The input owner only publishes one complete observation.
    class GuiTriggerLearn : public GuiPanel
    {
    public:
        struct Draft
        {
            struct RowInfo { std::optional<unsigned int> Physical; bool Bright = false; };
            std::vector<std::optional<io::RigFile::Binding>> Rows;
            std::vector<RowInfo> Presentation;
            std::vector<io::RigFile::Binding> Original;
            std::optional<std::size_t> Learning;
            std::optional<io::RigFile::Binding> Before;
            RowInfo BeforeInfo;
            std::uint64_t Attempt = 0u;
            explicit Draft(std::vector<io::RigFile::Binding> original = {}) : Original(std::move(original))
            { for (const auto& row : Original) { Rows.push_back(row); Presentation.push_back({}); } if (Rows.size() < 4u) Add(); }
            void Stop() { if (Learning) { Rows[*Learning] = Before; Presentation[*Learning] = BeforeInfo; } Learning.reset(); Before.reset(); ++Attempt; }
            void Learn(std::size_t row) { Stop(); if (row < Rows.size()) { Learning = row; Before = Rows[row]; BeforeInfo = Presentation[row]; } }
            void Add() { if (Rows.size() >= 4u) return; Stop(); Rows.push_back({}); Presentation.push_back({}); Learn(Rows.size() - 1u); }
            void CancelDelete(std::size_t row)
            {
                if (row >= Rows.size()) return;
                if (Learning && *Learning == row) { Stop(); return; }
                Stop(); Rows.erase(Rows.begin() + row); Presentation.erase(Presentation.begin() + row);
            }
            bool Accept(std::uint64_t attempt, const io::RigFile::Binding& binding, std::optional<unsigned int> physical = {})
            {
                if (!Learning || attempt != Attempt) return false;
                Rows[*Learning] = binding; Presentation[*Learning] = {physical, true}; Learning.reset(); Before.reset(); return true;
            }
            std::vector<io::RigFile::Binding> Bindings() const
            { std::vector<io::RigFile::Binding> result; for (const auto& row : Rows) if (row) result.push_back(*row); return result; }
            bool Changed() const { return !Learning && Bindings() != Original; }
        };
        GuiTriggerLearn() : GuiPanel(_Params()) {}
        void Init(const io::RigFile& rig, std::string id, io::RigFile::Control control,
            std::uint64_t revision, std::uint64_t session, utils::Size2d viewport, bool altHeld = true,
            std::vector<io::TriggerBindingLearn::Device> devices = {})
        {
            _rig = rig; Id = std::move(id); Control = control; Revision = revision; Session = session;
            _altReleased = !altHeld; _devices = std::move(devices);
            const auto target = std::find_if(rig.Triggers.begin(), rig.Triggers.end(), [&](const auto& trigger) { return trigger.Id == Id; });
            _draft = Draft(control == io::RigFile::Control::Activate ? target->ControlBindings.Activate : target->ControlBindings.Ditch);
            _title = Label("Learn " + target->Name + (control == io::RigFile::Control::Activate ? " / Activate" : " / Ditch"), 12, 346, 616u);
            _note = Label("MIDI and serial controls remain active while learning.", 12, 318, 616u);
            _status = Label("", 12, 278, 616u, 38u);
            GuiScrollPanelParams scroll; scroll.Size = {616u, 206u}; scroll.MinSize = {1u, 1u}; scroll.Position = {12, 68}; scroll.ScrollBarWidth = 12u;
            _scroll = std::make_shared<GuiScrollPanel>(scroll); AddChild(_scroll);
            _plus = Button("+", 12, 16, 50u, [this] { if (!Applying) { _draft.Add(); Changed(); } });
            _ok = Button("OK", 388, 16, 112u, [this] { if (!Applying && _draft.Changed()) ApplyRequested = true; });
            _cancel = Button("Cancel", 512, 16, 112u, [this] { if (!Applying) CloseRequested = true; });
            Fit(viewport); Changed(); Refresh();
        }
        void Fit(utils::Size2d viewport)
        {
            if (viewport.Width == _lastFit.Width && viewport.Height == _lastFit.Height) return;
            _lastFit = viewport; _rowsDirty = true;
            const auto width = std::min(640u, viewport.Width), height = std::min(384u, viewport.Height);
            SetPosition({static_cast<int>((viewport.Width - width) / 2u), static_cast<int>((viewport.Height - height) / 2u)});
            SetSize({width, height});
            if (!_scroll) return;
            const auto innerWidth = width > 24u ? width - 24u : 1u;
            _title->SetSize({innerWidth, 26u}); _title->SetPosition({12, static_cast<int>(height) - 38});
            _note->SetPosition({12, static_cast<int>(height) - 66}); _note->SetSize({innerWidth,26u});
            _status->SetPosition({12, static_cast<int>(height) - 106}); _status->SetSize({innerWidth, 38u});
            _scroll->SetPosition({12,56}); _scroll->SetSize({innerWidth, height > 164u ? height - 164u : 1u});
            const auto footerWidth = std::min(112u, width > 72u ? (width - 72u) / 2u : 1u);
            _plus->SetPosition({8,12}); _plus->SetSize({40u,36u});
            _ok->SetPosition({static_cast<int>(width) - 2 * static_cast<int>(footerWidth) - 16,12}); _ok->SetSize({footerWidth,36u});
            _cancel->SetPosition({static_cast<int>(width) - static_cast<int>(footerWidth) - 8,12}); _cancel->SetSize({footerWidth,36u});
        }
        void Refresh()
        {
            if (_rowsDirty)
            {
                Rebuild(); _rowsDirty = false;
                _valid = io::TriggerBindingLearn::ReplacePedal(_rig, Id, Control, _draft.Bindings(), _validation).has_value();
            }
            _ok->SetEnabled(!Applying && !Invalidated && _draft.Changed() && _valid);
            _cancel->SetEnabled(!Applying); _plus->SetEnabled(!Applying && !Invalidated && _draft.Rows.size() < 4u);
            _status->SetString(Applying ? "Applying bindings..." : !_error.empty() ? _error :
                _draft.Learning ? (_armed ? "Learning new binding... (press MIDI, serial button or key)" : "Waiting for input learning to arm...") :
                !_validation.empty() ? _validation : "Empty rows are ignored. MIDI note labels use C4 = note 60.");
        }
        std::optional<io::TriggerBindingLearn::Request> TakeRequest()
        {
            if (!_requestDirty) return {};
            _requestDirty = false; return io::TriggerBindingLearn::Request{Session, _draft.Learning ? _draft.Attempt : 0u, Revision};
        }
        void Observe(const std::shared_ptr<const io::TriggerBindingLearn::Observation>& value)
        {
            if (!value || value->Identity.Session != Session || !_draft.Learning || value->Identity.Attempt != _draft.Attempt) return;
            _armed = value->Armed;
            if (!value->Error.empty())
            { _error = value->Error; Invalidated = true; _draft.Stop(); _rowsDirty = true; return; }
            if (value->Binding && _draft.Accept(value->Identity.Attempt, *value->Binding, value->PhysicalChannel))
            { _rowsDirty = true; _error.clear(); }
        }
        bool AdmitKeyboard(const actions::KeyAction& action, bool fresh)
        {
            if (action.KeyChar == VK_MENU || action.KeyChar == VK_LMENU || action.KeyChar == VK_RMENU)
            { if (action.KeyActionType == actions::KeyAction::KEY_UP) _altReleased = true; return false; }
            return !Applying && !Invalidated && _draft.Learning && _armed && _altReleased && fresh &&
                action.Modifiers == base::Action::MODIFIER_NONE &&
                action.KeyActionType == actions::KeyAction::KEY_DOWN && io::TriggerBindingLearn::KeyboardAllowed(action.KeyChar);
        }
        void Reject(std::string error)
        { if (!Applying && _error == error) return; Applying = false; _rowsDirty = true; _error = std::move(error); Refresh(); }
        void Invalidate(std::string error) { Invalidated = true; _draft.Stop(); Reject(std::move(error)); }
        void SetApplying() { Applying = true; _rowsDirty = true; Refresh(); }
        std::vector<io::RigFile::Binding> Bindings() const { return _draft.Bindings(); }
        std::string Id;
        io::RigFile::Control Control = io::RigFile::Control::Activate;
        std::uint64_t Revision = 0u, Session = 0u;
        bool Applying = false, ApplyRequested = false, CloseRequested = false, Invalidated = false;
        using GuiPanel::OnAction;
        actions::ActionResult OnAction(actions::KeyAction action) override
        {
            if (action.KeyChar == VK_RETURN && action.KeyActionType == actions::KeyAction::KEY_UP && _ok->IsEnabled()) ApplyRequested = true;
            return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
        }
    private:
        class Click : public GuiButton
        {
        public:
            Click(GuiButtonParams params, std::function<void()> run) : GuiButton(params), _run(std::move(run)) {}
            using GuiButton::OnAction;
            actions::ActionResult OnAction(actions::TouchAction action) override
            {
                if (action.Touch == actions::TouchAction::TOUCH_MOUSE && action.Index != 0) return actions::ActionResult::NoAction();
                const bool was = _down; auto result = GuiButton::OnAction(action);
                if (action.State == actions::TouchAction::TOUCH_DOWN) { _down = result.IsEaten; _index = action.Index; _source = action.Touch; }
                else if (action.Index == _index && action.Touch == _source)
                { _down = false; if (was && IsEnabled() && HitTest(action.Position)) _run(); }
                return result;
            }
            void ClearPointerState() override { _down = false; GuiButton::ClearPointerState(); }
        private:
            std::function<void()> _run; bool _down = false;
            int _index = 0; actions::TouchAction::TouchType _source = actions::TouchAction::TOUCH_MOUSE;
        };
        static base::GuiElementParams _Params()
        {
            base::GuiElementParams params; params.Size = {640u,384u}; params.MinSize = {1u,1u};
            params.Texture = "rounded_but"; params.TextureShader = "texture_tinted"; params.TintColor = GuiStyle::Graphite(); params.GuiPassThrough = false; return params;
        }
        std::shared_ptr<GuiLabel> Label(std::string text, int x, int y, unsigned int width, unsigned int height = 26u)
        {
            auto params = GuiLabelParams::PanelHeader(text, static_cast<int>(width), height); params.Position = {x,y};
            auto label = std::make_shared<GuiLabel>(params); AddChild(label); return label;
        }
        std::shared_ptr<Click> Button(std::string text, int x, int y, unsigned int width, std::function<void()> run,
            const std::shared_ptr<GuiPanel>& parent = {})
        {
            auto params = GuiButtonParams::PanelButton(width); params.Position = {x,y}; params.Text = std::move(text);
            auto button = std::make_shared<Click>(params, std::move(run)); (parent ? parent.get() : this)->AddChild(button); return button;
        }
        std::string Describe(const io::RigFile::Binding& binding, std::optional<unsigned int> physical) const
        {
            std::ostringstream text;
            std::string display = binding.Device; bool available = binding.Source == io::RigFile::BindingSource::Keyboard;
            for (const auto& device : _devices)
                if (device.Source == binding.Source && device.Id == binding.Device) { display = device.Display + " [" + device.Id + "]"; available = device.Available; }
            if (binding.Source == io::RigFile::BindingSource::Keyboard) text << "Keyboard key " << binding.Press.Code;
            else if (binding.Source == io::RigFile::BindingSource::Serial) text << "Serial " << display << " button " << binding.Press.Code;
            else
            {
                text << "MIDI " << display << " ch " << (binding.Press.Channel ? std::to_string(*binding.Press.Channel + 1u) : "any")
                    << (binding.Press.Kind == io::RigFile::CC ? " CC " : " note ") << binding.Press.Code;
                if (binding.Press.Kind == io::RigFile::NOTE)
                {
                    static constexpr const char* pitches[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                    text << " (" << pitches[binding.Press.Code % 12u] << static_cast<int>(binding.Press.Code / 12u) - 1 << ")";
                }
                if (physical && binding.Press.Channel && *physical != *binding.Press.Channel) text << " (physical ch " << *physical + 1u << ")";
            }
            if (!available) text << " / unavailable";
            text << (binding.Press.State ? " / press" : " / release"); return text.str();
        }
        void Changed() { _requestDirty = true; _rowsDirty = true; _armed = false; _error.clear(); }
        void Rebuild()
        {
            const auto offset = _scroll->ScrollOffset();
            auto content = std::make_shared<GuiPanel>(base::GuiElementParams{});
            const auto height = std::max(44u, static_cast<unsigned int>(_draft.Rows.size()) * 96u);
            const auto panelWidth = _scroll->GetSize().Width;
            const auto width = panelWidth > 12u && height > _scroll->ViewportHeight() ? panelWidth - 12u : panelWidth;
            content->SetSize({width,height});
            for (std::size_t i = 0u; i < _draft.Rows.size(); ++i)
            {
                const int y = static_cast<int>(height - (i + 1u) * 96u);
                const bool learning = _draft.Learning == i;
                const int textWidth = std::max(40, static_cast<int>(width) - 134);
                auto label = GuiLabelParams::PanelHeader(std::to_string(i + 1u) + ". " + (_draft.Rows[i] ? Describe(*_draft.Rows[i], _draft.Presentation[i].Physical) : "Empty binding"), textWidth, 88u);
                label.FontSize = graphics::FontOptions::FONT_SMALL; label.Position = {4,y};
                if (_draft.Presentation[i].Bright)
                {
                    base::GuiElementParams highlight; highlight.Position = label.Position; highlight.Size = label.Size;
                    highlight.Texture = "rounded_but"; highlight.TextureShader = "texture_tinted"; highlight.TintColor = {0.08f,0.55f,0.65f}; highlight.GuiPassThrough = true;
                    content->AddChild(std::make_shared<base::GuiElement>(highlight));
                }
                auto rowLabel = std::make_shared<GuiLabel>(label); content->AddChild(rowLabel);
                auto learn = Button(learning ? "Stop learn" : "Learn", textWidth + 12, y + 48, 112u, [this,i] {
                    if (Applying || Invalidated) return; if (_draft.Learning == i) _draft.Stop(); else _draft.Learn(i); Changed(); }, content);
                auto remove = Button(learning ? "Cancel" : "Delete", textWidth + 12, y + 4, 112u, [this,i] {
                    if (!Applying && !Invalidated) { _draft.CancelDelete(i); Changed(); } }, content);
                learn->SetEnabled(!Applying && !Invalidated); remove->SetEnabled(!Applying && !Invalidated);
            }
            _scroll->SetContent(content);
            _scroll->SetScrollOffset(offset);
        }
        Draft _draft;
        io::RigFile _rig;
        std::vector<io::TriggerBindingLearn::Device> _devices;
        std::shared_ptr<GuiScrollPanel> _scroll;
        std::shared_ptr<GuiLabel> _title, _note, _status;
        std::shared_ptr<Click> _plus, _ok, _cancel;
        bool _requestDirty = false, _rowsDirty = false, _armed = false, _altReleased = false;
        bool _valid = false;
        utils::Size2d _lastFit{};
        std::string _error, _validation;
    };
}
