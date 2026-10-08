#include "GuiSlider.h"
#include "CommonTypes.h"
#include "glm/ext.hpp"
#include "../audio/AudioMixer.h"
#include <cmath>
#include <algorithm>
#include <limits>

using namespace gui;
using namespace utils;
using namespace base;
using namespace actions;
using graphics::GlDrawContext;
using resources::ResourceLib;

double GuiSliderParams::ValueToFraction(double value) const
{
	if (!(Max > Min))
		return 0.0;
	if (Scale == SliderScale::Linear)
		return (value - Min) / (Max - Min);
	if (!(value > 0.0) || !(Max > 0.0))
		return 0.0;
	const auto maxDecibels = 20.0 * std::log10(Max);
	if (!(maxDecibels > MinDecibels))
		return 0.0;
	return std::clamp((20.0 * std::log10(value) - MinDecibels) /
		(maxDecibels - MinDecibels), 0.0, 1.0);
}

double GuiSliderParams::FractionToValue(double fraction) const
{
	fraction = std::clamp(fraction, 0.0, 1.0);
	if (fraction <= 0.0 || !(Max > Min))
		return Min;
	if (fraction >= 1.0)
		return Max;
	if (Scale == SliderScale::Linear)
		return Min + fraction * (Max - Min);
	if (!(Max > 0.0))
		return Min;
	const auto maxDecibels = 20.0 * std::log10(Max);
	if (!(maxDecibels > MinDecibels))
		return Min;
	return std::pow(10.0, (MinDecibels + fraction * (maxDecibels - MinDecibels)) / 20.0);
}

GuiSlider::GuiSlider(GuiSliderParams params) :
	GuiElement(params),
	_sliderParams(params),
	_isDragging(false),
	_initClickPos({ 0,0 }),
	_initDragPos({ 0,0 }),
	_dragElement([params]() {
			GuiElementParams dragParams(0,
				DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1, 1 },
				"",
				"",
				"",
				{});
			dragParams.Texture = params.DragTexture;
			dragParams.OverTexture = params.DragOverTexture;
			dragParams.DownTexture = params.DragDownTexture;
			dragParams.OutTexture = params.DragOutTexture;
			dragParams.Position = { 0, 0 };
			dragParams.ModelPosition = params.ModelPosition;
			dragParams.ModelScale = params.ModelScale;
			dragParams.Size = params.DragControlSize;
			dragParams.MinSize = params.DragControlSize;
			dragParams.GuiPassThrough = false;
			return dragParams;
		}()),
	_scaleUnityImage(_MakeScaleImageParams("fader_scale_line", { 0, 4 })),
	_scaleEndpointImage(_MakeScaleImageParams("fader_scale_line", { 0, 4 })),
	_scaleIntermediateImage(_MakeScaleImageParams("fader_scale_line", { 0, 4 })),
	_scaleTrackImage(_MakeScaleImageParams("fader_scale_track", { 8, 0 })),
	_scaleMarks(),
	_scaleTrackBounds(),
	_scaleLayoutValid(false),
	_scaleLayoutSize({ 0, 0 }),
	_scaleLayoutHandleSize({ 0, 0 }),
	_scaleLayoutHandleOffset({ 0, 0 }),
	_scaleLayoutGap({ 0, 0 }),
	_scaleLayoutOrientation(GuiSliderParams::SLIDER_VERTICAL),
	_scaleLayoutScale(GuiSliderParams::SliderScale::Linear),
	_scaleLayoutMin(0.0),
	_scaleLayoutMax(0.0),
	_scaleLayoutMinDecibels(0.0),
	_scaleLayoutEnabled(false),
	_scaleImagePanelWidth(std::numeric_limits<unsigned int>::max()),
	_scaleAppliedTrackBounds(),
	_scaleTrackSizeApplied(false),
	_valueOffset(0.0),
	_initValue(params.InitValue),
	_mixer()
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
	_dragElement.SetSize(dragSize);
	_scaleLayoutValid = false;

	OnValueChange(true);
}

void GuiSlider::SetSize(Size2d size)
{
	GuiElement::SetSize(size);
	_scaleLayoutValid = false;

	OnValueChange(true);
}

