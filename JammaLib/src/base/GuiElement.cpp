#include "GuiElement.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace base;
using namespace utils;
using namespace actions;
using graphics::GlDrawContext;
using graphics::ImageParams;
using resources::ResourceLib;

GuiElement::GuiElement(GuiElementParams params) :
	Drawable(params),
	Moveable(params),
	Sizeable(params),
	_changesMade(false),
	_guiParams(params),
	_state(STATE_NORMAL),
	_texture(ImageParams(DrawableParams{ params.Texture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_overTexture(ImageParams(DrawableParams{ params.OverTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_downTexture(ImageParams(DrawableParams{ params.DownTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_outTexture(ImageParams(DrawableParams{ params.OutTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_children({})
{
}

void GuiElement::Init()
{
	InitReceivers();

	for (auto& child : _children)
	{
		child->SetParent(GuiElement::shared_from_this());
		child->Init();
	}
}

void GuiElement::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);

	for (auto& child : _children)
		child->InitResources(resourceLib, forceInit);
};

void GuiElement::SetSize(Size2d size)
{
	Sizeable::SetSize(size);

	_texture.SetSize(_sizeParams.Size);
	_overTexture.SetSize(_sizeParams.Size);
	_downTexture.SetSize(_sizeParams.Size);
	_outTexture.SetSize(_sizeParams.Size);
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
	auto &glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

	switch (_state)
	{
	case STATE_OVER:
		_overTexture.Draw(ctx);
		break;
	case STATE_DOWN:
		_downTexture.Draw(ctx);
		break;
	case STATE_OUT:
		_outTexture.Draw(ctx);
		break;
	default:
		_texture.Draw(ctx);
		break;
	}

	for (auto& child : _children)
		child->Draw(ctx);

	glCtx.PopMvp();
}

void GuiElement::Draw3d(DrawContext& ctx,
	unsigned int numInstances)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto pos = ModelPosition();
	auto scale = ModelScale();

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	for (auto& child : _children)
		child->Draw3d(ctx, 1);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

ActionResult GuiElement::OnAction(KeyAction action)
{
	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction(action);

		if (res.IsEaten)
			return res;
	}

	return { false, "", ACTIONRESULT_DEFAULT, nullptr };
}

ActionResult GuiElement::OnAction(TouchAction action)
{
	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction((*child)->ParentToLocal(action));

		if (res.IsEaten)
			return res;
	}

	if (Size2d::RectTest(_sizeParams.Size, action.Position))
		return { true, "", ACTIONRESULT_DEFAULT, nullptr };

	return { false, "", ACTIONRESULT_DEFAULT, nullptr };
}

ActionResult GuiElement::OnAction(TouchMoveAction action)
{
	for (auto child = _children.rbegin();
		child != _children.rend(); ++child)
	{
		auto res = (*child)->OnAction((*child)->ParentToLocal(action));

		if (res.IsEaten)
			return res;
	}

	return { false, "", ACTIONRESULT_DEFAULT, nullptr };
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

bool GuiElement::HitTest(Position2d localPos)
{
	for (auto& child : _children)
	{
		if (child->HitTest(child->ParentToLocal(localPos)))
			return true;
	}

	return Size2d::RectTest(_sizeParams.Size, localPos);

	return false;
}

void GuiElement::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_texture.InitResources(resourceLib, forceInit);
	_overTexture.InitResources(resourceLib, forceInit);
	_downTexture.InitResources(resourceLib, forceInit);
	_outTexture.InitResources(resourceLib, forceInit);

	for (auto& child : _children)
		child->InitResources(resourceLib, forceInit);
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