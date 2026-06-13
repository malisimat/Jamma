#include "QuantisationInteractionController.h"

#include <algorithm>
#include <iostream>
#include "../base/GuiElement.h"
#include "Loop.h"
#include "LoopTake.h"
#include "Station.h"

using namespace actions;
using namespace base;
using namespace engine;
using namespace utils;

QuantisationInteractionController::QuantisationInteractionController(graphics::CtrlHandleOverlay& overlay,
	Quantisation& quantisation,
	std::vector<std::shared_ptr<Station>>& stations) :
	_overlay(overlay),
	_quantisation(quantisation),
	_stations(stations),
	_ctrlHandleReleasedAt(Timer::GetZero()),
	_fractionDragStartFraction(midi::MidiQuantisationFraction::Whole)
{
}

void QuantisationInteractionController::OnCtrlModifierChanged(bool held,
	Time now,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (held && !_ctrlHandleHeld)
		_CaptureContext(context, childResolver);

	_quantisation.SetOverlayHeld(held);
	_ctrlHandleHeld = held;
	if (!held)
		_ctrlHandleReleasedAt = now;
	RefreshOverlay(context, childResolver);
}

void QuantisationInteractionController::RefreshOverlay(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (_ctrlOverlayContext.has_value())
	{
		_overlay.SetVisibleButtonCount(_ctrlOverlayContext->VisibleButtonCount);
		_overlay.SetAnchor(_ctrlOverlayContext->Anchor, context.ViewportSize);
		return;
	}

	_overlay.SetVisibleButtonCount(_VisibleButtonCount(context));
	if (_ctrlHandleHeld)
		_overlay.SetAnchor(context.CursorPos, context.ViewportSize);
}

void QuantisationInteractionController::Tick(Time now)
{
	_ApplyCtrlHandleAlpha(_CtrlHandleAlpha(now));
}