void GuiSlider::Draw(DrawContext & ctx)
{
	GuiElement::Draw(ctx);

	auto &glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));
	if (_isVisible && _sliderParams.ScaleMarksEnabled)
	{
		_EnsureScaleLayout();
		const auto previousTint = glCtx.GetUniform("TintColor");
		if (_scaleTrackBounds.Valid)
		{
			const auto& bounds = _scaleTrackBounds.Bounds;
			glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(bounds.Left, bounds.Bottom, 0.0f)));
			glCtx.SetUniform("TintColor", glm::vec3(0.0f));
			{
				auto opacity = ctx.WithOpacity(1.0f);
				_scaleTrackImage.Draw(ctx);
			}
			glCtx.PopMvp();
		}

		for (const auto& mark : _scaleMarks)
		{
			auto* image = &_scaleIntermediateImage;
			glm::vec3 tint(0xF6 / 255.0f, 0xDD / 255.0f, 0xE6 / 255.0f);
			float opacity = 0.25f;
			int inset = 12;
			switch (mark.Kind)
			{
			case ScaleMarkKind::Unity:
				image = &_scaleUnityImage;
				tint = glm::vec3(0xFF / 255.0f, 0xC8 / 255.0f, 0x78 / 255.0f);
				opacity = 0.90f;
				inset = 8;
				break;
			case ScaleMarkKind::Endpoint:
				image = &_scaleEndpointImage;
				opacity = 0.65f;
				inset = 8;
				break;
			case ScaleMarkKind::Intermediate:
				break;
			}
			if (image->GetSize().Width < 6u)
				continue;

			glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(inset, mark.CentreY - 2, 0.0f)));
			glCtx.SetUniform("TintColor", tint);
			{
				auto scopedOpacity = ctx.WithOpacity(opacity);
				image->Draw(ctx);
			}
			glCtx.PopMvp();
		}
		if (previousTint.has_value())
			glCtx.SetUniform("TintColor", *previousTint);
		else
			glCtx.SetUniform("TintColor", glm::vec3(1.0f));
	}
	_dragElement.Draw(ctx);

	auto mixer = _mixer.lock();
	if (mixer)
		mixer->DrawVu(ctx, GetSize());

	glCtx.PopMvp();
}

ActionResult GuiSlider::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	//if (res.IsEaten)
	//	return res;

	if (!_isEnabled || !_isVisible)
		return res;

	_dragElement.OnAction(_dragElement.ParentToLocal(action));

	if (_isDragging)
	{
		if (TouchAction::TOUCH_UP == action.State)
		{
			_isDragging = false;
			auto oldValue = _initValue;

			std::cout << "Slider UP" << std::endl;

			_initValue = oldValue + _valueOffset;
			_valueOffset = 0.0;
			ApplyHoverPoint(action.Position);
			std::cout << "New InitValue: " << _initValue << ", ValueOffset: " << _valueOffset << " = " << (_initValue + _valueOffset) << std::endl;

			ActionResult res;
			res.IsEaten = true;
			res.ResultType = ACTIONRESULT_DEFAULT;
			res.Undo = std::make_shared<GuiActionUndo>(oldValue, GuiElement::shared_from_this());

			return res;
		}
	}
	else
	{
		if (TouchAction::TOUCH_DOWN == action.State)
		{
			if (RouteHitTest(action.Position))
			{
				std::cout << "Slider DOWN" << std::endl;
				_isDragging = true;
				_initClickPos = action.Position;
				_initDragPos = _dragElement.Position();
				_valueOffset = 0.0;
				if (!_HitTest(action.Position))
					_state = STATE_DOWN;

				ActionResult res;
				res.IsEaten = true;
				res.ResultType = ACTIONRESULT_ACTIVEELEMENT;
				res.ActiveElement = GuiElement::shared_from_this();

				return res;
			}
		}
	}

	return res;
}

ActionResult GuiSlider::OnAction(TouchMoveAction action)
{
	ActionResult res = ActionResult::NoAction();

	if (!_isEnabled || !_isVisible)
		return res;

	if (!_isDragging)
	{
		res = GuiElement::OnAction(action);
		if (res.IsEaten)
			return res;

		_dragElement.OnAction(_dragElement.ParentToLocal(action));
	}

	if (!_isDragging)
		return res;

	auto dPos = action.Position - _initClickPos;
	auto dragPos = _initDragPos + dPos;
	if (CalcDragLength(_sliderParams, _sizeParams.Size) == 0u || !(_sliderParams.Max > _sliderParams.Min))
	{
		// Degenerate layouts preserve the current gain and must not emit an
		// unchanged slider action to the mixer.
		res.IsEaten = true;
		return res;
	}

	_valueOffset = CalcValueOffset(_sliderParams, _sizeParams.Size, dragPos, _initDragPos, _initValue);
	std::cout << "InitValue: " << _initValue << ", ValueOffset: " << _valueOffset << " = " << (_initValue + _valueOffset) << std::endl;

	OnValueChange(false);

	res.IsEaten = true;

	return res;
}

