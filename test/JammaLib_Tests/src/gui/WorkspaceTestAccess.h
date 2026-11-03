#pragma once

#include "../../../../JammaLib/src/gui/Workspace.h"

namespace gui
{
	class WorkspaceTestAccess
	{
	public:
		template<class T> static decltype(auto) Camera(T& owner) { return (owner._camera); }
		static std::shared_ptr<engine::LoopTake> OverdubHover(const Workspace& owner)
		{ return owner._overdubSourceDrag ? owner._overdubSourceDrag->Hover.lock() : nullptr; }
		static auto OverdubPreview(const Workspace& owner, const std::vector<std::shared_ptr<engine::Station>>& stations)
		{ return owner._OverdubSourcePreviewMembersLocked(stations); }
        static auto MarqueeTargets(const Workspace& owner, const std::vector<std::shared_ptr<engine::Station>>& stations,
            utils::Rect2d rectangle, base::SelectDepth depth)
        { return owner._MuteMarqueeTargetsLocked(stations,rectangle,depth); }
		static bool HasCapture(const Workspace& owner) { return owner._pointer.HasCapture(); }
		static void SetProjection(Workspace& owner, glm::mat4 projection) { owner._viewProj = projection; }
		static void ReplaceHud(Workspace& owner, std::shared_ptr<GuiHud> hud)
		{
			for (auto& child : owner._guiChildren) if (child == owner._hudPanel) child = hud;
			owner._hudPanel = std::move(hud);
		}
	};
}
