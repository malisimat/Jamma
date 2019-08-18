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
		GuiElementParams(DrawableParams drawParams,
			MoveableParams moveParams,
			SizeableParams sizeParams,
			std::string overTexture,
			std::string downTexture,
			std::string outTexture,
			std::vector<GuiElementParams> childParams) :
			DrawableParams(drawParams),
			MoveableParams(moveParams),
			SizeableParams(sizeParams),
			OverTexture(overTexture),
			DownTexture(downTexture),
			OutTexture(outTexture),
			ChildParams(childParams)
		{
		}

	public:
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
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(DrawContext& ctx) override;
		virtual void Draw3d(DrawContext& ctx) override;
		virtual bool HitTest(utils::Position2d localPos);

		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

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
		virtual bool _InitResources(resources::ResourceLib& resourceLib) override;
		virtual bool _ReleaseResources() override;

	protected:
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
