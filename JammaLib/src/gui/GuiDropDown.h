#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "GuiElement.h"
#include "GuiLabel.h"
#include "GuiPopupHost.h"
#include "../actions/KeyAction.h"
#include "../actions/TouchAction.h"

namespace gui
{
	// Popup list rendered while a dropdown is open.  Owns one label per item and
	// reports a selection via a callback when an item is clicked or chosen with
	// the keyboard (Up/Down to move the highlight, Enter to choose).
	class GuiDropDownList : public base::GuiElement
	{
	public:
		GuiDropDownList(base::GuiElementParams params,
			std::vector<std::string> items,
			unsigned int rowHeight,
			unsigned int padding,
			const std::string& highlightTexture);

		void SetOnSelect(std::function<void(int)> onSelect);
		void SetHighlight(int index);
		int Highlight() const;

		using base::GuiElement::OnAction;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

	private:
		int _RowFromLocalY(int localY) const;

		static base::GuiElementParams _MakeQuadParams(const std::string& texture);
		static std::shared_ptr<GuiLabel> _MakeRowLabel(const std::string& text, int y, unsigned int width, unsigned int rowHeight, unsigned int padding);

		std::vector<std::string>          _items;
		std::vector<std::shared_ptr<GuiLabel>> _rowLabels;
		unsigned int                      _rowHeight;
		int                               _highlight;
		base::GuiElement                  _highlightQuad;
		std::function<void(int)>          _onSelect;
	};

	struct GuiDropDownParams : public base::GuiElementParams
	{
		GuiDropDownParams()
		{
			GuiPassThrough = false;
		}

		std::vector<std::string> Items;
		unsigned int             InitIndex        = 0u;
		unsigned int             RowHeight        = 24u;
		unsigned int             Padding          = 4u;
		std::string              ListTexture      = "rounded_but";
		std::string              HighlightTexture = "blue";
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	// Combo box.  Closed it behaves like a focusable button showing the selected
	// item; pressing it (or Enter/Space/Down when focused) opens a popup list
	// through the scene popup host.
	class GuiDropDown : public base::GuiElement
	{
	public:
		explicit GuiDropDown(GuiDropDownParams params);

	public:
		void SetPopupHost(GuiPopupHost* host);
		int SelectedIndex() const;
		std::string SelectedText() const;
		void SetSelectedIndex(int index, bool notify = false);
		bool IsOpen() const;
		void Open();
		void Close();

		virtual bool WantsFocusOnPress() const override;
		virtual void SetSize(utils::Size2d size) override;

		using base::GuiElement::OnAction;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

	private:
		void _Select(int index, bool notify);
		void _SyncLabel();

		static std::shared_ptr<GuiLabel> _MakeClosedLabel(const GuiDropDownParams& params);
		static std::shared_ptr<GuiDropDownList> _MakeList(const GuiDropDownParams& params);

		std::vector<std::string>           _items;
		int                                _selectedIndex;
		unsigned int                       _rowHeight;
		std::shared_ptr<GuiLabel>          _label;
		std::shared_ptr<GuiDropDownList>   _list;
		GuiPopupHost*                      _popupHost;
		bool                               _open;
		unsigned int                       _padding;
		std::weak_ptr<base::ActionReceiver> _receiver;
	};
}
