#pragma once

#include <functional>
#include <optional>
#include "GuiRack.h"
#include "GuiVstRack.h"
#include "RuntimeDialogs.h"
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
			std::function<RuntimeDialogDecision(std::string, std::string, std::string)> Confirm;
			std::function<void(const std::wstring&)> ShowError;
		};

		explicit VstRackControls(Host host) : _host(std::move(host)) {}
		~VstRackControls() { Reset(); }
		void UpdateLocked(base::SelectDepth depth);
		void Pump();
		void Reset();

	private:
		struct Target
		{
			std::shared_ptr<engine::Station> Station;
			std::shared_ptr<engine::LoopTake> Take;
			std::shared_ptr<engine::Loop> Loop;
			std::string PluginId;
		};
		struct Intent
		{
			Target Owner;
			base::Action::Modifiers Modifiers;
			std::optional<GuiVstRack::Command> RackCommand;
		};
		bool _AvailableLocked(const Target& target) const;
		struct Confirmation
		{
			Intent Captured;
			vst::VstRackSlot Slot;
			std::uint64_t RackRevision;
			RuntimeDialogDecision Decision;
		};
		void _RequestConfirmation(const Intent& intent, vst::VstRackSlot slot, std::uint64_t revision,
			std::string title, std::string message, std::string accept);
		void _CompleteConfirmation();
		void _ShowError(const std::wstring& message) const;
		vst::VstRackSlot _SlotLocked(const Target& target) const;
		bool _LoadLocked(const Target& target, const std::wstring& path);
		bool _UnloadLocked(const Target& target, const std::string& identity);
		void _PresentLocked(const std::shared_ptr<GuiRack>& rack, Target target, const std::string& label);
		void _PresentStationLocked(const std::shared_ptr<engine::Station>& station);
		void _PumpStation(const Intent& intent);
		Host _host;
		// UI thread only. An intent owns the exact hierarchy before any modal pump.
		std::optional<Intent> _intent;
		std::optional<Confirmation> _confirmation;
		std::vector<Target> _loads;
		bool _busy = false;
		std::shared_ptr<int> _lifetime = std::make_shared<int>(0);
	};
}
