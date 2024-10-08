#include "GuiSlider.h"
#include "CommonTypes.h"
#include "glm/ext.hpp"

using namespace gui;
using namespace utils;
using namespace base;
using namespace actions;
using graphics::GlDrawContext;
using resources::ResourceLib;

GuiSlider::GuiSlider(GuiSliderParams params) :
	GuiElement(params),
	_sliderParams(params),
	_isDragging(false),
	_initClickPos({ 0,0 }),
	_initDragPos({ 0,0 }),
	_initValue(params.InitValue),
	_valueOffset(0.0),
	_dragElement(GuiElementParams(
		DrawableParams{ params.DragTexture },
		MoveableParams(params.Position, params.ModelPosition, params.ModelScale),
		SizeableParams{ params.DragControlSize, params.DragControlSize },
		params.DragOverTexture,
		params.DragDownTexture,
		params.DragOutTexture,
		{} ))
{
	SetValue(params.InitValue);
	_initDragPos = _dragElement.Position();
}

double GuiSlider::Value() const
{
	return _initValue + _valueOffset;
}

void GuiSlider::SetValue(double value)
{
	SetValue(value, false);
}

void GuiSlider::SetValue(double value, bool bypassUpdates)
{
	if (_isDragging)
		return;

	_initValue = value;
	_valueOffset = 0.0;

	OnValueChange(bypassUpdates);
}

void GuiSlider::SetDragParams(utils::Position2d dragOffset,
	utils::Size2d dragSize,
	utils::Size2d dragGap)
{
	_sliderParams.DragControlOffset = dragOffset;
	_sliderParams.DragControlSize = dragSize;
	_sliderParams.DragGap = dragGap;

	OnValueChange(true);
}

void GuiSlider::SetSize(Size2d size)
{
	GuiElement::SetSize(size);

	OnValueChange(true);
}

bool GuiSlider::HitTest(Position2d localPos)
{
	for (auto& child : _children)
	{
		if (child->HitTest(child->ParentToLocal(localPos)))
			return true;
	}

	auto left = _dragElement.Position().X;
	auto right = _dragElement.Position().X + (int)_dragElement.GetSize().Width;

	if (localPos.X > left && localPos.X < right)
	{
		auto bottom = _dragElement.Position().Y;
		auto top = _dragElement.Position().X + (int)_dragElement.GetSize().Height;

		if (localPos.Y > bottom && localPos.Y < top)
			return true;
	}

	return false;
}

void GuiSlider::Draw(DrawContext & ctx)
{
	GuiElement::Draw(ctx);

	auto &glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));
	_dragElement.Draw(ctx);
	glCtx.PopMvp();
}

ActionResult GuiSlider::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	//if (res.IsEaten)
	//	return res;

	if (_isDragging)
	{
		if (TouchAction::TOUCH_UP == action.State)
		{
			_isDragging = false;
			auto oldValue = _initValue;

			std::cout << "Slider UP" << std::endl;

			_initValue = oldValue + _valueOffset;
			_valueOffset = 0.0;
			std::cout << "New InitValue: " << _initValue << ", ValueOffset: " << _valueOffset << " = " << (_initValue + _valueOffset) << std::endl;

			ActionResult res;
			res.IsEaten = true;
			res.ResultType = ACTIONRESULT_DEFAULT;
			res.Undo = std::make_shared<DoubleActionUndo>(oldValue, GuiElement::shared_from_this());

			return res;
		}
	}
	else
	{
		if (TouchAction::TOUCH_DOWN == action.State)
		{
			if (_dragElement.HitTest(_dragElement.ParentToLocal(action.Position)))
			{
				std::cout << "Slider DOWN" << std::endl;
				_isDragging = true;
				_initClickPos = action.Position;
				_initDragPos = _dragElement.Position();
				_valueOffset = 0.0;

				ActionResult res;
				res.IsEaten = true;
				res.ResultType = ACTIONRESULT_ACTIVEELEMENT;
				res.ActiveElement = GuiElement::shared_from_this();

				return res;
			}
		}
	}

	return GuiElement::OnAction(action);
}

ActionResult GuiSlider::OnAction(TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
		return res;

	res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;

	if (!_isDragging)
		return res;

	auto dPos = action.Position - _initClickPos;
	auto dragPos = _initDragPos + dPos;

	_valueOffset = CalcValueOffset(_sliderParams, _sizeParams.Size, dragPos, _initDragPos, _initValue);
	std::cout << "InitValue: " << _initValue << ", ValueOffset: " << _valueOffset << " = " << (_initValue + _valueOffset) << std::endl;

	OnValueChange(false);

	res.IsEaten = true;
	return res;
}

