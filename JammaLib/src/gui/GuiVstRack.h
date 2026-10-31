#pragma once

#include "GuiButton.h"
#include "GuiToggle.h"
#include "GuiScrollPanel.h"
#include "GuiLabel.h"
#include "CableInteraction.h"
#include "../vst/VstRackSlot.h"
#include "../vst/RackMove.h"
#include "../midi/MidiChannelRoute.h"
#include <array>
#include <functional>

namespace resources { class ShaderResource; }

namespace gui
{
	// Window-owned presentation and gestures only. Commands are applied by
	// VstRackControls after Scene membership/revision validation.
	class GuiVstRack : public base::GuiElement
	{
	public:
		using Routes = std::array<midi::MidiChannelRoute, 16>;
		struct Command
		{
			enum class Kind { Slot, Append, Recover, ClearInactiveRoutes, Diagnostic, Routes, Move };
			Kind Operation = Kind::Slot;
			std::string Identity;
			std::size_t Ordinal = 0u;
			std::uint64_t Revision = 0u;
			base::Action::Modifiers Modifiers = base::Action::MODIFIER_NONE;
			GuiVstRack::Routes ChannelRoutes{};
			vst::RackMove Move;
		};
		struct Slot
		{
			vst::VstRackSlot Value;
			bool EditorOpen = false;
		};
		struct Model
		{
			std::string OwnerName;
			std::string OwnerId;
			vst::RackMove::Scope OwnerScope = vst::RackMove::Scope::Station;
			std::vector<Slot> Slots;
			std::size_t TotalSlots = 0u;
			std::uint64_t Revision = 0u;
			bool Editable = false;
			bool Inactive = false;
			bool Suspended = false;
			std::string Diagnostic;
			Routes ChannelRoutes{}; // Never populated from unbounded inactive imports.
		};

