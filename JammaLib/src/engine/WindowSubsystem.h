#pragma once

#include <vector>
#include <memory>
#include "../graphics/VstEditorWindow.h"
#include "Station.h"

namespace engine
{
	class WindowSubsystem
	{
	public:
		WindowSubsystem() = default;
		~WindowSubsystem();

		void CloseAllVstEditorWindows();
		void PruneClosedVstEditorWindows();

		bool OpenVstEditorForPlugin(const std::shared_ptr<vst::IVstPlugin>& plugin);
		bool TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex);
		bool TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex);
		bool TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			size_t pluginIndex);

	private:
		std::vector<std::unique_ptr<graphics::VstEditorWindow>> _vstEditorWindows;
	};
}