void GuiSlider::ApplyHoverPoint(utils::Position2d localPos)
{
	const auto dragLocalPos = _dragElement.ParentToLocal(localPos);
	const bool dragIsHovered = _dragElement.HitTest(dragLocalPos);
	GuiElement::ApplyHoverState(_HitTest(localPos) || dragIsHovered);
	_dragElement.ApplyHoverState(dragIsHovered);
}

bool GuiSlider::RouteHitTest(utils::Position2d localPos)
{
	if (!IsEnabled() || !IsVisible() || _guiParams.GuiPassThrough)
		return false;

	const auto dragLocalPos = _dragElement.ParentToLocal(localPos);
	return GuiElement::RouteHitTest(localPos) || _dragElement.HitTest(dragLocalPos);
}

std::shared_ptr<base::GuiElement> GuiSlider::FindTopmostDescendant(utils::Position2d localPos)
{
	if (_guiParams.GuiPassThrough)
		return GuiElement::FindTopmostDescendant(localPos);

	if (!RouteHitTest(localPos))
		return nullptr;

	const auto dragLocalPos = _dragElement.ParentToLocal(localPos);
	if (_dragElement.HitTest(dragLocalPos))
		return shared_from_this();

	return GuiElement::FindTopmostDescendant(localPos);
}

void GuiSlider::ApplyHoverState(bool inside)
{
	GuiElement::ApplyHoverState(inside);
	if (!inside)
		_dragElement.ApplyHoverState(false);
}

void GuiSlider::ClearPointerState()
{
	GuiElement::ClearPointerState();
	_dragElement.ClearPointerState();
	if (_isDragging)
	{
		_isDragging = false;
		_valueOffset = 0.0;
		OnValueChange(false);
	}
}

bool GuiSlider::Undo(std::shared_ptr<ActionUndo> undo)
{
	if (_isDragging)
		return false;

	auto doubleUndo = std::dynamic_pointer_cast<GuiActionUndo>(undo);

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

	auto doubleUndo = std::dynamic_pointer_cast<GuiActionUndo>(undo);

	if (doubleUndo)
	{
		SetValue(doubleUndo->Value());
		return true;
	}

	return false;
}

std::vector<GuiSlider::ScaleMark> GuiSlider::BuildScaleMarks(
	const GuiSliderParams& params,
	utils::Size2d size)
{
	std::vector<ScaleMark> marks;
	_BuildScaleMarks(params, size, marks);
	return marks;
}

