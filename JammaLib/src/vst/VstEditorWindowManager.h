#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include "../actions/ActionResult.h"
#include "../base/LoggingConfig.h"
#include "../base/GuiElement.h"
#include "../graphics/VstEditorWindow.h"
#include "../engine/Station.h"

namespace vst
{
	// Window/plugin operations and queries belong to the UI thread. The mutex
	// protects bookkeeping only; never hold it across native or plugin calls.
	class VstEditorWindowManager
	{
	public:
		VstEditorWindowManager() = default;
		~VstEditorWindowManager();

		void CloseAllVstEditorWindows();
		void CloseVstEditorsForStation(const std::shared_ptr<engine::Station>& station);
		void CloseVstEditorsForTake(const std::shared_ptr<engine::LoopTake>& take);
		void PruneClosedVstEditorWindows();

		actions::ActionResult HandleVstInsert(const std::wstring& pluginPath,
			base::SelectDepth depth,
			const std::shared_ptr<base::GuiElement>& hovering,
			const std::function<void()>& onCommit);

		actions::ActionResult HandleVstEditorOpen(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
		void SetLogging(base::LoggingConfig config) noexcept;

		bool OpenVstEditorForPlugin(const std::shared_ptr<IVstPlugin>& plugin);
		bool IsVstEditorOpen(const std::shared_ptr<IVstPlugin>& plugin) const;
		bool CloseVstEditorForPlugin(const std::shared_ptr<IVstPlugin>& plugin);
		bool ToggleVstEditorForPlugin(const std::shared_ptr<IVstPlugin>& plugin);
		bool TryOpenVstEditorForLoop(const std::shared_ptr<engine::Loop>& loop, size_t pluginIndex);
		bool TryOpenVstEditorForStation(const std::shared_ptr<engine::Station>& station, size_t pluginIndex);
		bool TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			size_t pluginIndex);

		static actions::ActionResult EatAction();

	private:
		base::LoggingConfig _loggingConfig;
		mutable std::mutex _vstEditorWindowsMutex;
		std::vector<std::shared_ptr<graphics::VstEditorWindow>> _vstEditorWindows;
		std::vector<std::weak_ptr<graphics::VstEditorWindow>> _closingEditorWindows;
		// Reservations survive native message pumping during editor creation.
		std::vector<const IVstPlugin*> _pendingEditorOpens;
		// UI owner only, protected alongside window bookkeeping. A close pumped
		// during OpenEditor cancels publication without touching processor state.
		std::vector<const IVstPlugin*> _cancelledEditorOpens;
		unsigned int _closeAllDepth = 0u;
	};
}
