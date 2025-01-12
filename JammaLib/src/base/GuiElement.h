#pragma once

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
	class GuiElement;

	class GuiElementParams :
		public DrawableParams,
		public MoveableParams,
		public SizeableParams
	{
	public:
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
			Index(index),
			OverTexture(overTexture),
			DownTexture(downTexture),
			OutTexture(outTexture),
			ChildParams(childParams)
		{
		}

	public:
		unsigned int Index;
		std::string OverTexture;
		std::string DownTexture;
		std::string OutTexture;
		std::vector<GuiElementParams> ChildParams;
	};

	class GuiElement :
		public Drawable, 
		public Sizeable, 
		public Moveable,
		public virtual ActionSender,
		public virtual ActionReceiver
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

	public:
		virtual ActionDirection Direction() const override { return ACTIONDIR_DUPLEX; }
		virtual void Init();
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(DrawContext& ctx) override;
		virtual void Draw3d(DrawContext& ctx, unsigned int numInstances) override;
		virtual bool HitTest(utils::Position2d localPos);
		virtual void SetSelected(bool selected);
		virtual void SetPicking3d(bool picking);
		virtual void SetIndex(unsigned int index);
		virtual std::vector<unsigned int> GlobalId();
		virtual std::shared_ptr<GuiElement> TryGetChild(unsigned char index);

		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

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
			return std::dynamic_pointer_cast<GuiElement>(
				Actionable::shared_from_this());
		}

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual std::vector<actions::JobAction> _CommitChanges();

	protected:
		bool _changesMade;
		bool _isSelected;
		bool _isPicking3d;
		unsigned int _index;
		GuiElementParams _guiParams;
		GuiElementState _state;
		graphics::Image _texture;
		graphics::Image _overTexture;
		graphics::Image _downTexture;
		graphics::Image _outTexture;
		std::shared_ptr<GuiElement> _parent;
		std::vector<std::shared_ptr<GuiElement>> _children;
	};
}
