#pragma once

#include <vector>
#include <memory>
#include <functional>
#include "../actions/ActionResult.h"
#include "../base/GuiElement.h"
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

		actions::ActionResult HandleVstInsert(const std::wstring& pluginPath,
			base::SelectDepth depth,
			const std::shared_ptr<base::GuiElement>& hovering,
			const std::function<void()>& onCommit);

		actions::ActionResult HandleVstEditorOpen(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			const std::vector<std::shared_ptr<Station>>& stations);

		bool OpenVstEditorForPlugin(const std::shared_ptr<vst::IVstPlugin>& plugin);
		bool TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex);
		bool TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex);
		bool TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			size_t pluginIndex);

		static actions::ActionResult EatAction();

	private:
		std::vector<std::unique_ptr<graphics::VstEditorWindow>> _vstEditorWindows;
	};
}
