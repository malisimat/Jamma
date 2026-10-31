#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "../base/GuiElement.h"
#include "GuiFocusManager.h"
#include "../actions/ActionUndoHistory.h"

namespace gui
{
	// Window-owned capture, panel dispatch and hover mechanics. The caller chooses
	// routing order/admission and holds any lock required by a target's callbacks.
	class PointerInteraction
	{
	public:
		enum class Owner { General, Hud, Settings };
		enum class Release { Explicit, AnyUp, LeftUp };
		enum class CaptureLoss { MouseZeroButtons, AnyZeroButtons };
		using HoverPath = std::vector<std::weak_ptr<base::GuiElement>>;
		bool HasCapture() const { return !_captured.expired(); }
		std::shared_ptr<base::GuiElement> Captured() const { return _captured.lock(); }
		bool IsCapturedBy(Owner owner) const { return _owner == owner; }
		void Capture(std::weak_ptr<base::GuiElement> target, Owner owner = Owner::General)
		{ _captured = std::move(target); _owner = owner; }
		void ReleaseCapture() { _captured.reset(); _owner = Owner::General; }
		bool ConsumesRelease() const { return _consumeSettingsRelease; }
		void ConsumeNextRelease() { _consumeSettingsRelease = true; }
		void ClearConsumedRelease() { _consumeSettingsRelease = false; }
		static bool IsPress(const actions::TouchAction& action);
		static void FocusPress(const std::shared_ptr<base::GuiElement>& active,
			GuiFocusManager& focus, bool finalizePrevious = false);
		actions::ActionResult DispatchCapture(actions::TouchAction action, Release release, bool eat);
		actions::ActionResult DispatchCapture(actions::TouchMoveAction action, CaptureLoss loss, bool eat);
		// Complete an already-dispatched root result after the caller releases its target lock.
		bool CompleteDispatch(const actions::TouchAction& action, const actions::ActionResult& result,
			Owner owner, GuiFocusManager* focus, actions::ActionUndoHistory& history);
		void OnSubtreeHidden(const std::shared_ptr<base::GuiElement>& subtree, GuiFocusManager& focus);
		std::optional<actions::ActionResult> RoutePanels(actions::TouchAction action,
			std::span<const std::shared_ptr<base::GuiElement>> panels, GuiFocusManager& focus, bool otherPointerOwner);
		std::optional<actions::ActionResult> RoutePanels(actions::TouchMoveAction action,
			std::span<const std::shared_ptr<base::GuiElement>> panels, bool otherPointerOwner);

		bool HoverDirty() const { return _hoverDirty; }
		void InvalidateHover() { _hoverDirty = true; }
		void HoverResolved() { _hoverDirty = false; }
		const HoverPath& HoverPath2d() const { return _hoverPath; }
		HoverPath& HoverScratch() { return _hoverScratch; }
		const auto& HoverElements() const { return _nextShared; }
		void ClearHoverPath() { _hoverPath.clear(); }
		void CommitHover(HoverPath path) { _hoverPath = std::move(path); _hoverDirty = false; }
		void ApplyHover(const HoverPath& next, utils::Position2d cursor);
		static bool ResolveHoverPath(std::span<const std::shared_ptr<base::GuiElement>> roots,
			bool reverse, utils::Position2d cursor, HoverPath& path);

	private:
		std::weak_ptr<base::GuiElement> _captured;
		Owner _owner = Owner::General;
		bool _consumeSettingsRelease = false;
		bool _hoverDirty = true;
		HoverPath _hoverPath, _hoverScratch;
		std::vector<std::shared_ptr<base::GuiElement>> _previousShared, _nextShared;
		static void _LockPath(const HoverPath& path, std::vector<std::shared_ptr<base::GuiElement>>& result);
	};
}
