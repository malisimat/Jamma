#include "GuiElement.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include <typeinfo>

using namespace base;
using namespace utils;
using namespace actions;
using graphics::GlDrawContext;
using graphics::ImageParams;
using resources::ResourceLib;

GuiElement::GuiElement(GuiElementParams params) :
	Drawable(static_cast<DrawableParams&>(params)),
	Moveable(static_cast<MoveableParams&>(params)),
	Sizeable(static_cast<SizeableParams&>(params)),
	_changesMade(false),
	_isVisible(true),
	_isEnabled(true),
	_isSelected(false),
	_isPicking3d(false),
	_hasFocus(false),
	_index(params.Index),
	_guiParams(params),
	_state(STATE_NORMAL),
	_texture(ImageParams(DrawableParams{ params.Texture }, SizeableParams{ params.Size,params.MinSize }, "texture", params.Rot90, params.FlipH, params.FlipV)),
	_overTexture(ImageParams(DrawableParams{ params.OverTexture }, SizeableParams{ params.Size,params.MinSize }, "texture", params.Rot90, params.FlipH, params.FlipV)),
	_downTexture(ImageParams(DrawableParams{ params.DownTexture }, SizeableParams{ params.Size,params.MinSize }, "texture", params.Rot90, params.FlipH, params.FlipV)),
	_outTexture(ImageParams(DrawableParams{ params.OutTexture }, SizeableParams{ params.Size,params.MinSize }, "texture", params.Rot90, params.FlipH, params.FlipV)),
	_gestureState(),
	_children({})
{
}

void GuiElement::_BeginGesture(GestureKind kind, Position2d startPosition, int startValue) noexcept
{
	_gestureState.Kind = kind;
	_gestureState.Active = true;
	_gestureState.Moved = false;
	_gestureState.StartPosition = startPosition;
	_gestureState.StartValue = startValue;
}

void GuiElement::_MarkGestureMoved() noexcept
{
	_gestureState.Moved = true;
}

void GuiElement::_EndGesture() noexcept
{
	_gestureState.Kind = GestureKind::None;
	_gestureState.Active = false;
	_gestureState.Moved = false;
	_gestureState.StartPosition = { 0, 0 };
	_gestureState.StartValue = 0;
}

bool GuiElement::_IsGestureActive(GestureKind kind) const noexcept
{
	return _gestureState.Active && _gestureState.Kind == kind;
}

void GuiElement::Init()
{
	_InitReceivers();

	unsigned int index = 0;
	for (auto& child : _children)
	{
		child->SetParent(GuiElement::shared_from_this());
		child->Init();
		child->SetIndex(index);
		index++;
	}
}

void GuiElement::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);

	// Iterate a snapshot in case child initialisation mutates _children.
	auto children = _children;
	for (auto& child : children)
	{
		if (child)
			child->InitResources(resourceLib, forceInit);
	}
}

void GuiElement::SetSize(Size2d size)
{
	Sizeable::SetSize(size);

	_texture.SetSize(_sizeParams.Size);
	_overTexture.SetSize(_sizeParams.Size);
	_downTexture.SetSize(_sizeParams.Size);
	_outTexture.SetSize(_sizeParams.Size);
}

utils::Size2d GuiElement::ContentSize() const
{
	return GetSize();
}

LayoutSizing GuiElement::GetHorizSizing() const
{
	return _guiParams.HorizSizing;
}

LayoutSizing GuiElement::GetVertSizing() const
{
	return _guiParams.VertSizing;
}

bool GuiElement::IsSelected() const
{
	return _isSelected;
}

GuiElement::GuiElementState GuiElement::GetState() const
{
	return _state;
}

bool GuiElement::HasFocus() const
{
	return _hasFocus;
}

bool GuiElement::RequestFocus()
{
	_hasFocus = true;
	return true;
}

void GuiElement::ClearFocus()
{
	_hasFocus = false;
}

bool GuiElement::IsVisible() const
{
	return _isVisible;
}

