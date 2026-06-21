#pragma once

#include <atomic>
#include <cstdint>
#include <tuple>
#include <vector>
#include "CommonTypes.h"
#include "Drawable.h"
#include "Sizeable.h"
#include "Moveable.h"
#include "../graphics/GlDrawContext.h"
#include "../graphics/Image.h"
#include "ActionSender.h"
#include "ActionReceiver.h"

namespace base
{
	// How a layout container should size this element in one axis.
	enum class LayoutSizing : std::uint8_t
	{
		Fixed,  // Use the element's explicit Size; the container does not resize this axis.
		Auto,   // Let the container size this axis to ContentSize().
		Fill    // Stretch to fill remaining space in the container.
	};

	// Horizontal alignment of an element within a layout cell.
	enum class LayoutHAlign : std::uint8_t
	{
		Left,
		Center,
		Right,
		Fill    // Stretch to fill cell width.
	};

	// Vertical alignment of an element within a layout cell.
	enum class LayoutVAlign : std::uint8_t
	{
		Top,
		Center,
		Bottom,
		Fill    // Stretch to fill cell height.
	};

	class GuiElement;

	class GuiElementParams :
		public DrawableParams,
		public MoveableParams,
		public SizeableParams
	{
	public:
		GuiElementParams() :
			DrawableParams{ "" },
			MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
			SizeableParams{ 1,1 },
			Rot90(false),
			FlipH(false),
			FlipV(false),
			GuiPassThrough(true),
			Index(0u),
			OverTexture(""),
			DownTexture(""),
			OutTexture("")
		{
		}

		GuiElementParams(unsigned int index,
			DrawableParams drawParams,
			MoveableParams moveParams,
			SizeableParams sizeParams,
			std::string overTexture,
			std::string downTexture,
			std::string outTexture,
			std::vector<GuiElementParams> childParams) :
			DrawableParams(drawParams),
			MoveableParams(moveParams),
			SizeableParams(sizeParams),
			Rot90(false),
			FlipH(false),
			FlipV(false),
			GuiPassThrough(true),
			Index(index),
			OverTexture(overTexture),
			DownTexture(downTexture),
			OutTexture(outTexture)
		{
		}

	public:
		bool Rot90;
		bool FlipH;
		bool FlipV;
		bool GuiPassThrough;
		unsigned int Index;
		std::string OverTexture;
		std::string DownTexture;
		std::string OutTexture;
		std::string TextureShader = "texture";
		LayoutSizing HorizSizing = LayoutSizing::Fixed;
		LayoutSizing VertSizing  = LayoutSizing::Fixed;
	};

	class GuiElement :
		public Drawable, 
		public Sizeable, 
		public Moveable,
		public ActionSender,
		public ActionReceiver
	{
	public:
		GuiElement(GuiElementParams params);

	public:
		enum GuiElementState
		{
			STATE_NORMAL,
			STATE_OVER,
			STATE_DOWN,
			STATE_OUT
		};

		enum EditMode
		{
			EDIT_SELECT,
			EDIT_MUTE
		};

		enum class GestureKind : std::uint8_t
		{
			None,
			MidiQuantisation
		};

		struct GestureState
		{
			GestureKind Kind = GestureKind::None;
			bool Active = false;
			bool Moved = false;
			utils::Position2d StartPosition = { 0, 0 };
			int StartValue = 0;
		};

	public:
		virtual ActionDirection Direction() const override { return ACTIONDIR_DUPLEX; }
		virtual void Init();
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, DrawPass pass) override;
		virtual void SetVisible(bool visible);
		virtual void SetEnabled(bool enabled);
		virtual bool Select();
		virtual bool DeSelect();
		virtual void SetPicking3d(bool picking);
		virtual void SetPickingFromState(EditMode mode, bool flipState);
		virtual void SetStateFromPicking(EditMode mode, bool flipState);
		virtual void SetIndex(unsigned int index);
		unsigned int Index() const;
		virtual std::vector<unsigned int> GlobalId();
		virtual void AddChild(std::shared_ptr<GuiElement> child);
		virtual std::shared_ptr<GuiElement> TryGetChild(unsigned char index);

		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

		virtual utils::Size2d ContentSize() const;
		LayoutSizing GetHorizSizing() const;
		LayoutSizing GetVertSizing() const;

		bool IsVisible() const;
		bool IsEnabled() const;
		bool IsSelected() const;
		GuiElementState GetState() const;
		bool HasFocus() const;
		virtual bool RequestFocus();
		virtual void ClearFocus();
		// True if this element should take keyboard focus when pressed.  Only
		// editing-style controls (text boxes, dropdowns) opt in.
		virtual bool WantsFocusOnPress() const;
		// True while this element owns the keyboard for text entry; lets the scene
		// suppress global shortcuts (Space/Ctrl+Z/Ctrl+S) during editing.
		virtual bool IsTextEditing() const;
		// Top-left position of this element in global (scene overlay) coordinates.
		utils::Position2d GlobalPosition() const;
		bool HitTest(utils::Position2d localPos);
		void _ApplyTextureTint(graphics::GlDrawContext& ctx) const;
		std::vector<actions::JobAction> CommitChanges();
		void SetParent(std::shared_ptr<GuiElement> parent);
		actions::TouchAction GlobalToLocal(actions::TouchAction action);
		actions::TouchAction ParentToLocal(actions::TouchAction action);
		actions::TouchMoveAction GlobalToLocal(actions::TouchMoveAction action);
		actions::TouchMoveAction ParentToLocal(actions::TouchMoveAction action);
		utils::Position2d GlobalToLocal(utils::Position2d pos);
		utils::Position2d ParentToLocal(utils::Position2d pos);

		std::shared_ptr<GuiElement> shared_from_this()
		{
			auto self = Actionable::shared_from_this();
			return self ? std::dynamic_pointer_cast<GuiElement>(self) : nullptr;
		}

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual std::vector<actions::JobAction> _CommitChanges();
		virtual bool _HitTest(utils::Position2d localPos);
		void _BeginGesture(GestureKind kind, utils::Position2d startPosition, int startValue = 0) noexcept;
		void _MarkGestureMoved() noexcept;
		void _EndGesture() noexcept;
		bool _IsGestureActive(GestureKind kind) const noexcept;
		const GestureState& _GestureState() const noexcept { return _gestureState; }

	protected:
		std::atomic<bool> _changesMade;
		bool _isVisible;
		bool _isEnabled;
		bool _isSelected;
		bool _isPicking3d;
		bool _hasFocus;
		unsigned int _index;
		GuiElementParams _guiParams;
		GuiElementState _state;
		graphics::Image _texture;
		graphics::Image _overTexture;
		graphics::Image _downTexture;
		graphics::Image _outTexture;
		GestureState _gestureState;
		std::shared_ptr<GuiElement> _parent;
		std::vector<std::shared_ptr<GuiElement>> _children;
	};
}
