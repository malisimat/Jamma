#pragma once

#include <memory>
#include <string>
#include <optional>
#include "GuiElement.h"
#include "GuiLabel.h"
#include "../graphics/Font.h"
#include "../actions/KeyAction.h"
#include "../actions/TouchAction.h"

namespace gui
{
	struct GuiTextBoxParams : public base::GuiElementParams
	{
		GuiTextBoxParams()
		{
			GuiPassThrough = false;
		}

		std::string  Text;
		unsigned int Padding     = 4u;   // inner padding in pixels.
		std::string  CaretTexture = "blue"; // solid colour used for caret/selection.
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	// Single-line, focus-aware text entry control.
	//
	// The editing core (caret, selection, insert/delete/navigation) is plain CPU
	// state so it is fully testable without a GL context.  Rendering reuses a
	// child GuiLabel for the glyphs and draws a caret/selection bar on top.
	//
	// Keyboard input arrives as Windows virtual-key codes (see Window.cpp), so
	// printable characters are reconstructed from VK + Shift.  Scope is ASCII
	// printable; this is not a full Unicode editor.
	class GuiTextBox : public base::GuiElement
	{
	public:
		explicit GuiTextBox(GuiTextBoxParams params);

	public:
		const std::string& Text() const;
		void SetText(const std::string& text, bool notify = false);

		// Editing-state accessors (testable without GL).
		unsigned int CaretIndex() const;
		bool HasSelection() const;
		unsigned int SelectionStart() const;
		unsigned int SelectionLength() const;
		std::string SelectedText() const;

		virtual bool IsTextEditing() const override;
		virtual bool WantsFocusOnPress() const override;

		using base::GuiElement::OnAction;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

		// Map a Windows virtual-key code (+ shift) to a printable ASCII char.
		static std::optional<char> VkToChar(unsigned int vk, bool shift);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

		// Extension hooks for derived controls (e.g. numeric input).
		virtual bool _AcceptChar(char c) const;
		virtual void _OnTextChanged();
		virtual void _OnCommit();

		// Editing primitives operating on _text / _caret / _anchor.
		void _InsertChar(char c);
		void _Backspace();
		void _Delete();
		void _MoveCaret(int delta, bool select);
		void _SetCaret(unsigned int index, bool select);
		bool _DeleteSelection();
		unsigned int _CaretFromLocalX(int localX) const;
		void _SyncLabel();
		void _NotifyReceiver(bool commit);

		static base::GuiElementParams _MakeCaretParams(const std::string& texture);
		static GuiLabelParams _MakeLabelParams(const GuiTextBoxParams& params);

	protected:
		std::string _text;
		unsigned int _caret;
		unsigned int _anchor;
		unsigned int _padding;
		bool _editing;
		std::shared_ptr<GuiLabel> _label;
		base::GuiElement _caretQuad;
		std::weak_ptr<graphics::Font> _font;
		std::weak_ptr<base::ActionReceiver> _receiver;
	};
}