bool GuiElement::IsEnabled() const
{
	return _isEnabled;
}

bool GuiElement::HitTest(Position2d localPos)
{
	for (auto& child : _children)
	{
		if (child->HitTest(child->ParentToLocal(localPos)))
			return true;
	}

	return _HitTest(localPos);
}

std::vector<JobAction> GuiElement::CommitChanges()
{
	std::vector<JobAction> jobList = {};
	if (_changesMade)
	{
		auto jobs = _CommitChanges();
		if (!jobs.empty())
			jobList.insert(jobList.end(), jobs.begin(), jobs.end());
	}

	_changesMade = false;

	for (auto& child : _children)
	{
		auto jobs = child->CommitChanges();
		if (!jobs.empty())
			jobList.insert(jobList.end(), jobs.begin(), jobs.end());
	}

	return jobList;
}

void GuiElement::Draw(DrawContext& ctx)
{
	if (_isVisible)
	{
		auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

		auto pos = Position();
		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

		switch (_state)
		{
		case STATE_OVER:
			if (_overTexture.IsDrawInitialised())
				_overTexture.Draw(ctx);
			else
				_texture.Draw(ctx);

			break;
		case STATE_DOWN:
			if (_downTexture.IsDrawInitialised())
				_downTexture.Draw(ctx);
			else
				_texture.Draw(ctx);

			break;
		case STATE_OUT:
			if (_outTexture.IsDrawInitialised())
				_outTexture.Draw(ctx);
			else
				_texture.Draw(ctx);

			break;
		default:
			_texture.Draw(ctx);
			break;
		}

		for (auto& child : _children)
			child->Draw(ctx);

		glCtx.PopMvp();
	}
}

void GuiElement::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (_isVisible)
	{
		auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

		auto pos = ModelPosition();
		auto scale = ModelScale();

		_modelScreenPos = glCtx.ProjectScreen(pos);
		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
		glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

		for (auto& child : _children)
			child->Draw3d(ctx, 1, pass);

		glCtx.PopMvp();
		glCtx.PopMvp();
	}
}

void GuiElement::SetVisible(bool visible)
{
	_isVisible = visible;
}

void GuiElement::SetEnabled(bool enabled)
{
	_isEnabled = enabled;
}

ActionResult GuiElement::OnAction(KeyAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if (_hasFocus && (action.KeyActionType == KeyAction::KEY_UP) && ((action.KeyChar == 13) || (action.KeyChar == 32)))
	{
		return { true, std::to_string(_index), "", ACTIONRESULT_ACTIVATE, nullptr, shared_from_this() };
	}

	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction(action);

		if (res.IsEaten)
			return res;
	}

	return ActionResult::NoAction();
}

ActionResult GuiElement::OnAction(GuiAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction(action);
		if (res.IsEaten)
			return res;
	}

	return ActionResult::NoAction();
}

ActionResult GuiElement::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction((*child)->ParentToLocal(action));

		if (res.IsEaten)
			return res;
	}

	if (_guiParams.GuiPassThrough)
		return ActionResult::NoAction();

	switch (action.State)
	{
	case TouchAction::TouchState::TOUCH_DOWN:
		if (HitTest(action.Position))
		{
			_state = STATE_DOWN;

			return { true, "", "", ACTIONRESULT_DEFAULT, nullptr };
		}
		break;
	case TouchAction::TouchState::TOUCH_UP:
		_state = STATE_NORMAL;

		if (HitTest(action.Position))
			return { true, "", "", ACTIONRESULT_DEFAULT, nullptr };

		break;
	}

	return ActionResult::NoAction();
}

ActionResult GuiElement::OnAction(TouchMoveAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction((*child)->ParentToLocal(action));

		if (res.IsEaten)
			return res;
	}

	if (_guiParams.GuiPassThrough)
		return ActionResult::NoAction();

	if ((STATE_DOWN == _state) || (STATE_OUT == _state))
	{
		_state = HitTest(action.Position) ?
			STATE_DOWN : STATE_OUT;
	}
	else
	{
		_state = HitTest(action.Position) ?
			STATE_OVER : STATE_NORMAL;
	}

	return ActionResult::NoAction();
}

