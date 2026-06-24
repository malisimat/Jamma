#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include "../actions/ActionResult.h"
#include "../base/GuiElement.h"
#include "../graphics/VstEditorWindow.h"
#include "../engine/Station.h"

namespace vst
{
	class VstEditorWindowManager
	{
	public:
		VstEditorWindowManager() = default;
		~VstEditorWindowManager();

		void CloseAllVstEditorWindows();
		void PruneClosedVstEditorWindows();

		actions::ActionResult HandleVstInsert(const std::wstring& pluginPath,
			base::SelectDepth depth,
			const std::shared_ptr<base::GuiElement>& hovering,
			const std::function<void()>& onCommit);

		actions::ActionResult HandleVstEditorOpen(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			const std::vector<std::shared_ptr<engine::Station>>& stations);

		bool OpenVstEditorForPlugin(const std::shared_ptr<IVstPlugin>& plugin);
		bool TryOpenVstEditorForLoop(const std::shared_ptr<engine::Loop>& loop, size_t pluginIndex);
		bool TryOpenVstEditorForStation(const std::shared_ptr<engine::Station>& station, size_t pluginIndex);
		bool TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			size_t pluginIndex);

		static actions::ActionResult EatAction();

	private:
		mutable std::mutex _vstEditorWindowsMutex;
		std::vector<std::unique_ptr<graphics::VstEditorWindow>> _vstEditorWindows;
	};
}
