#pragma once

#include "GuiTextBox.h"
#include "GuiButton.h"
#include "../graphics/GlDrawContext.h"
#include <functional>
#include <cstdlib>
#include <optional>

namespace gui
{
	// Window-owned draft. Scene supplies stable identity/revision validation and focus.
	class GuiStationLabel : public base::GuiElement
	{
		class Editor : public GuiTextBox
		{
		public:
			Editor() : GuiTextBox(_Params()) {}
			std::function<void()> Commit;
			std::function<void()> Cancel;
			actions::ActionResult OnAction(actions::KeyAction action) override
			{
				if (action.KeyChar == 0x1bu)
				{
					if (action.KeyActionType == actions::KeyAction::KEY_DOWN && Cancel) Cancel();
					return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
				}
				return GuiTextBox::OnAction(action);
			}
			using GuiTextBox::OnAction;
			void _OnCommit() override { if (Commit) Commit(); }
		private:
			static GuiTextBoxParams _Params()
			{
				auto params = GuiTextBoxParams::PanelInput(LabelWidth);
				params.Texture.clear(); // Keep the same shared fader background while editing.
				return params;
			}
		};
	public:
		static constexpr unsigned int LabelWidth = 200u;
		static constexpr unsigned int LabelHeight = GuiButtonParams::DefaultHeight;
		static constexpr unsigned int ButtonWidth = 76u;
		static constexpr unsigned int ButtonGap = 4u;
		GuiStationLabel() : base::GuiElement(base::GuiElementParams{})
		{
			auto label = GuiLabelParams::PanelHeader("", 80u, 26u);
			label.CenterHorizontally = true; label.Ellipsize = true;
			_label = std::make_shared<GuiLabel>(label);
			base::GuiElementParams background;
			background.TextureShader = "texture"; background.Texture = "fader_back_panel";
			background.TextureOpacity = 0.75f;
			_background = std::make_shared<base::GuiElement>(background);
			_editor = std::make_shared<Editor>();
			auto button = GuiButtonParams::PanelButton(ButtonWidth); button.Text = "Edit";
			_edit = std::make_shared<GuiButton>(button);
			button.Text = "Cancel"; _cancel = std::make_shared<GuiButton>(button);
			_feedback = std::make_shared<GuiLabel>(GuiLabelParams::PanelHeader("", 280u, 22u));
			_feedback->SetPosition({0, static_cast<int>(LabelHeight + 4u)});
			AddChild(_background); AddChild(_label); AddChild(_editor); AddChild(_edit); AddChild(_cancel);
			AddChild(_feedback);
			_editor->Commit = [this]() { Accept(); };
			_editor->Cancel = [this]() { if (!_pending) Cancel(); };
			SetVisible(false);
		}
		std::function<void(const std::shared_ptr<base::GuiElement>&)> Focus;
		std::function<std::string(const std::string&, std::uint64_t)> Submit;
		void Update(const std::string& name, std::uint64_t revision, bool ctrl, bool editable, bool applying)
		{
			if (_labelPress && (!editable || revision != _currentRevision))
			{ _labelPress.reset(); _consumeRelease = true; }
			_editable = editable;
			if (_active && revision != _revision) Cancel();
			if (_pending && !applying)
			{
				_pending = false;
				if (name == _editor->Text()) Cancel();
				else _feedback->SetString("Rename rejected; edit and retry");
			}
			_name = name; _currentRevision = revision;
			_label->SetString(name);
			_label->SetVisible(!_active); _editor->SetVisible(_active);
			_edit->SetVisible(ctrl && editable && !_active);
			_cancel->SetVisible(_active); _cancel->SetEnabled(!_pending);
			_editor->SetEnabled(!_pending);
		}
		void Layout(utils::Position2d origin, unsigned int width)
		{
			origin.X += (static_cast<int>(width) - static_cast<int>(LabelWidth)) / 2;
			width = LabelWidth;
			SetPosition(origin); SetSize({ width + ButtonGap + ButtonWidth, LabelHeight });
			_label->SetSize({width, 26u});
			_label->SetPosition({0, static_cast<int>((LabelHeight - 26u) / 2u)});
			_editor->SetSize({width, LabelHeight});
			_background->SetSize({width, LabelHeight});
			_edit->SetSize({ButtonWidth, LabelHeight}); _cancel->SetSize({ButtonWidth, LabelHeight});
			_edit->SetPosition({static_cast<int>(width + 4u), 0});
			_cancel->SetPosition(_edit->Position());
		}
		bool Active() const { return _active; }
		bool OwnsPointer() const { return _active || _labelPress.has_value() || _consumeRelease; }
		bool OwnsFocus(const std::shared_ptr<base::GuiElement>& focus) const { return focus == _editor; }
		utils::Position2d BinPosition(utils::Position2d preferred) const
		{
			const auto origin = GlobalPosition();
			return {preferred.X, std::max(preferred.Y, origin.Y + static_cast<int>(LabelHeight + 8u))};
		}
		// Scene renders this overlay at full opacity while Ctrl fades the rack.
		void Draw(base::DrawContext&) override {}
		void DrawOverlay(base::DrawContext& ctx)
		{
			for (auto ancestor = Parent(); ancestor; ancestor = ancestor->Parent())
				if (!ancestor->IsVisible()) return;
			auto& gl = dynamic_cast<graphics::GlDrawContext&>(ctx);
			const auto global = GlobalPosition(), local = Position();
			gl.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(global.X - local.X, global.Y - local.Y, 0.0f)));
			base::GuiElement::Draw(ctx);
			gl.PopMvp();
		}
		bool Move(actions::TouchMoveAction action)
		{
			if (_labelPress)
			{
				_labelPress->Moved |= _MovedFromPress(action.Position);
				if (action.Origin == actions::TouchMoveAction::Provenance::CaptureCancellation)
				{ _labelPress.reset(); _consumeRelease = true; }
			}
			if (!_isVisible) return false;
			for (auto ancestor = Parent(); ancestor; ancestor = ancestor->Parent())
				if (!ancestor->IsVisible()) return false;
			bool over = false;
			for (const auto& button : {_edit, _cancel})
			{
				button->OnAction(button->ParentToLocal(action));
				if (button->IsVisible() && button->HitTest(button->ParentToLocal(action.Position))) over = true;
			}
			return over;
		}
		bool Touch(actions::TouchAction action)
		{
			if (_consumeRelease && action.State == actions::TouchAction::TOUCH_UP)
			{ _consumeRelease = false; return true; }
			if (!_isVisible) return false;
			for (auto ancestor = Parent(); ancestor; ancestor = ancestor->Parent())
				if (!ancestor->IsVisible()) return false;
			const auto hit = [&](const auto& control) { return control->IsVisible() && control->HitTest(control->ParentToLocal(action.Position)); };
			if (_labelPress)
			{
				_labelPress->Moved |= _MovedFromPress(action.Position);
				if (action.State == actions::TouchAction::TOUCH_UP &&
					action.Index == _labelPress->Index && action.Touch == _labelPress->Source)
				{
					const bool edit = !_labelPress->Moved && _editable && hit(_background);
					_labelPress.reset();
					if (edit) _BeginEditing(false);
				}
				return true;
			}
			if (_active)
			{
				if (action.State == actions::TouchAction::TOUCH_DOWN)
				{
					if (!_pending && hit(_cancel)) { Cancel(); _consumeRelease = true; }
					else if (hit(_editor))
					{
						if (Focus) Focus(_editor);
						_editor->OnAction(_editor->ParentToLocal(action));
					}
					else { Accept(); _consumeRelease = true; }
				}
				return true;
			}
			if (hit(_edit))
			{
				if (action.State == actions::TouchAction::TOUCH_DOWN)
				{
					_BeginEditing(true);
				}
				return true;
			}
			if (_editable && hit(_background) && action.State == actions::TouchAction::TOUCH_DOWN &&
				(action.Touch == actions::TouchAction::TOUCH_FINGER || action.Index == 0))
			{
				_labelPress = LabelPress{action.Position, action.Touch, action.Index};
				return true;
			}
			return false;
		}
		void Cancel()
		{
			if (_labelPress) { _labelPress.reset(); _consumeRelease = true; }
			const bool wasActive = _active;
			_active = false; _pending = false;
			if (wasActive && Focus) Focus(nullptr); // Disarm before FinalizeEdits.
			_editor->SetVisible(false); _cancel->SetVisible(false); _label->SetVisible(true);
			_feedback->SetString("");
		}
		void Accept()
		{
			if (!_active || _pending) return;
			auto name = _editor->Text();
			const auto first = name.find_first_not_of(" \t");
			name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t") - first + 1u);
			if (name == _name) { Cancel(); return; }
			const auto error = Submit ? Submit(name, _revision) : "Unavailable";
			if (error.empty()) { _editor->SetText(name); _pending = true; _editor->SetEnabled(false); }
			else _feedback->SetString(error);
		}
	private:
		struct LabelPress
		{
			utils::Position2d Position;
			actions::TouchAction::TouchType Source;
			int Index;
			bool Moved = false;
		};
		bool _MovedFromPress(utils::Position2d position) const
		{
			return std::abs(position.X - _labelPress->Position.X) > 4 ||
				std::abs(position.Y - _labelPress->Position.Y) > 4;
		}
		void _BeginEditing(bool consumeRelease)
		{
			_active = true; _revision = _currentRevision; _editor->SetText(_name);
			_editor->SetVisible(true); _label->SetVisible(false); _edit->SetVisible(false); _cancel->SetVisible(true);
			if (Focus) Focus(_editor);
			_editor->BeginEditing(); _consumeRelease = consumeRelease;
		}
		std::optional<LabelPress> _labelPress;
		std::shared_ptr<GuiLabel> _label;
		std::shared_ptr<base::GuiElement> _background;
		std::shared_ptr<GuiLabel> _feedback;
		std::shared_ptr<Editor> _editor;
		std::shared_ptr<GuiButton> _edit, _cancel;
		std::string _name;
		std::uint64_t _revision = 0u, _currentRevision = 0u;
		bool _active = false, _pending = false, _consumeRelease = false, _editable = false;
	};
}
