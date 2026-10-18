#pragma once

#include "GuiPanel.h"
#include "GuiTextBox.h"
#include "GuiButton.h"
#include <functional>

namespace gui
{
	// UI-owned draft only. The HUD validates identity/revision and submits the rig.
	class GuiTriggerRename : public GuiPanel
	{
		class Editor : public GuiTextBox
		{
		public:
			Editor() : GuiTextBox(GuiTextBoxParams::PanelInput(360u)) {}
			std::function<void()> Accept;
			using GuiTextBox::OnAction;
			actions::ActionResult OnAction(actions::KeyAction action) override
			{
				if (action.KeyChar == 13u)
				{
					if (action.KeyActionType == actions::KeyAction::KEY_DOWN && Accept) Accept();
					return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
				}
				return GuiTextBox::OnAction(action);
			}
			void _OnCommit() override {} // Blur finalizes draft; only explicit Enter/OK accepts.
		};
	public:
		GuiTriggerRename() : GuiPanel(_Params())
		{
			_title = std::make_shared<GuiLabel>(GuiLabelParams::PanelHeader("Rename trigger", 360u));
			_editor = std::make_shared<Editor>();
			auto errorParams = GuiLabelParams::PanelScrollRow("", 360u); errorParams.ClipText = true;
			_error = std::make_shared<GuiLabel>(errorParams);
			auto params = GuiButtonParams::PanelButton(76u); params.Text = "OK";
			_ok = std::make_shared<GuiButton>(params);
			params.Text = "Cancel"; _cancel = std::make_shared<GuiButton>(params);
			AddChild(_title); AddChild(_editor); AddChild(_error); AddChild(_ok); AddChild(_cancel);
			_editor->Accept = [this]() { Accept(); };
		}
		// Request/clear focus through Scene; clear must only affect this editor.
		std::function<void(const std::shared_ptr<base::GuiElement>&, bool)> Focus;
		std::function<std::string(const std::string&, const std::string&, std::uint64_t)> Submit;
		std::function<void()> Close;
		bool CanDismiss() const { return !_pending; }
		void Begin(const std::string& id, const std::string& name, std::uint64_t revision)
		{
			_id = id; _original = name; _revision = revision; _active = true; _pending = false;
			_editor->SetText(name); _editor->SetEnabled(true); _error->SetString(""); _ok->SetEnabled(true); _cancel->SetEnabled(true);
			if (Focus) Focus(_editor, true);
			_editor->BeginEditing();
		}
		void Dismissed()
		{
			_active = false; _pending = false; _pressed.reset();
			if (Focus) Focus(_editor, false);
		}
		void Update(bool applying)
		{
			if (_active && _pending && !applying)
			{
				_pending = false; _editor->SetEnabled(true); _ok->SetEnabled(true); _cancel->SetEnabled(true);
				_error->SetString("Rename rejected; edit and retry");
			}
		}
		void FitToViewport(utils::Size2d viewport)
		{
			const auto width = std::min(400u, viewport.Width), height = std::min(184u, viewport.Height);
			SetSize({width, height}); SetPosition({static_cast<int>((viewport.Width - width) / 2u), static_cast<int>((viewport.Height - height) / 2u)});
			const auto inset = std::min(12u, width / 4u), content = width - 2u * inset;
			const auto y = [&](unsigned int top) { return std::max(0, static_cast<int>(height) - static_cast<int>(top)); };
			_title->SetPosition({static_cast<int>(inset), y(36u)}); _title->SetSize({content, 24u});
			_editor->SetPosition({static_cast<int>(inset), y(80u)}); _editor->SetSize({content, 36u});
			_error->SetPosition({static_cast<int>(inset), y(112u)}); _error->SetSize({content, 24u});
			const auto buttonWidth = std::min(76u, content / 2u);
			_ok->SetSize({buttonWidth, 36u}); _cancel->SetSize({buttonWidth, 36u});
			_ok->SetPosition({static_cast<int>(inset), 12});
			_cancel->SetPosition({static_cast<int>(width - inset - buttonWidth), 12});
		}
		using GuiPanel::OnAction;
		actions::ActionResult OnAction(actions::KeyAction action) override
		{
			if (_active && !_pending) _editor->OnAction(action);
			return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
		}
		actions::ActionResult OnAction(actions::TouchAction action) override
		{
			_ok->OnAction(_ok->ParentToLocal(action));
			_cancel->OnAction(_cancel->ParentToLocal(action));
			const auto hit = [&](const auto& item) { return item->HitTest(item->ParentToLocal(action.Position)); };
			if (action.State == actions::TouchAction::TOUCH_DOWN)
			{
				if (!_pending && hit(_cancel)) _pressed = _cancel;
				else if (!_pending && hit(_ok)) _pressed = _ok;
				else if (!_pending && hit(_editor)) { if (Focus) Focus(_editor, true); _editor->OnAction(_editor->ParentToLocal(action)); }
			}
			else
			{
				const auto pressed = _pressed; _pressed.reset();
				if (!_pending && pressed && hit(pressed)) { if (pressed == _cancel) { if (Close) Close(); } else Accept(); }
			}
			return {true, {}, {}, actions::ACTIONRESULT_DEFAULT, nullptr, {}};
		}
		void ClearPointerState() override { _pressed.reset(); GuiPanel::ClearPointerState(); }
	private:
		void Accept()
		{
			if (!_active || _pending) return;
			auto name = _editor->Text();
			const auto first = name.find_first_not_of(" \t\r\n\v\f");
			name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t\r\n\v\f") - first + 1u);
			if (name == _original) { if (Close) Close(); return; }
			const auto error = Submit ? Submit(name, _id, _revision) : "Rename unavailable";
			if (!error.empty()) { _error->SetString(error); return; }
			_pending = true; _editor->SetEnabled(false); _ok->SetEnabled(false); _cancel->SetEnabled(false); _error->SetString("Applying...");
		}
		static base::GuiElementParams _Params()
		{
			base::GuiElementParams params;
			params.Texture = "rounded_but_on"; params.TextureShader = "texture_tinted";
			params.TintColor = GuiStyle::Graphite(); params.GuiPassThrough = false;
			return params;
		}
		std::shared_ptr<Editor> _editor;
		std::shared_ptr<GuiLabel> _title, _error;
		std::shared_ptr<GuiButton> _ok, _cancel;
		std::shared_ptr<base::GuiElement> _pressed;
		std::string _id, _original;
		std::uint64_t _revision = 0u;
		bool _active = false, _pending = false;
	};
}