bool GuiSlider::Undo(std::shared_ptr<ActionUndo> undo)
{
	if (_isDragging)
		return false;

	auto doubleUndo = std::dynamic_pointer_cast<DoubleActionUndo>(undo);

	if (doubleUndo)
	{
		SetValue(doubleUndo->Value());
		return true;
	}

	return false;
}

bool GuiSlider::Redo(std::shared_ptr<ActionUndo> undo)
{
	if (_isDragging)
		return false;

	auto doubleUndo = std::dynamic_pointer_cast<DoubleActionUndo>(undo);

	if (doubleUndo)
	{
		SetValue(doubleUndo->Value());
		return true;
	}

	return false;
}

void GuiSlider::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_dragElement.InitResources(resourceLib, forceInit);
	GuiElement::_InitResources(resourceLib, forceInit);
}

void GuiSlider::_ReleaseResources()
{
	_dragElement.ReleaseResources();
	GuiElement::_ReleaseResources();
}

void GuiSlider::OnValueChange(bool bypassUpdates)
{
	auto value = _initValue + _valueOffset;
	_dragElement.SetPosition(CalcDragPos(_sliderParams, _sizeParams.Size, value));

	if (_receiver && !bypassUpdates)
		_receiver->OnAction(DoubleAction(value));
}

double GuiSlider::CalcValueOffset(GuiSliderParams params,
	utils::Size2d size,
	Position2d dragPos,
	Position2d initDragPos,
	double initValue)
{
	auto valRange = params.Max - params.Min;
	double dragFrac = 0.0;

	auto dragLength = CalcDragLength(params, size);

	if (dragLength > 0)
		dragFrac = GuiSliderParams::SLIDER_VERTICAL == params.Orientation ?
		std::clamp(dragPos.Y - params.DragControlOffset.Y, 0, (int)dragLength) / (double)dragLength :
		std::clamp(dragPos.X - params.DragControlOffset.X, 0, (int)dragLength) / (double)dragLength;

	if (params.Steps > 0)
	{
		auto dStep = valRange / (double)params.Steps;
		auto stepNum = (int)round(dragFrac * params.Steps);
		auto newValue = params.Min + (stepNum * dStep);
		return newValue - initValue;
	}

	auto initDrag = GuiSliderParams::SLIDER_VERTICAL == params.Orientation ?
		initDragPos.Y :
		initDragPos.X;

	auto newDragPos = GuiSliderParams::SLIDER_VERTICAL == params.Orientation ?
		Position2d{
			params.DragControlOffset.X,
			((int)round(dragFrac * CalcDragLength(params, size))) + params.DragControlOffset.Y
	} :
		Position2d{
			((int)round(dragFrac * CalcDragLength(params, size))) + params.DragControlOffset.X,
			params.DragControlOffset.Y
	};

	auto dDragPos = newDragPos - initDragPos;
	auto dDrag = GuiSliderParams::SLIDER_VERTICAL == params.Orientation ?
		dDragPos.Y :
		dDragPos.X;
	auto dDragFrac = ((double)dDrag) / ((double)CalcDragLength(params, size));

	return valRange * dDragFrac;
}

utils::Position2d GuiSlider::CalcDragPos(GuiSliderParams params,
	utils::Size2d size,
	double value)
{
	auto valRange = params.Max - params.Min;
	auto valFrac = (value - params.Min) / valRange;

	return GuiSliderParams::SLIDER_VERTICAL == params.Orientation ?
		Position2d{
			params.DragControlOffset.X,
			((int)round(valFrac * CalcDragLength(params, size))) + params.DragControlOffset.Y
	} :
		Position2d{
			((int)round(valFrac * CalcDragLength(params, size))) + params.DragControlOffset.X,
			params.DragControlOffset.Y
	};
}

unsigned int GuiSlider::CalcDragLength(GuiSliderParams params,
	utils::Size2d size)
{
	if (GuiSliderParams::SLIDER_VERTICAL == params.Orientation)
	{
		auto h = params.DragControlSize.Height + (2 * params.DragGap.Height);
		if (h > size.Height)
			return size.Height;
		else
			return size.Height - h;
	}
	else
	{
		auto w = params.DragControlSize.Width + (2 * params.DragGap.Width);
		if (w > params.Size.Width)
			return params.Size.Width;
		else
			return params.Size.Width - w;
	}
}
