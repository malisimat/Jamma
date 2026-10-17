#pragma once

#include <functional>
#include <optional>
#include "GuiRack.h"
#include "../vst/VstEditorWindowManager.h"

namespace gui
{
	// Window-thread presentation/gestures. Engine membership and slot access use
	// Scene's existing off-callback lock; native dialogs/editors never hold it.
	class VstRackControls
	{
	public:
		struct Host
		{
			std::mutex& Mutex;
			const std::vector<std::shared_ptr<engine::Station>>& Stations;
			vst::VstEditorWindowManager& Editors;
			std::function<bool()> EditsEnabled;
			std::function<void()> Commit;
			std::function<std::wstring()> PickPlugin;
			std::function<bool()> ConfirmRemoval;
			std::function<void(const std::wstring&)> ShowError;
		};

		explicit VstRackControls(Host host) : _host(std::move(host)) {}
		void UpdateLocked(base::SelectDepth depth);
		void Pump();
		void Reset();

	private:
		struct Target
		{
			std::shared_ptr<engine::Station> Station;
			std::shared_ptr<engine::LoopTake> Take;
			std::shared_ptr<engine::Loop> Loop;
		};
		struct Intent
		{
			Target Owner;
			base::Action::Modifiers Modifiers;
		};
		bool _AvailableLocked(const Target& target) const;
		vst::VstRackSlot _SlotLocked(const Target& target) const;
		bool _LoadLocked(const Target& target, const std::wstring& path);
		bool _UnloadLocked(const Target& target, const std::string& identity);
		void _PresentLocked(const std::shared_ptr<GuiRack>& rack, Target target, const std::string& label);
		Host _host;
		// UI thread only. An intent owns the exact hierarchy before any modal pump.
		std::optional<Intent> _intent;
		std::vector<Target> _loads;
		bool _busy = false;
		std::shared_ptr<int> _lifetime = std::make_shared<int>(0);
	};
}