void GuiElement::SetParent(std::shared_ptr<GuiElement> parent)
{
	_parent = parent;
}

TouchAction GuiElement::GlobalToLocal(actions::TouchAction action)
{
	auto actionParent = nullptr == _parent ? action : _parent->GlobalToLocal(action);
	actionParent.Position -= Position();
	return actionParent;
}

TouchAction GuiElement::ParentToLocal(actions::TouchAction action)
{
	action.Position -= Position();
	return action;
}

TouchMoveAction GuiElement::GlobalToLocal(TouchMoveAction action)
{
	auto actionParent = nullptr == _parent ? action : _parent->GlobalToLocal(action);
	actionParent.Position -= Position();
	return actionParent;
}

TouchMoveAction GuiElement::ParentToLocal(TouchMoveAction action)
{
	action.Position -= Position();
	return action;
}

utils::Position2d GuiElement::GlobalToLocal(utils::Position2d pos)
{
	auto posParent = nullptr == _parent ? pos : _parent->GlobalToLocal(pos);
	posParent -= Position();
	return posParent;
}

utils::Position2d GuiElement::ParentToLocal(utils::Position2d pos)
{
	pos -= Position();
	return pos;
}

bool GuiElement::Select()
{
	auto isNewState = !_isSelected;

	if (isNewState)
	{
		_isSelected = true;

		for (auto& child : _children)
		{
			child->Select();
		}
	}

	return isNewState;
}

bool GuiElement::DeSelect()
{
	auto isNewState = _isSelected;

	if (isNewState)
	{
		_isSelected = false;

		for (auto& child : _children)
		{
			child->DeSelect();
		}
	}

	return isNewState;
}


void GuiElement::SetPicking3d(bool picking)
{
	_isPicking3d = picking;

	for (auto& child : _children)
	{
		child->SetPicking3d(picking);
	}
}

void GuiElement::SetPickingFromState(EditMode mode, bool flipState)
{
	for (auto& child : _children)
	{
		child->SetPickingFromState(mode, flipState);
	}
}

void GuiElement::SetStateFromPicking(EditMode mode, bool flipState)
{
	for (auto& child : _children)
	{
		child->SetStateFromPicking(mode, flipState);
	}
}

void GuiElement::SetIndex(unsigned int index)
{
	_index = index;
}

unsigned int GuiElement::Index() const
{
	return _index;
}

std::vector<unsigned int> GuiElement::GlobalId()
{
	if (nullptr == _parent)
		return { _index };

	auto id = _parent->GlobalId();
	id.push_back(_index);

	return id;
}

void GuiElement::AddChild(std::shared_ptr<GuiElement> child)
{
	if (nullptr == child)
		return;

	auto it = std::find(_children.begin(), _children.end(), child);
	if (it == _children.end())
	{
		_children.push_back(child);
	}

	Init();
}

std::shared_ptr<GuiElement> GuiElement::TryGetChild(unsigned char index)
{
	for (auto& child : _children)
	{
		if (child->Index() == index)
			return child;
	}

	return nullptr;
}

void GuiElement::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_texture.InitResources(resourceLib, forceInit);
	_overTexture.InitResources(resourceLib, forceInit);
	_downTexture.InitResources(resourceLib, forceInit);
	_outTexture.InitResources(resourceLib, forceInit);
}

void GuiElement::_ReleaseResources()
{
	for (auto& child : _children)
		child->ReleaseResources();

	_texture.ReleaseResources();
	_overTexture.ReleaseResources();
	_downTexture.ReleaseResources();
	_outTexture.ReleaseResources();
}

std::vector<JobAction> GuiElement::_CommitChanges()
{
	return {};
}

bool GuiElement::_HitTest(Position2d localPos)
{
	return Size2d::RectTest(_sizeParams.Size, localPos);
}