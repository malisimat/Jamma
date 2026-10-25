#include "HierarchySelection.h"

using namespace engine;
using namespace base;

std::shared_ptr<GuiElement> HierarchySelection::Resolve(const LockedStations& view, const std::vector<unsigned char>& path)
{
	if (path.empty() || path.front() >= view.Stations.size()) return nullptr;
	std::shared_ptr<GuiElement> target = view.Stations[path.front()];
	for (std::size_t depth = 1u; target && depth < path.size() && path[depth] != 0xffu; ++depth)
		target = target->TryGetChild(path[depth]);
	return target;
}

void HierarchySelection::SetSelected(const LockedStations&, const std::shared_ptr<GuiElement>& target,
	SelectDepth depth, bool selected)
{
	if (!target) return;
	if (depth == base::DEPTH_STATION)
	{
		if (auto station = std::dynamic_pointer_cast<Station>(target))
		{
			if (selected) station->Select(); else station->DeSelect();
			for (const auto& take : station->GetLoopTakes())
				if (selected) take->Select(); else take->DeSelect();
			return;
		}
	}
	if (depth == base::DEPTH_LOOP)
	{
		if (auto take = std::dynamic_pointer_cast<LoopTake>(target->Parent()))
		{
			// A take keeps the aggregate selected state used by the editor, while
			// individual loop models carry the loop-depth visual selection.
			if (selected && !take->IsSelected())
			{
				take->Select();
				for (const auto& loop : take->GetLoops())
					if (loop != target) loop->DeSelect();
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model(); model && model != target)
						model->DeSelect();
			}
			if (selected) target->Select(); else target->DeSelect();
			if (!selected && take->IsSelected())
			{
				bool anySelected = false;
				for (const auto& loop : take->GetLoops()) anySelected |= loop->IsSelected();
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) anySelected |= model->IsSelected();
				if (!anySelected) take->DeSelect();
			}
			return;
		}
	}
	if (selected) target->Select();
	else target->DeSelect();
}

void HierarchySelection::Clear(const LockedStations& view)
{
	for (const auto& station : view.Stations)
	{
		station->DeSelect();
		for (const auto& take : station->GetLoopTakes())
		{
			take->DeSelect();
			for (const auto& loop : take->GetLoops()) loop->DeSelect();
			for (const auto& midiLoop : take->GetMidiLoops())
				if (auto model = midiLoop->Model()) model->DeSelect();
		}
	}
}

void HierarchySelection::SetMuted(const LockedStations& view, const std::vector<unsigned char>& path, bool muted)
{
	// Caller holds the scene lock across target resolution and membership access.
	// The existing mute flags are atomic for audio readers.
	auto target = Resolve(view, path);
	const auto setMuted = [muted](const std::shared_ptr<Tweakable>& tweakable) {
		if (!tweakable) return;
		if (muted) tweakable->Mute(); else tweakable->UnMute();
	};
	if (auto station = std::dynamic_pointer_cast<Station>(target))
	{
		for (const auto& take : station->GetLoopTakes()) setMuted(take);
	}
	else if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(target))
		setMuted(tweakable);
	else if (target)
		// MIDI streams share their owning take's mute and backing ring.
		setMuted(std::dynamic_pointer_cast<LoopTake>(target->Parent()));
}

base::Tweakable::TweakState HierarchySelection::TweakState(const LockedStations&, const std::shared_ptr<GuiElement>& target)
{
	if (auto station = std::dynamic_pointer_cast<Station>(target))
		return station->AllTakesMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(target))
		return tweakable->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	if (target)
		if (auto take = std::dynamic_pointer_cast<LoopTake>(target->Parent()))
			return take->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	return Tweakable::TWEAKSTATE_NONE;
}

void HierarchySelection::SelectAllLocal(const LockedStations& view, base::SelectDepth depth)
{
	for (const auto& station : view.Stations)
	{
		if (station->IsRemote()) continue;
		if (depth == base::DEPTH_STATION) station->Select(); else station->DeSelect();
		for (const auto& take : station->GetLoopTakes())
		{
			const bool hasSelectedLoop = std::any_of(take->GetLoops().begin(), take->GetLoops().end(),
				[](const auto& loop) { return loop->Model() != nullptr; }) ||
				std::any_of(take->GetMidiLoops().begin(), take->GetMidiLoops().end(),
					[](const auto& loop) { return loop->Model() != nullptr; });
			// Aggregate selection can fan to children, so set it before correcting
			// individual leaves; loop depth keeps only selectable take ownership.
			take->SetGroupSelected(depth != base::DEPTH_LOOP || hasSelectedLoop);
			for (const auto& loop : take->GetLoops())
			{
				const bool valid = loop->Model() != nullptr;
				if (valid) loop->Select(); else loop->DeSelect();
			}
			for (const auto& loop : take->GetMidiLoops())
				if (auto model = loop->Model())
				{
					model->Select();
				}
		}
	}
}

