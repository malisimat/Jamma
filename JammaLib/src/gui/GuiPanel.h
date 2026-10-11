#pragma once

#include <memory>
#include <algorithm>
#include "GuiElement.h"
#include "ActionReceiver.h"

namespace gui
{
	struct GuiStyle
	{
		static glm::vec3 Graphite() { return { 0.18f, 0.20f, 0.23f }; }
		static glm::vec3 Control() { return { 1.0f, 0.7f, 0.2f }; }
		static glm::vec3 Edge() { return { 0.66f, 0.77f, 0.80f }; }
		static constexpr int StatusBarHeight = 28;
		static int StatusBarWidth(int viewportWidth) { return std::max(0, viewportWidth); }
		// Reserve tempo first so narrow windows lose messages before BPM/BPI.
		// 19px Inter: widest three-digit BPM/BPI text fits with 8px padding.
		static constexpr int TempoColumnMaxWidth = 160;
		static int TempoColumnWidth(int barWidth) { return std::min(TempoColumnMaxWidth, barWidth); }
		static int VersionColumnWidth(int barWidth) { return std::min(160, std::max(0, barWidth - TempoColumnWidth(barWidth))); }
		static int StatusColumnWidth(int barWidth) { return std::min(220, std::max(0, barWidth - TempoColumnWidth(barWidth) - VersionColumnWidth(barWidth))); }
		static int MessageColumnWidth(int barWidth) { return std::max(0, barWidth - TempoColumnWidth(barWidth) - StatusColumnWidth(barWidth) - VersionColumnWidth(barWidth)); }
		static constexpr float PanelFillOpacity = 0.80f;
		static constexpr float HudFillOpacity = 0.48f;
		static constexpr float HudBorderOpacity = 0.50f;
		static constexpr float PanelTransitionSeconds = 0.22f;
		static constexpr unsigned int ControlHeight = 36u;
		static constexpr unsigned int TextPadding = 8u;
	};
	// UI-owned binding adapter. Tree indices stay local; command identity stays
	// explicit, and the weak owner prevents a binding extending Scene lifetime.
	class GuiCommandReceiver : public base::ActionReceiver
	{
	public:
		GuiCommandReceiver(unsigned int commandIndex, std::weak_ptr<base::ActionReceiver> owner = {}) :
			_commandIndex(commandIndex), _owner(std::move(owner)) {}
		void SetOwner(std::weak_ptr<base::ActionReceiver> owner) { _owner = std::move(owner); }
		using base::ActionReceiver::OnAction;
		actions::ActionResult OnAction(actions::GuiAction action) override;
	private:
		unsigned int _commandIndex;
		std::weak_ptr<base::ActionReceiver> _owner;
	};

	class GuiPanel :
		public base::GuiElement
	{
	public:
		GuiPanel(base::GuiElementParams guiParams);

	public:
		void AddChild(std::shared_ptr<base::GuiElement> child) override;
		bool RemoveChild(const std::shared_ptr<base::GuiElement>& child);
	};
}
