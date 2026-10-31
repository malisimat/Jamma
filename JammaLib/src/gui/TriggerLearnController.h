#pragma once

#include "GuiPopup.h"
#include "../io/TriggerBindingLearn.h"
#include <sstream>
#include <algorithm>

namespace gui
{
    // Trigger-specific draft and input state. GuiPopup owns only generic presentation.
    class TriggerLearnController : public base::ActionReceiver
    {
    public:
        using InputExchange = std::function<std::shared_ptr<const io::TriggerBindingLearn::Observation>(
            std::optional<io::TriggerBindingLearn::Request>)>;
        enum class UpdateResult { Active, Close, Applied };
        class WeakActionReceiver : public base::ActionReceiver
        {
        public:
            explicit WeakActionReceiver(std::weak_ptr<TriggerLearnController> owner) : _owner(std::move(owner)) {}
            actions::ActionResult OnAction(actions::GuiAction action) override
            { if (auto owner = _owner.lock()) return owner->OnAction(action); return actions::ActionResult::NoAction(); }
        private:
            std::weak_ptr<TriggerLearnController> _owner;
        };
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

        void Init(const io::RigFile& rig, std::string id, io::RigFile::Control control,
            std::uint64_t revision, std::uint64_t session, utils::Size2d viewport, bool altHeld,
            std::vector<io::TriggerBindingLearn::Device> devices)
        {
            _rig = rig; Id = std::move(id); Control = control; Revision = revision; Session = session;
            _altReleased = !altHeld; _devices = std::move(devices);
            const auto target = std::find_if(rig.Triggers.begin(),rig.Triggers.end(),[&](const auto& trigger) { return trigger.Id == Id; });
            _draft = Draft(control == io::RigFile::Control::Activate ? target->ControlBindings.Activate : target->ControlBindings.Ditch);
            auto params = GuiPopupParams::PanelDefault(); params.Size = {640u,384u}; params.MinSize = {1u,1u};
            _popup = std::make_shared<GuiPopup>(params);
            _popup->SetTitle(target->Name + (control == io::RigFile::Control::Activate ? " / Activate" : " / Ditch") + " bindings");
            _self = std::static_pointer_cast<TriggerLearnController>(shared_from_this());
            _buttonReceiver = std::make_shared<WeakActionReceiver>(_self);
            _popup->SetButtonReceiver(_buttonReceiver);
            _popup->ConfigureButtons({{{"+",0u,true},{"OK",1u},{"Cancel",2u}},true});
            _popup->SetKeyAction([weak = _self](actions::KeyAction action) {
                if (auto self = weak.lock(); self && action.KeyChar == VK_RETURN && action.KeyActionType == actions::KeyAction::KEY_UP && self->_draft.Changed() && self->_valid)
                    self->ApplyRequested = true;
                return true;
            });
            Fit(viewport); ChangedState(); Refresh();
        }
        std::shared_ptr<GuiPopup> Popup() const { return _popup; }
        void Fit(utils::Size2d viewport)
        {
            if (viewport.Width == _lastFit.Width && viewport.Height == _lastFit.Height) return;
            _lastFit = viewport; _rowsDirty = true; _popup->FitToViewport(viewport);
        }
        void Refresh()
        {
            if (_rowsDirty)
            {
                std::vector<GuiPopupRow> rows; rows.reserve(_draft.Rows.size());
                for (std::size_t i = 0; i < _draft.Rows.size(); ++i)
                    rows.push_back({std::to_string(i + 1u) + ". " + (_draft.Rows[i] ? Describe(*_draft.Rows[i],_draft.Presentation[i].Physical) : "Empty binding"),
                        _draft.Learning == i ? "Stop" : "Learn",_draft.Learning == i,_draft.Presentation[i].Bright,!Applying && !Invalidated});
                _popup->SetRows(rows,
                    [weak = _self](std::size_t i) { if (auto self = weak.lock(); self && !self->Applying && !self->Invalidated)
                        { if (self->_draft.Learning == i) self->_draft.Stop(); else self->_draft.Learn(i); self->ChangedState(); } },
                    [weak = _self](std::size_t i) { if (auto self = weak.lock(); self && !self->Applying && !self->Invalidated)
                        { self->_draft.CancelDelete(i); self->ChangedState(); } },
                    _revealLearning ? _draft.Learning : std::nullopt);
                _rowsDirty = false; _revealLearning = false;
                _valid = io::TriggerBindingLearn::ReplacePedal(_rig,Id,Control,_draft.Bindings(),_validation).has_value();
            }
            _popup->SetVisualMode(_draft.Learning ? GuiPopupParams::VisualMode::Learning : GuiPopupParams::VisualMode::Normal);
            _popup->SetRowText("MIDI and serial controls remain active while learning.",
                Applying ? "Applying bindings..." : !_error.empty() ? _error :
                _draft.Learning ? (_armed ? "Learning new binding... (press MIDI, serial button or key)" : "Waiting for input learning to arm...") :
                !_validation.empty() ? _validation : "Empty rows are ignored. MIDI note labels use C4 = note 60.");
            _popup->SetActionEnabled(0u,!Applying && !Invalidated && _draft.Rows.size() < 4u);
            _popup->SetActionEnabled(1u,!Applying && !Invalidated && _draft.Changed() && _valid);
            _popup->SetActionEnabled(2u,!Applying);
        }
        std::optional<io::TriggerBindingLearn::Request> TakeRequest()
        { if (!_requestDirty) return {}; _requestDirty = false; return io::TriggerBindingLearn::Request{Session,_draft.Learning ? _draft.Attempt : 0u,Revision}; }
        void Observe(const std::shared_ptr<const io::TriggerBindingLearn::Observation>& value)
        {
            if (!value || !_draft.Learning || value->Identity != io::TriggerBindingLearn::Request{Session, _draft.Attempt, Revision}) return;
            _armed = value->Armed;
            if (!value->Error.empty()) { _error = value->Error; Invalidated = true; _draft.Stop(); _rowsDirty = true; return; }
            if (value->Binding && _draft.Accept(value->Identity.Attempt,*value->Binding,value->PhysicalChannel))
            { _rowsDirty = true; _error.clear(); }
        }
        UpdateResult Update(const io::RigFile& rig, std::uint64_t revision, utils::Size2d viewport,
            const InputExchange& input, const std::function<bool(const io::RigFile&, std::size_t)>& submit,
            const std::function<bool()>& editApplying)
        {
            if (CloseRequested) return UpdateResult::Close;
            const auto target = std::find_if(rig.Triggers.begin(), rig.Triggers.end(),
                [&](const auto& trigger) { return trigger.Id == Id; });
            if (Revision != revision)
            {
                const bool accepted = Applying && target != rig.Triggers.end() &&
                    (Control == io::RigFile::Control::Activate ? target->ControlBindings.Activate : target->ControlBindings.Ditch) == Bindings();
                if (accepted) return UpdateResult::Applied;
                if (input) input(io::TriggerBindingLearn::Request{});
                if (!Invalidated) Invalidate("Rig changed; cancel and reopen learning");
                Fit(viewport); Refresh();
                return UpdateResult::Active;
            }
            if (input) Observe(input(TakeRequest()));
            if (ApplyRequested)
            {
                ApplyRequested = false;
                std::string error;
                const auto candidate = io::TriggerBindingLearn::ReplacePedal(rig, Id, Control, Bindings(), error);
                if (!candidate) Reject(error);
                else if (target == rig.Triggers.end() || !submit || !submit(*candidate, static_cast<std::size_t>(target - rig.Triggers.begin())))
                    Reject("Finish trigger action or pending edit, then retry OK");
                else { SetApplying(); if (input) input(io::TriggerBindingLearn::Request{}); }
            }
            else if (Applying && (!editApplying || !editApplying()))
                Reject("Binding change rejected; retry when idle");
            Fit(viewport); Refresh();
            return UpdateResult::Active;
        }
        bool AdmitKeyboard(const actions::KeyAction& action, bool fresh)
        {
            if (action.KeyChar == VK_MENU || action.KeyChar == VK_LMENU || action.KeyChar == VK_RMENU)
            { if (action.KeyActionType == actions::KeyAction::KEY_UP) _altReleased = true; return false; }
            return !Applying && !Invalidated && _draft.Learning && _armed && _altReleased && fresh &&
                action.Modifiers == base::Action::MODIFIER_NONE && action.KeyActionType == actions::KeyAction::KEY_DOWN &&
                io::TriggerBindingLearn::KeyboardAllowed(action.KeyChar);
        }
        void Reject(std::string error) { Applying = false; _rowsDirty = true; _error = std::move(error); Refresh(); }
        void Invalidate(std::string error) { Invalidated = true; _draft.Stop(); Reject(std::move(error)); }
        void SetApplying() { Applying = true; _rowsDirty = true; Refresh(); }
        std::vector<io::RigFile::Binding> Bindings() const { return _draft.Bindings(); }
        actions::ActionResult OnAction(actions::GuiAction action) override
        {
            if (action.Index == 0u && !Applying && !Invalidated) { _draft.Add(); ChangedState(); }
            else if (action.Index == 1u && !Applying && _draft.Changed() && _valid) ApplyRequested = true;
            else if (action.Index == 2u && !Applying) CloseRequested = true;
            return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
        }
        std::string Id;
        io::RigFile::Control Control = io::RigFile::Control::Activate;
        std::uint64_t Revision = 0u, Session = 0u;
        bool Applying = false, ApplyRequested = false, CloseRequested = false, Invalidated = false;
    private:
        void ChangedState() { _requestDirty = true; _rowsDirty = true; _revealLearning = _draft.Learning.has_value(); _armed = false; _error.clear(); }
        std::string Describe(const io::RigFile::Binding& binding, std::optional<unsigned int> physical) const
        {
            std::ostringstream text; std::string display = binding.Device;
            bool available = binding.Source == io::RigFile::BindingSource::Keyboard;
            for (const auto& device : _devices)
                if (device.Source == binding.Source && device.Id == binding.Device)
                { display = device.Display + " [" + device.Id + "]"; available = device.Available; }
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
        Draft _draft;
        io::RigFile _rig;
        std::vector<io::TriggerBindingLearn::Device> _devices;
        std::shared_ptr<GuiPopup> _popup;
        std::shared_ptr<WeakActionReceiver> _buttonReceiver;
        std::weak_ptr<TriggerLearnController> _self;
        bool _requestDirty = false, _rowsDirty = false, _revealLearning = false, _armed = false, _altReleased = false, _valid = false;
        utils::Size2d _lastFit{};
        std::string _error, _validation;
    };
}