		GuiVstRack(std::shared_ptr<base::GuiElement> channels = {},
			const std::vector<std::shared_ptr<GuiToggle>>& toggles = {});
		~GuiVstRack();
		using base::GuiElement::OnAction;
		void Update(Model model, std::function<void(Command)> submit);
		void Layout(unsigned int width, int faderBottom,
			utils::Position2d expanderPosition, utils::Size2d expanderSize);
		void SetViewport(utils::Size2d viewport);
		void SetFeedback(std::string message);
		const std::string& Feedback() const noexcept { return _feedback; }
		void SetRevealHeld(bool held) { _revealHeld = held; }
		void SetChannelsCollapsed(bool collapsed);
		void SetAllowedMidiChannels(const std::vector<int>& channels);
		bool ChannelsCollapsed() const noexcept { return _collapsed; }
		bool ChannelShownActive(unsigned int channel) const noexcept;
		std::size_t PageOffset() const noexcept { return _pageOffset; }
		std::shared_ptr<GuiButton> AppendButton() const;
		std::shared_ptr<GuiToggle> ChannelsExpander() const;
		std::shared_ptr<GuiButton> SlotButton(std::size_t index) const;
		bool HasSlotMove() const noexcept { return _slotMove.has_value(); }
		std::shared_ptr<GuiScrollPanel> SlotScroll() const { return _scroll; }
		bool HasCableDrag() const noexcept { return _drag.has_value(); }
		void CancelInteraction();
		bool IsInView(utils::Size2d viewport) const;
		void MovePointer(actions::TouchMoveAction action);
		void BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
			std::vector<CableInteraction::Cable>& cables) const;
		actions::ActionResult OnAction(actions::TouchAction action) override;
		actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		actions::ActionResult OnAction(actions::KeyAction action) override;
		bool RouteHitTest(utils::Position2d point) override;
		std::shared_ptr<base::GuiElement> FindTopmostDescendant(utils::Position2d point) override;
		void ClearPointerState() override;
		void SetVisible(bool visible) override;
		void Draw(base::DrawContext& context) override;
	protected:
		void _InitResources(resources::ResourceLib& resources, bool force) override;
		void _ReleaseResources() override;
	private:
		class Button;
		class Socket;
		std::optional<unsigned int> _hoveredMidiSocket;
		std::optional<std::size_t> _hoveredSlot;
		class Expander;
		struct Row
		{
			std::string Id;
			std::size_t Ordinal = 0u;
			bool Missing = false;
			bool Eligible = false;
			std::array<std::shared_ptr<Button>, 4> Buttons;
			std::shared_ptr<base::GuiElement> Socket;
		};
		struct Link { unsigned int Channel; std::string Id; };
		struct Drag
		{
			std::uint64_t Revision;
			bool MoveSource;
			std::optional<Link> Original;
			unsigned int Channel;
			std::string Id;
			utils::Position2d Pointer;
			std::optional<CableInteraction::Endpoint> Snap;
		};
		struct SlotMove
		{
			vst::RackMove Move;
			std::optional<std::size_t> Target;
			std::shared_ptr<Button> SourceButton;
			utils::Position2d Start, Pointer, Offset;
			bool Dragging = false;
			int Index;
			actions::TouchAction::TouchType Touch;
		};
		struct ChannelPress
		{
			unsigned int Channel;
			utils::Position2d Position;
			int Index;
			actions::TouchAction::TouchType Touch;
		};
		static constexpr unsigned int _RowHeight = 36u;
		static constexpr unsigned int _RowPitch = 40u;
		static constexpr unsigned int _PageSize = 32u;
		std::shared_ptr<Button> _MakeButton(std::string text, std::string texture,
			std::function<Command()> capture,
			graphics::FontOptions::FontSize fontSize = graphics::FontOptions::FONT_COMPACT);
		void _RebuildRows();
		void _LayoutRows();
		void _Geometry(std::vector<CableInteraction::Endpoint>& endpoints,
			std::vector<CableInteraction::Cable>& cables, std::vector<Link>& links) const;
		bool _BeginDrag(utils::Position2d point);
		bool _BeginSlotMove(const actions::TouchAction& action);
		void _UpdateSlotMove(utils::Position2d point);
		void _FinishSlotMove(const actions::TouchAction& action);
		void _DrawSlotMove(base::DrawContext& context);
		void _UpdateDrag(utils::Position2d point);
		void _FinishDrag();
		void _RemoveLink(Routes& routes, const Link& link) const;
		void _DrawCables(base::DrawContext& context);
		void _RefreshChannelToggles();
		actions::ActionResult _Captured();
		Model _model;
		std::function<void(Command)> _submit;
		std::shared_ptr<base::GuiElement> _channels;
		std::vector<std::shared_ptr<GuiToggle>> _toggles;
		std::vector<std::shared_ptr<base::GuiElement>> _sourceSockets;
		std::shared_ptr<GuiScrollPanel> _scroll;
		std::shared_ptr<GuiPanel> _content;
		std::shared_ptr<Button> _append, _recover, _clearRoutes, _diagnostic, _previous, _next;
		std::shared_ptr<Expander> _collapse;
		std::shared_ptr<GuiLabel> _midiLabel;
		std::array<bool, 16> _allowedChannels{};
		std::vector<Row> _rows;
		std::size_t _pageOffset = 0u;
		unsigned int _width = 64u;
		int _faderBottom = 40;
		utils::Size2d _viewport{1920u, 1080u};
		bool _collapsed = true, _revealHeld = false, _hovered = false, _consumeRelease = false, _consumeEscape = false, _rowsBuilt = false;
		std::optional<Drag> _drag;
		std::optional<SlotMove> _slotMove;
		std::optional<ChannelPress> _channelPress;
		std::string _feedback;
		std::shared_ptr<int> _lifetime = std::make_shared<int>(0);
		std::weak_ptr<resources::ShaderResource> _cableShader;
		unsigned int _vertexArray = 0u, _vertexBuffer = 0u;
	};
}
