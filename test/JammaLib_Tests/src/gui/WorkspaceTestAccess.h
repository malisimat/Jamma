#pragma once

#include "../../../../JammaLib/src/gui/Workspace.h"

namespace gui
{
	class WorkspaceTestAccess
	{
	public:
		template<class T> static decltype(auto) Camera(T& owner) { return (owner._camera); }
		static void SetProjection(Workspace& owner, glm::mat4 projection) { owner._viewProj = projection; }
		static void ReplaceHud(Workspace& owner, std::shared_ptr<GuiHud> hud)
		{
			for (auto& child : owner._guiChildren) if (child == owner._hudPanel) child = hud;
			owner._hudPanel = std::move(hud);
		}
	};
}