void GuiSlider::_BuildScaleMarks(
	const GuiSliderParams& params,
	utils::Size2d size,
	std::vector<ScaleMark>& marks)
{
	marks.clear();
	if (!params.ScaleMarksEnabled || params.Orientation != GuiSliderParams::SLIDER_VERTICAL ||
		params.Scale != GuiSliderParams::SliderScale::Decibels || params.Min != 0.0 ||
		!(params.Max >= 1.0) || !(params.MinDecibels < 0.0) ||
		!std::isfinite(params.MinDecibels) || params.MinDecibels != GuiSliderParams::RackMinDecibels ||
		!(params.Max > params.Min))
		return;

	const auto handleHeight = params.DragControlSize.Height;
	const auto gaps = 2ull * params.DragGap.Height;
	if (size.Height <= static_cast<unsigned long long>(handleHeight) + gaps)
		return;

	const auto travel = CalcDragLength(params, size);
	if (travel == 0u)
		return;

	const auto maxDecibels = 20.0 * std::log10(params.Max);
	// The scale grid below is defined for the rack range (-48 to +20 dB).
	// If that range changes, its integer index policy must be updated explicitly.
	if (!std::isfinite(maxDecibels) || std::abs(maxDecibels - GuiSliderParams::RackMaxDecibels) > 1e-6)
		return;
	constexpr double range = GuiSliderParams::RackMaxDecibels - GuiSliderParams::RackMinDecibels;

	const auto addMark = [&params, size, &marks](double gain, ScaleMarkKind kind)
	{
		const auto position = CalcDragPos(params, size, gain);
		marks.push_back({ position.Y + static_cast<int>(params.DragControlSize.Height / 2u), gain, kind });
	};
	// Keep the base 6 dB grid at least nine pixels apart on compact faders.
	if (travel < range * 9.0 / 6.0)
	{
		addMark(params.Min, ScaleMarkKind::Endpoint);
		addMark(1.0, ScaleMarkKind::Unity);
		addMark(params.Max, ScaleMarkKind::Endpoint);
	}
	else
	{
		const auto subdivisions = static_cast<unsigned int>(std::clamp(
			static_cast<int>(std::floor(6.0 * travel / (range * 14.0))), 1, 4));
		const auto lastGridIndex = static_cast<unsigned int>(std::floor(range * subdivisions / 6.0));
		marks.reserve(lastGridIndex + 2u);
		for (auto index = 0u; index <= lastGridIndex; ++index)
		{
			const auto scaledDecibels = static_cast<int>(GuiSliderParams::RackMinDecibels) *
				static_cast<int>(subdivisions) + 6 * static_cast<int>(index);
			const auto decibels = static_cast<double>(scaledDecibels) / subdivisions;
			const auto gain = index == 0u ? params.Min : std::pow(10.0, decibels / 20.0);
			ScaleMarkKind kind = ScaleMarkKind::Intermediate;
			if (index == 0u || (index == lastGridIndex && scaledDecibels ==
				static_cast<int>(GuiSliderParams::RackMaxDecibels) * static_cast<int>(subdivisions)))
				kind = ScaleMarkKind::Endpoint;
			else if (index == 8u * subdivisions)
				kind = ScaleMarkKind::Unity;
			addMark(kind == ScaleMarkKind::Endpoint && index != 0u
				? params.Max : gain, kind);
		}

		if (GuiSliderParams::RackMinDecibels + 6.0 * lastGridIndex / subdivisions < GuiSliderParams::RackMaxDecibels)
			addMark(params.Max, ScaleMarkKind::Endpoint);
	}

	const auto priority = [](ScaleMarkKind kind)
	{
		switch (kind)
		{
		case ScaleMarkKind::Unity: return 4;
		case ScaleMarkKind::Endpoint: return 3;
		case ScaleMarkKind::Intermediate: return 1;
		}
		return 0;
	};
	std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b)
	{
		return a.CentreY < b.CentreY;
	});
	auto uniqueCount = 0u;
	const auto maximumCentreY = CalcDragPos(params, size, params.Max).Y +
		static_cast<int>(handleHeight / 2u);
	for (const auto& mark : marks)
	{
		// The partial top interval can be very short. Drop its intermediate mark
		// instead of hiding the rest of an otherwise well-spaced scale.
		if (mark.Kind == ScaleMarkKind::Intermediate && maximumCentreY - mark.CentreY < 6)
			continue;
		if (uniqueCount == 0u || marks[uniqueCount - 1u].CentreY != mark.CentreY)
			marks[uniqueCount++] = mark;
		else if (priority(mark.Kind) > priority(marks[uniqueCount - 1u].Kind))
			marks[uniqueCount - 1u] = mark;
	}
	marks.erase(marks.begin() + uniqueCount, marks.end());
}

GuiSlider::ScaleTrackBounds GuiSlider::BuildScaleTrackBounds(
	const GuiSliderParams& params,
	utils::Size2d size)
{
	if (!params.ScaleMarksEnabled || params.Orientation != GuiSliderParams::SLIDER_VERTICAL ||
		params.Scale != GuiSliderParams::SliderScale::Decibels || !(params.Max > params.Min) ||
		params.Min != 0.0 || params.MinDecibels != GuiSliderParams::RackMinDecibels || !(params.Max >= 1.0))
		return {};
	const auto handleHeight = params.DragControlSize.Height;
	const auto gaps = 2ull * params.DragGap.Height;
	if (size.Height <= static_cast<unsigned long long>(handleHeight) + gaps)
		return {};
	const auto travel = CalcDragLength(params, size);
	const auto maxDecibels = 20.0 * std::log10(params.Max);
	if (travel < 4u || !std::isfinite(maxDecibels) || std::abs(maxDecibels - GuiSliderParams::RackMaxDecibels) > 1e-6)
		return {};
	const auto minimum = CalcDragPos(params, size, params.Min);
	const auto maximum = CalcDragPos(params, size, params.Max);
	const auto centerX = params.DragControlOffset.X + static_cast<int>(params.DragControlSize.Width / 2u);
	const auto minimumY = minimum.Y + static_cast<int>(handleHeight / 2u);
	const auto maximumY = maximum.Y + static_cast<int>(handleHeight / 2u);
	if (maximumY - minimumY < 4)
		return {};
	return { { centerX - 4, minimumY, centerX + 4, maximumY }, true };
}

