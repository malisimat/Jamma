#pragma once

#include <memory>
#include <array>
#include <vector>
#include "Station.h"

namespace engine
{
	class Scene;

	// Committed hierarchy selection under the caller's existing scene lock.
	// Applies resulting hierarchy presentation too; no gesture recognition,
	// rack/editor coordination, callbacks, or locking.
	class HierarchySelection
	{
	public:
		class LockedStations
		{
			friend class Scene;
			friend class HierarchySelection;
			// Scene must retain _sceneMutex throughout every use of this borrowed view.
			explicit LockedStations(const std::vector<std::shared_ptr<Station>>& stations) : Stations(stations) {}
			const std::vector<std::shared_ptr<Station>>& Stations;
		};

		static LockedStations BorrowLocked(const std::vector<std::shared_ptr<Station>>& stations) { return LockedStations(stations); }

		struct Appearance
		{
			const std::vector<unsigned char>& HoverPath;
			bool ClearPicking;
			bool HighlightHover;
			bool HighlightMidiSiblings;
			bool ClickPressed;
			bool MutePressed;
		};
		enum class Edit { Select, Deselect, Mute, Unmute };
		struct TargetChange { Edit Operation; std::vector<unsigned char> Path; };
		struct EditBatch
		{
			bool ClearSelection = false;
			base::SelectDepth Depth = base::DEPTH_STATION;
			std::array<TargetChange, 2> Changes{};
			std::size_t ChangeCount = 0u;
			Appearance Presentation;
		};
		struct Outcome { bool CollapseRacks = false; };
		static Outcome ApplyLocked(const LockedStations& view, const EditBatch& batch);
		static void ApplyAppearance(const LockedStations& view, const Appearance& appearance);
		static std::shared_ptr<base::GuiElement> Resolve(const LockedStations& view, const std::vector<unsigned char>& path);
		static void SetSelected(const LockedStations& view, const std::shared_ptr<base::GuiElement>& target,
			base::SelectDepth depth, bool selected);
		static void SelectAllLocal(const LockedStations& view, base::SelectDepth depth);
		static void Clear(const LockedStations& view);
		static bool HasSelection(const LockedStations& view);
		static void SetMuted(const LockedStations& view, const std::vector<unsigned char>& path, bool muted);
		static base::Tweakable::TweakState TweakState(const LockedStations& view,
			const std::shared_ptr<base::GuiElement>& target);
	};
}