void HierarchySelection::ApplyAppearance(const LockedStations& view, const Appearance& appearance)
{
	// Presentation follows committed changes while the caller still holds the
	// hierarchy lock; no render observer sees intermediate click/paint shading.
	if (appearance.ClearPicking)
		for (const auto& station : view.Stations) station->SetPicking3d(false);
	if (appearance.HighlightHover)
		if (auto hovered = Resolve(view, appearance.HoverPath)) hovered->SetPicking3d(true);
	if (appearance.HighlightMidiSiblings)
	{
		if (auto hovered = Resolve(view, appearance.HoverPath))
		{
			if (auto take = std::dynamic_pointer_cast<LoopTake>(hovered->Parent()))
			{
				for (const auto& midiLoop : take->GetMidiLoops())
					if (midiLoop->Model() == hovered)
					{
						for (const auto& sibling : take->GetMidiLoops())
							if (auto model = sibling->Model()) model->SetPicking3d(true);
						break;
					}
			}
		}
	}
	// A click has a short-lived pressed appearance. Keep it separate from
	// persistent selection and hover, and clear it when the gesture becomes paint.
	for (const auto& station : view.Stations)
	{
		station->SetClickPressed(false);
		for (const auto& take : station->GetLoopTakes())
		{
			for (const auto& loop : take->GetLoops())
				if (auto model = loop->Model()) model->SetClickPressed(false);
			for (const auto& midiLoop : take->GetMidiLoops())
				if (auto model = midiLoop->Model()) model->SetClickPressed(false);
		}
	}
	if (appearance.ClickPressed)
	{
		if (auto pressed = Resolve(view, appearance.HoverPath))
		{
			const bool mutePressed = appearance.MutePressed;
			const auto pressTakeLoops = [mutePressed](const std::shared_ptr<LoopTake>& take) {
				for (const auto& loop : take->GetLoops())
					if (auto model = loop->Model()) model->SetClickPressed(true, mutePressed);
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) model->SetClickPressed(true, mutePressed);
			};
			if (auto station = std::dynamic_pointer_cast<Station>(pressed))
			{
				station->SetClickPressed(true, mutePressed);
				for (const auto& take : station->GetLoopTakes())
					pressTakeLoops(take);
			}
			else if (auto take = std::dynamic_pointer_cast<LoopTake>(pressed))
				pressTakeLoops(take);
			else if (auto loop = std::dynamic_pointer_cast<Loop>(pressed))
			{
				if (auto model = loop->Model()) model->SetClickPressed(true, mutePressed);
			}
			else if (auto take = std::dynamic_pointer_cast<LoopTake>(pressed->Parent()))
			{
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) model->SetClickPressed(true, mutePressed);
			}
		}
	}
}

bool HierarchySelection::HasSelection(const LockedStations& view)
{
	for (const auto& station : view.Stations)
	{
		if (!station)
			continue;

		if (station->IsSelected())
			return true;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			if (take->IsSelected())
				return true;

			for (const auto& loop : take->GetLoops())
			{
				if (loop && loop->IsSelected())
					return true;
			}
			for (const auto& loop : take->GetMidiLoops())
			{
				if (loop && loop->Model() && loop->Model()->IsSelected())
					return true;
			}
		}
	}

	return false;
}

HierarchySelection::Outcome HierarchySelection::ApplyLocked(const LockedStations& view, const EditBatch& batch)
{
	Outcome outcome{ batch.ClearSelection };
	if (batch.ClearSelection) Clear(view);
	for (std::size_t index = 0u; index < batch.ChangeCount; ++index)
	{
		const auto& change = batch.Changes[index];
		if (change.Operation == Edit::Select || change.Operation == Edit::Deselect)
		{
			if (auto target = Resolve(view, change.Path))
			{
				outcome.CollapseRacks = true;
				SetSelected(view, target, batch.Depth, change.Operation == Edit::Select);
			}
		}
		else SetMuted(view, change.Path, change.Operation == Edit::Mute);
	}
	ApplyAppearance(view, batch.Presentation);
	return outcome;
}