graphics::ImageParams GuiSlider::_MakeScaleImageParams(const std::string& texture, utils::Size2d size)
{
	return graphics::ImageParams(
		DrawableParams{ texture },
		SizeableParams{ size, size },
		"texture_tinted", false, false, false, 1.0f);
}

bool GuiSlider::_ScaleLayoutKeyMatches() const
{
	return _scaleLayoutValid && _scaleLayoutSize == _sizeParams.Size &&
		_scaleLayoutHandleSize == _sliderParams.DragControlSize &&
		_scaleLayoutHandleOffset == _sliderParams.DragControlOffset &&
		_scaleLayoutGap == _sliderParams.DragGap &&
		_scaleLayoutOrientation == _sliderParams.Orientation &&
		_scaleLayoutScale == _sliderParams.Scale && _scaleLayoutMin == _sliderParams.Min &&
		_scaleLayoutMax == _sliderParams.Max && _scaleLayoutMinDecibels == _sliderParams.MinDecibels &&
		_scaleLayoutEnabled == _sliderParams.ScaleMarksEnabled;
}

void GuiSlider::_StoreScaleLayoutKey()
{
	_scaleLayoutSize = _sizeParams.Size;
	_scaleLayoutHandleSize = _sliderParams.DragControlSize;
	_scaleLayoutHandleOffset = _sliderParams.DragControlOffset;
	_scaleLayoutGap = _sliderParams.DragGap;
	_scaleLayoutOrientation = _sliderParams.Orientation;
	_scaleLayoutScale = _sliderParams.Scale;
	_scaleLayoutMin = _sliderParams.Min;
	_scaleLayoutMax = _sliderParams.Max;
	_scaleLayoutMinDecibels = _sliderParams.MinDecibels;
	_scaleLayoutEnabled = _sliderParams.ScaleMarksEnabled;
	_scaleLayoutValid = true;
}

void GuiSlider::_UpdateScaleImageWidths(unsigned int panelWidth)
{
	const auto lineWidth = [panelWidth](unsigned int inset)
	{
		if (panelWidth <= 2u * inset)
			return 0u;
		const auto width = panelWidth - 2u * inset;
		return width >= 6u ? width : 0u;
	};
	const auto endpointWidth = lineWidth(8u);
	const auto intermediateWidth = lineWidth(12u);
	if (_scaleUnityImage.GetSize().Width != endpointWidth)
		_scaleUnityImage.SetSize({ endpointWidth, 4u });
	if (_scaleEndpointImage.GetSize().Width != endpointWidth)
		_scaleEndpointImage.SetSize({ endpointWidth, 4u });
	if (_scaleIntermediateImage.GetSize().Width != intermediateWidth)
		_scaleIntermediateImage.SetSize({ intermediateWidth, 4u });
}

void GuiSlider::_EnsureScaleLayout()
{
	if (_ScaleLayoutKeyMatches())
		return;
	if (_scaleImagePanelWidth != _sizeParams.Size.Width)
	{
		_UpdateScaleImageWidths(_sizeParams.Size.Width);
		_scaleImagePanelWidth = _sizeParams.Size.Width;
	}
	_BuildScaleMarks(_sliderParams, _sizeParams.Size, _scaleMarks);
	_scaleTrackBounds = BuildScaleTrackBounds(_sliderParams, _sizeParams.Size);
	utils::Rect2d trackRect;
	if (_scaleTrackBounds.Valid)
		trackRect = _scaleTrackBounds.Bounds;
	if (!_scaleTrackSizeApplied || trackRect.Left != _scaleAppliedTrackBounds.Left ||
		trackRect.Bottom != _scaleAppliedTrackBounds.Bottom || trackRect.Right != _scaleAppliedTrackBounds.Right ||
		trackRect.Top != _scaleAppliedTrackBounds.Top)
	{
		const auto trackWidth = trackRect.IsEmpty() ? 0u : static_cast<unsigned int>(trackRect.Right - trackRect.Left);
		const auto trackHeight = trackRect.IsEmpty() ? 0u : static_cast<unsigned int>(trackRect.Top - trackRect.Bottom);
		_scaleTrackImage.SetSize({ trackWidth, trackHeight });
		_scaleAppliedTrackBounds = trackRect;
		_scaleTrackSizeApplied = true;
	}
	_scaleLayoutValid = false;
	_StoreScaleLayoutKey();
}

