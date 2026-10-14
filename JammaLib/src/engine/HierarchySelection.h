#pragma once

#include <memory>
#include <vector>
#include "Station.h"

namespace engine
{
	class Scene;

	// Committed hierarchy selection under the caller's existing scene lock.
	// No gesture recognition, rack/editor coordination, callbacks, or locking.
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

		static std::shared_ptr<base::GuiElement> Resolve(const LockedStations& view, const std::vector<unsigned char>& path);
		static void SetSelected(const LockedStations& view, const std::shared_ptr<base::GuiElement>& target,
			base::SelectDepth depth, bool selected);
		static void Clear(const LockedStations& view);
		static void SetMuted(const LockedStations& view, const std::vector<unsigned char>& path, bool muted);
		static base::Tweakable::TweakState TweakState(const LockedStations& view,
			const std::shared_ptr<base::GuiElement>& target);
	};
}
