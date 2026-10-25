#pragma once

#include "GuiPanel.h"
#include "GuiTextBox.h"
#include "GuiButton.h"
#include <functional>

namespace gui
{
	// Attached to its card: display decoration never enters the raw draft.
	class GuiTriggerRename : public GuiPanel
	{
	public:
		// The pencil and cancellation control share the same label-row slot.
		static constexpr unsigned int ActionSize = 28u;
		static constexpr int ActionX = 104;
		static constexpr int RowY = 76;
	private:
		class Editor : public GuiTextBox
		{
		public:
			Editor() : GuiTextBox(_Params()) {}
			std::function<void()> Accept, Cancel;
			using GuiTextBox::OnAction;
			actions::ActionResult OnAction(actions::KeyAction action) override
			{
				if (action.KeyChar == 27u)
				{
					if (action.KeyActionType == actions::KeyAction::KEY_DOWN && Cancel) Cancel();
					return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
				}
				return GuiTextBox::OnAction(action);
			}
			void _OnCommit() override { if (Accept) Accept(); }
		private:
			static GuiTextBoxParams _Params()
			{
				auto params = GuiTextBoxParams::PanelInput(96u); params.Texture.clear(); return params;
			}
		};
	public:
		GuiTriggerRename() : GuiPanel(base::GuiElementParams{})
		{
			SetSize({136u, 132u});
			GuiLabelParams band;
			band.UseDarkFlatBackground();
			band.Position = {4, RowY}; band.Size = {128u, ActionSize};
			AddChild(std::make_shared<base::GuiElement>(band));
			auto label = GuiLabelParams::PanelScrollRow("", 0u);
			label.Position = {6, RowY + 2}; label.Size = {96u, 24u}; label.Ellipsize = true;
			label.CenterHorizontally = true; label.WrapWidth = -1; // Keep decorated names on one centred line.
			_label = std::make_shared<GuiLabel>(label);
			_editor = std::make_shared<Editor>(); _editor->SetPosition({6, RowY}); _editor->SetSize({96u, ActionSize});
			auto cancel = GuiButtonParams::PanelButton(ActionSize);
			cancel.Texture = "rename_cancel"; cancel.OverTexture = "rename_cancel_over"; cancel.DownTexture = "rename_cancel_down";
			cancel.TintColor = glm::vec3(1.0f);
			cancel.Position = {ActionX, RowY}; cancel.Size = {ActionSize, ActionSize}; cancel.MinSize = cancel.Size;
			_cancel = std::make_shared<GuiButton>(cancel);
			AddChild(_label); AddChild(_editor); AddChild(_cancel);
			_editor->Accept = [this]() { Accept(); };
			_editor->Cancel = [this]() { if (!_pending) Cancel(); };
			_editor->SetVisible(false); _cancel->SetVisible(false);
		}
		std::function<void(const std::shared_ptr<base::GuiElement>&, bool)> Focus;
		std::function<std::string(const std::string&, const std::string&, std::uint64_t)> Submit;
		std::function<void(const std::string&)> Feedback;
		bool Active() const { return _active; }
		bool HitLabel(utils::Position2d point) const { return !_active && _label->IsVisible() && _label->HitTest(_label->ParentToLocal(point)); }
		bool OwnsPointer() const { return _active || _consumeRelease; }
		void SetDisplay(const std::string& name) { _label->SetString(name); }
		void Begin(const std::string& id, const std::string& name, std::uint64_t revision)
		{
			_id = id; _original = name; _revision = revision; _active = true; _pending = false;
			_editor->SetText(name); _editor->SetEnabled(true); _editor->SetVisible(true);
			_label->SetVisible(false); _cancel->SetVisible(true); _cancel->SetEnabled(true);
			if (Feedback) Feedback("");
			if (Focus) Focus(_editor, true);
			_editor->BeginEditing();
		}
		void Cancel()
		{
			_active = false; _pending = false;
			if (Focus) Focus(_editor, false); // Disarm before FinalizeEdits.
			_editor->SetVisible(false); _cancel->SetVisible(false); _label->SetVisible(true);
			ClearPointerState();
		}
		void Update(bool applying)
		{
			if (_active && _pending && !applying)
			{
				_pending = false; _editor->SetEnabled(true); _cancel->SetEnabled(true);
				if (Feedback) Feedback("Rename rejected; edit and retry");
			}
		}
		// Routed before controls below the draft. Outside acceptance retains its
		// release so the same click cannot also fire a pedal or another control.
		bool Touch(actions::TouchAction action, bool inViewport)
		{
			if (_consumeRelease && action.State == actions::TouchAction::TOUCH_UP)
			{ _consumeRelease = false; return true; }
			if (!_active) return false;
			if (action.State == actions::TouchAction::TOUCH_DOWN &&
				!(action.Touch == actions::TouchAction::TOUCH_MOUSE && action.Index == 4))
			{
				if (!_pending && inViewport && _cancel->HitTest(_cancel->ParentToLocal(action.Position)))
				{ Cancel(); _consumeRelease = true; }
				else if (!_pending && inViewport && _editor->HitTest(_editor->ParentToLocal(action.Position)))
				{
					if (Focus) Focus(_editor, true);
					_editor->OnAction(_editor->ParentToLocal(action));
				}
				else { Accept(); _consumeRelease = true; }
			}
			return true;
		}
	private:
		void Accept()
		{
			if (!_active || _pending || _accepting) return;
			auto name = _editor->Text();
			const auto first = name.find_first_not_of(" \t\r\n\v\f");
			name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t\r\n\v\f") - first + 1u);
			if (name == _original) { Cancel(); return; }
			_accepting = true;
			const auto error = Submit ? Submit(name, _id, _revision) : "Rename unavailable";
			_accepting = false;
			if (!error.empty()) { if (Feedback) Feedback(error); return; }
			_pending = true; _editor->SetEnabled(false); _cancel->SetEnabled(false);
			if (Feedback) Feedback("Applying trigger name...");
		}
		std::shared_ptr<Editor> _editor;
		std::shared_ptr<GuiLabel> _label;
		std::shared_ptr<GuiButton> _cancel;
		std::string _id, _original;
		std::uint64_t _revision = 0u;
		bool _active = false, _pending = false, _accepting = false, _consumeRelease = false;
	};
}