bool GuiSlider::DragHandleIsOverForTest() const noexcept
{
	return _dragElement.GetState() == GuiElement::STATE_OVER;
}

void GuiSlider::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_dragElement.InitResources(resourceLib, forceInit);
	if (_sliderParams.ScaleMarksEnabled)
	{
		_EnsureScaleLayout();
		_scaleUnityImage.InitResources(resourceLib, forceInit);
		_scaleEndpointImage.InitResources(resourceLib, forceInit);
		_scaleIntermediateImage.InitResources(resourceLib, forceInit);
		_scaleTrackImage.InitResources(resourceLib, forceInit);
	}
	
	// Initialize mixer's resources (VU meter shaders, vertex buffers, etc.)
	auto mixer = _mixer.lock();
	if (mixer)
		mixer->InitResources(resourceLib, forceInit);
	
	GuiElement::_InitResources(resourceLib, forceInit);
}

void GuiSlider::_ReleaseResources()
{
	_dragElement.ReleaseResources();
	if (_sliderParams.ScaleMarksEnabled)
	{
		_scaleUnityImage.ReleaseResources();
		_scaleEndpointImage.ReleaseResources();
		_scaleIntermediateImage.ReleaseResources();
		_scaleTrackImage.ReleaseResources();
	}
	
	// Release mixer's resources
	auto mixer = _mixer.lock();
	if (mixer)
		mixer->ReleaseResources();
	
	GuiElement::_ReleaseResources();
}

bool GuiSlider::_HitTest(Position2d localPos)
{
	return Size2d::RectTest(_sizeParams.Size, localPos);
}

void GuiSlider::OnValueChange(bool bypassUpdates)
{
	auto value = _initValue + _valueOffset;
	_dragElement.SetPosition(CalcDragPos(_sliderParams, _sizeParams.Size, value));

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	// Use the semantic slider index from params, not container child order.
	action.Index = _sliderParams.Index;
	action.Data = GuiAction::GuiDouble(value);

	if (_receiver && !bypassUpdates)
		_receiver->OnAction(action);
}

void GuiSlider::SetMixer(std::shared_ptr<audio::AudioMixer> mixer)
{
	_mixer = mixer;
	// Reinitialize if the mixer was connected after GL resources were created.
	if (mixer)
		_resourcesNeedInitialising = true;
}

void GuiSlider::SetVuVisible(bool visible)
{
	auto mixer = _mixer.lock();
	if (mixer)
		mixer->SetVuVisible(visible);
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
	if (dragLength == 0u || !(params.Max > params.Min))
		return 0.0;

	if (params.Scale == GuiSliderParams::SliderScale::Decibels)
	{
		const auto vertical = params.Orientation == GuiSliderParams::SLIDER_VERTICAL;
		const auto position = vertical ? dragPos.Y : dragPos.X;
		const auto initialPosition = vertical ? initDragPos.Y : initDragPos.X;
		const auto offset = vertical ? params.DragControlOffset.Y : params.DragControlOffset.X;
		if (position == initialPosition)
			return 0.0;
		// Apply pointer motion in dB travel, preserving the exact initial gain
		// despite the handle's rounded pixel position. Endpoints remain exact.
		auto fraction = std::clamp(params.ValueToFraction(initValue) +
			static_cast<double>(position - initialPosition) / dragLength, 0.0, 1.0);
		if (position <= offset)
			fraction = 0.0;
		else if (position - offset >= static_cast<int>(dragLength))
			fraction = 1.0;
		if (params.Steps > 0u)
			fraction = std::round(fraction * params.Steps) / params.Steps;
		return params.FractionToValue(fraction) - initValue;
	}

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
	auto valFrac = params.ValueToFraction(value);

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
		if (h >= size.Height)
			return 0u;
		else
			return size.Height - h;
	}
	else
	{
		auto w = params.DragControlSize.Width + (2 * params.DragGap.Width);
		if (w >= size.Width)
			return 0u;
		else
			return size.Width - w;
	}
}
