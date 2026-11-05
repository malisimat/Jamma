#pragma once
#include "base/DrawContext.h"
#include "gui/GuiHud.h"
namespace gui
{
	class GuiHudTestAccess
	{
	public:
		static void SetSubmitRigEdit(GuiHud& hud, std::function<bool(const io::RigFile&)> submit)
		{ hud._submitRigEdit = std::move(submit); }
		static std::shared_ptr<GuiButton> TriggerClose(GuiHud& hud, std::size_t index)
		{ return hud._triggerWidgets.at(index).Close; }
		static void CableFrame(GuiHud& hud)
		{
			base::DrawContext context({1200u, 800u}, base::DrawContext::SCREEN);
			hud._DrawCables(context);
		}
		static void SetCableElapsed(GuiHud& hud, std::chrono::milliseconds elapsed)
		{ hud._lastCableFrame = std::chrono::steady_clock::now() - elapsed; }
		static const std::vector<glm::vec4>& CableColors(const GuiHud& hud) { return hud._cableColors; }
		static void MarkCableCache(GuiHud& hud) { hud._cableColors.push_back({9.0f, 0.0f, 0.0f, 0.0f}); }
		static std::chrono::steady_clock::time_point LastCableFrame(const GuiHud& hud) { return hud._lastCableFrame; }
		static void EditedCableReveal(GuiHud& hud, std::chrono::steady_clock::time_point until)
		{ hud._editedCableTrigger = 0u; hud._cableEditRevealUntil = until; hud._cablesDirty = true; }
	};
}