std::optional<ActionResult> QuantisationInteractionController::TryHandleTouchAction(TouchAction action,
	unsigned int sampleRate,
	bool ctrlModifier,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (_isFractionDragging)
	{
		if (TouchAction::TouchState::TOUCH_UP == action.State)
			return _EndFractionDrag(action);

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if (_isMidiPhaseDragging)
	{
		if (TouchAction::TouchState::TOUCH_UP == action.State)
			return _EndMidiPhaseDrag(action, sampleRate);

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if ((TouchAction::TouchState::TOUCH_DOWN != action.State)
		|| (0 != action.Index)
		|| !ctrlModifier)
		return std::nullopt;

	const int hitBtn = _overlay.HitTestButton(action.Position);
	const int visibleButtons = _ctrlOverlayContext.has_value()
		? _ctrlOverlayContext->VisibleButtonCount
		: _VisibleButtonCount(context);
	const int fractionBtn = (visibleButtons >= 3) ? 2 : 1;
	std::cout << "Ctrl overlay handle down: button=" << hitBtn
		<< " mode="
		<< ((0 == hitBtn)
			? "phase-global"
			: ((1 == hitBtn) && (visibleButtons >= 3))
				? "phase-local"
				: (fractionBtn == hitBtn)
					? "fraction"
					: "none")
		<< std::endl;
	if (0 == hitBtn)
		return _BeginMidiPhaseDrag(action, MidiPhaseDragRoute::Global, context, childResolver);
	if ((1 == hitBtn) && (visibleButtons >= 3))
		return _BeginMidiPhaseDrag(action, MidiPhaseDragRoute::Local, context, childResolver);
	if (fractionBtn == hitBtn)
		return _BeginFractionDrag(action, context, childResolver);

	ActionResult res;
	res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

std::optional<ActionResult> QuantisationInteractionController::TryHandleTouchMove(TouchMoveAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
		return _UpdateMidiPhaseDrag(action, sampleRate);

	if (_isFractionDragging)
		return _UpdateFractionDrag(action);

	return std::nullopt;
}

int QuantisationInteractionController::VisibleButtonCountForTest() const noexcept
{
	return _overlay.VisibleButtonCount();
}

std::optional<Position2d> QuantisationInteractionController::ButtonCenterForTest(int buttonIndex) const noexcept
{
	return _overlay.ButtonCenter(buttonIndex);
}

void QuantisationInteractionController::_CaptureContext(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	CtrlOverlayContext captured;
	captured.Anchor = context.CursorPos;
	captured.VisibleButtonCount = _VisibleButtonCount(context);
	captured.SelectDepth = context.SelectDepth;

	std::vector<unsigned char> hoverPath = context.HoverPath;
	auto hovering = childResolver(hoverPath);
	if (!hovering && !context.HoverPath3d.empty())
	{
		hoverPath = context.HoverPath3d;
		hovering = childResolver(hoverPath);
	}
	if (!hovering)
		hoverPath.clear();

	captured.HoverPath = std::move(hoverPath);
	_ctrlOverlayContext = std::move(captured);
}

float QuantisationInteractionController::_CtrlHandleAlpha(Time now) const
{
	if (_ctrlHandleHeld)
		return 1.0f;
	if (Timer::IsZero(_ctrlHandleReleasedAt))
		return 0.0f;
	static constexpr double FadeSeconds = 0.5;
	const auto elapsed = Timer::GetElapsedSeconds(_ctrlHandleReleasedAt, now);
	if (elapsed >= FadeSeconds)
		return 0.0f;
	return static_cast<float>(1.0 - elapsed / FadeSeconds);
}

void QuantisationInteractionController::_ApplyCtrlHandleAlpha(float alpha)
{
	_overlay.SetAlpha(alpha);
	if ((alpha <= 0.001f) && !_ctrlHandleHeld)
		_ctrlOverlayContext = std::nullopt;
}

int QuantisationInteractionController::_VisibleButtonCount(const QuantisationInteractionContext& context) const
{
	if (_ctrlOverlayContext.has_value())
		return _ctrlOverlayContext->VisibleButtonCount;

	switch (_SelectDepth(context))
	{
	case base::SelectDepth::DEPTH_LOOPTAKE:
	case base::SelectDepth::DEPTH_LOOP:
		return 3;
	case base::SelectDepth::DEPTH_STATION:
		return 2;
	default:
		return 1;
	}
}

SelectDepth QuantisationInteractionController::_SelectDepth(const QuantisationInteractionContext& context) const noexcept
{
	if (_ctrlOverlayContext.has_value())
		return _ctrlOverlayContext->SelectDepth;

	return context.SelectDepth;
}

std::shared_ptr<GuiElement> QuantisationInteractionController::_HoverElement(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	if (_ctrlOverlayContext.has_value())
		return childResolver(_ctrlOverlayContext->HoverPath);

	auto hovering = childResolver(context.HoverPath);
	if (!hovering && !context.HoverPath3d.empty())
		hovering = childResolver(context.HoverPath3d);
	return hovering;
}

ActionResult QuantisationInteractionController::_BeginMidiPhaseDrag(TouchAction action,
	MidiPhaseDragRoute route,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_isMidiPhaseDragging = true;
	_midiPhaseDragStartPosition = action.Position;
	_midiPhaseDragTarget = _ResolveMidiPhaseDragTarget(route, context, childResolver);
	_midiPhaseDragStartOffsetSamps = _MidiPhaseOffsetForTarget(_midiPhaseDragTarget);
	_quantisation.SetOverlayHeld(true);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantisationInteractionController::_UpdateMidiPhaseDrag(TouchMoveAction action,
	unsigned int sampleRate)
{
	const auto delta = action.Position - _midiPhaseDragStartPosition;
	const auto offsetSamps = Quantisation::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
		delta.X,
		sampleRate);
	std::cout << "Ctrl overlay drag: mode=phase deltaX=" << delta.X
		<< " deltaY=" << delta.Y
		<< " phaseOffsetSamps=" << offsetSamps
		<< std::endl;
	_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantisationInteractionController::_EndMidiPhaseDrag(TouchAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
	{
		const auto delta = action.Position - _midiPhaseDragStartPosition;
		const auto offsetSamps = Quantisation::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
			delta.X,
			sampleRate);
		_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	}

	_isMidiPhaseDragging = false;
	_midiPhaseDragTarget = MidiPhaseDragTarget{};
	_quantisation.SetOverlayHeld(false);
	_quantisation.PulseOverlay();
	_quantisation.ApplyOverlayAlpha(_quantisation.OverlayAlpha(Timer::GetTime()), _stations);

	return ActionResult::NoAction();
}

ActionResult QuantisationInteractionController::_BeginFractionDrag(TouchAction action,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_fractionDragTargets = _ResolveFractionDragTargets(context, childResolver);
	if (_fractionDragTargets.empty())
	{
		std::cout << "Ctrl overlay handle down: mode=fraction target=none" << std::endl;
		return ActionResult::NoAction();
	}

	_fractionDragStartY = action.Position.Y;
	_fractionDragMoved = false;
	_fractionDragTake.reset();
	_fractionDragStartFraction = _fractionDragTargets.front()->MidiQuantisation().Fraction;

	if (_fractionDragTargets.size() == 1u)
	{
		auto take = _fractionDragTargets.front();
		auto res = take->BeginMidiQuantisationGesture(action);
		if (!res.IsEaten)
		{
			_fractionDragTargets.clear();
			return res;
		}

		_isFractionDragging = true;
		_fractionDragTake = take;
		const auto quantisation = take->MidiQuantisation();
		std::cout << "Ctrl overlay handle down: mode=fraction take=" << take->Id()
			<< " startFraction=" << midi::MidiQuantisation::FractionLabel(quantisation.Fraction)
			<< std::endl;
		return res;
	}

	_isFractionDragging = true;
	std::cout << "Ctrl overlay handle down: mode=fraction takes=" << _fractionDragTargets.size()
		<< " startFraction=" << midi::MidiQuantisation::FractionLabel(_fractionDragStartFraction)
		<< std::endl;

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantisationInteractionController::_UpdateFractionDrag(TouchMoveAction action)
{
	if (!_isFractionDragging)
		return ActionResult::NoAction();

	_fractionDragMoved = true;

	if (_fractionDragTake)
	{
		const auto startFraction = _fractionDragTake->MidiQuantisation().Fraction;
		const auto startLabel = midi::MidiQuantisation::FractionLabel(startFraction);
		const auto deltaY = action.Position.Y - _fractionDragStartY;
		auto res = _fractionDragTake->OnAction(action);
		const auto updatedFraction = _fractionDragTake->MidiQuantisation().Fraction;
		std::cout << "Ctrl overlay drag: mode=fraction deltaY=" << deltaY
			<< " fraction=" << startLabel
			<< "->" << midi::MidiQuantisation::FractionLabel(updatedFraction)
			<< std::endl;
		return res;
	}

	if (_fractionDragTargets.empty())
		return ActionResult::NoAction();

	const auto deltaY = action.Position.Y - _fractionDragStartY;
	const auto fraction = midi::MidiQuantisation::ResolveDragFraction(_fractionDragStartFraction,
		deltaY);
	for (const auto& take : _fractionDragTargets)
	{
		if (!take)
			continue;

		auto settings = take->MidiQuantisation();
		settings.Enabled = true;
		settings.Fraction = fraction;
		take->SetMidiQuantisation(settings);
	}

	std::cout << "Ctrl overlay drag: mode=fraction deltaY=" << deltaY
		<< " fraction=" << midi::MidiQuantisation::FractionLabel(_fractionDragStartFraction)
		<< "->" << midi::MidiQuantisation::FractionLabel(fraction)
		<< " targets=" << _fractionDragTargets.size()
		<< std::endl;

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantisationInteractionController::_EndFractionDrag(TouchAction action)
{
	auto take = _fractionDragTake;
	auto targets = _fractionDragTargets;
	const auto moved = _fractionDragMoved;
	_isFractionDragging = false;
	_fractionDragStartY = 0;
	_fractionDragTake.reset();
	_fractionDragTargets.clear();
	_fractionDragMoved = false;

	if (take)
		return take->OnAction(action);

	if (targets.empty())
		return ActionResult::NoAction();

	if (!moved)
	{
		for (const auto& target : targets)
		{
			if (!target)
				continue;

			auto settings = target->MidiQuantisation();
			settings.Enabled = !settings.Enabled;
			target->SetMidiQuantisation(settings);
		}
	}

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

void QuantisationInteractionController::_ForEachTake(const std::function<void(const std::shared_ptr<Station>& station,
	const std::shared_ptr<LoopTake>& take)>& visit) const
{
	for (const auto& station : _stations)
	{
		if (!station)
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			visit(station, take);
		}
	}
}

std::shared_ptr<Station> QuantisationInteractionController::_StationForTake(const std::shared_ptr<LoopTake>& take) const
{
	if (!take)
		return nullptr;

	std::shared_ptr<Station> resolved;
	_ForEachTake([&resolved, &take](const std::shared_ptr<Station>& station,
		const std::shared_ptr<LoopTake>& candidate) {
		if (!resolved && (candidate == take))
			resolved = station;
	});
	return resolved;
}

std::shared_ptr<LoopTake> QuantisationInteractionController::_TakeForLoop(const std::shared_ptr<Loop>& loop) const
{
	if (!loop)
		return nullptr;

	std::shared_ptr<LoopTake> resolved;
	_ForEachTake([&resolved, &loop](const std::shared_ptr<Station>&,
		const std::shared_ptr<LoopTake>& candidateTake) {
		if (resolved)
			return;
		const auto& loops = candidateTake->GetLoops();
		if (std::find(loops.begin(), loops.end(), loop) != loops.end())
			resolved = candidateTake;
	});
	return resolved;
}

std::shared_ptr<LoopTake> QuantisationInteractionController::_TakeFromElement(const std::shared_ptr<GuiElement>& element) const
{
	if (!element)
		return nullptr;

	auto take = std::dynamic_pointer_cast<LoopTake>(element);
	if (take)
		return take;

	return _TakeForLoop(std::dynamic_pointer_cast<Loop>(element));
}

std::shared_ptr<LoopTake> QuantisationInteractionController::_FirstTakeForStation(const std::shared_ptr<Station>& station) const
{
	if (!station)
		return nullptr;

	for (const auto& take : station->GetLoopTakes())
	{
		if (take)
			return take;
	}

	return nullptr;
}

std::vector<std::shared_ptr<LoopTake>> QuantisationInteractionController::_ResolveFractionDragTargets(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		auto hovering = _HoverElement(context, childResolver);
		auto station = std::dynamic_pointer_cast<Station>(hovering);
		if (!station)
			station = _StationForTake(std::dynamic_pointer_cast<LoopTake>(hovering));
		if (!station)
			station = _StationForTake(_TakeForLoop(std::dynamic_pointer_cast<Loop>(hovering)));

		if (!station || station->IsRemote())
			return {};

		std::vector<std::shared_ptr<LoopTake>> targets;
		for (const auto& take : station->GetLoopTakes())
		{
			if (take)
				targets.push_back(take);
		}
		return targets;
	}

	auto take = _ResolveFractionDragTake(context, childResolver);
	if (!take)
		return {};

	return { take };
}

std::shared_ptr<LoopTake> QuantisationInteractionController::_ResolveFractionDragTake(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	auto take = _TakeFromElement(_HoverElement(context, childResolver));
	if (take)
		return take;

	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		auto hovering = _HoverElement(context, childResolver);
		auto station = std::dynamic_pointer_cast<Station>(hovering);
		if (!station)
			station = _StationForTake(std::dynamic_pointer_cast<LoopTake>(hovering));
		if (!station)
			station = _StationForTake(_TakeForLoop(std::dynamic_pointer_cast<Loop>(hovering)));

		if (auto takeFromStation = _FirstTakeForStation(station))
			return takeFromStation;

		for (const auto& candidateStation : _stations)
		{
			if (candidateStation && candidateStation->IsSelected())
			{
				if (auto selectedTake = _FirstTakeForStation(candidateStation))
					return selectedTake;
			}
		}
	}

	std::shared_ptr<LoopTake> selectedTake;
	_ForEachTake([&selectedTake, depth](const std::shared_ptr<Station>&,
		const std::shared_ptr<LoopTake>& candidateTake) {
		if (selectedTake)
			return;

		if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) && candidateTake->IsSelected())
		{
			selectedTake = candidateTake;
			return;
		}

		if (depth == base::SelectDepth::DEPTH_LOOP)
		{
			for (const auto& loop : candidateTake->GetLoops())
			{
				if (loop && loop->IsSelected())
				{
					selectedTake = candidateTake;
					return;
				}
			}
		}
	});
	return selectedTake;
}

QuantisationInteractionController::MidiPhaseDragTarget QuantisationInteractionController::_ResolveMidiPhaseDragTarget(MidiPhaseDragRoute route,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	MidiPhaseDragTarget target;
	if (route == MidiPhaseDragRoute::Global)
		return target;

	auto hovering = _HoverElement(context, childResolver);
	if (!hovering)
		return target;

	auto addTakeTarget = [](std::vector<std::shared_ptr<LoopTake>>& targets,
		const std::shared_ptr<LoopTake>& take) {
		if (!take)
			return;
		if (std::find(targets.begin(), targets.end(), take) == targets.end())
			targets.push_back(take);
	};

	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
		return target;

	auto take = std::dynamic_pointer_cast<LoopTake>(hovering);
	if (!take)
		take = _TakeForLoop(std::dynamic_pointer_cast<Loop>(hovering));

	if (take)
	{
		target.Kind = MidiPhaseDragTargetKind::LoopTake;
		target.TakeRef = std::move(take);
		addTakeTarget(target.TakeTargets, target.TakeRef);
	}

	_ForEachTake([&target, &addTakeTarget, depth](const std::shared_ptr<Station>&,
		const std::shared_ptr<LoopTake>& candidateTake) {
		if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) && candidateTake->IsSelected())
			addTakeTarget(target.TakeTargets, candidateTake);

		if (depth == base::SelectDepth::DEPTH_LOOP)
		{
			for (const auto& loop : candidateTake->GetLoops())
			{
				if (loop && loop->IsSelected())
				{
					addTakeTarget(target.TakeTargets, candidateTake);
					break;
				}
			}
		}
	});

	return target;
}

std::int32_t QuantisationInteractionController::_MidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target) const noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::Station:
		return target.StationRef ? target.StationRef->StationPhaseOffsetSamps() : 0;
	case MidiPhaseDragTargetKind::LoopTake:
		return target.TakeRef ? target.TakeRef->MidiQuantisation().PhaseOffsetSamps : 0;
	case MidiPhaseDragTargetKind::Global:
	default:
		return _quantisation.GlobalPhaseOffsetSamps();
	}
}

void QuantisationInteractionController::_SetMidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target,
	std::int32_t offsetSamps) noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::Station:
		for (const auto& station : target.StationTargets)
		{
			if (station)
				station->SetStationPhaseOffsetSamps(offsetSamps);
		}
		break;
	case MidiPhaseDragTargetKind::LoopTake:
		for (const auto& take : target.TakeTargets)
		{
			if (!take)
				continue;

			auto settings = take->MidiQuantisation();
			settings.PhaseOffsetSamps = offsetSamps;
			take->SetMidiQuantisation(settings);
		}
		break;
	case MidiPhaseDragTargetKind::Global:
	default:
		_quantisation.SetGlobalPhaseOffsetSamps(offsetSamps, _stations);
		break;
	}
}
