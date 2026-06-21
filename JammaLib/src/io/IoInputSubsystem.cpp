#include "stdafx.h"
#include "IoInputSubsystem.h"

#include <iostream>

using namespace engine;

namespace io
{
	HHOOK IoInputSubsystem::_globalInsertHook = nullptr;
	std::atomic<bool> IoInputSubsystem::_globalInsertDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalInsertLastDispatchedDown{ false };

	IoInputSubsystem::IoInputSubsystem(io::UserConfig userConfig, io::LoggingConfig loggingConfig) :
		_userConfig(userConfig),
		_loggingConfig(loggingConfig)
	{
	}

	IoInputSubsystem::~IoInputSubsystem()
	{
		Close();
	}

	void IoInputSubsystem::Init(std::atomic<std::uint64_t>& audioSampleCounter,
		std::atomic<std::int64_t>& midiAnchorMicros)
	{
		_midiRouter.InitMidi(_userConfig, _loggingConfig, audioSampleCounter, midiAnchorMicros);
		_midiRouter.InitSerial(_userConfig);
	}

	void IoInputSubsystem::Close()
	{
		CloseGlobalInsertCapture();
		_midiRouter.CloseSerial();
		_midiRouter.CloseMidi();
	}

	bool IoInputSubsystem::InitGlobalInsertCapture()
	{
		if (_globalInsertHook)
			return true;

		const auto down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
		_globalInsertDown.store(down, std::memory_order_release);
		_globalInsertLastDispatchedDown.store(down, std::memory_order_release);

		_globalInsertHook = SetWindowsHookEx(WH_KEYBOARD_LL, _LowLevelKeyboardProc,
			GetModuleHandle(nullptr), 0);
		if (!_globalInsertHook)
		{
			std::cerr << "[Input] Global insert hook install failed, error="
				<< GetLastError() << std::endl;
			return false;
		}

		return true;
	}

	void IoInputSubsystem::CloseGlobalInsertCapture()
	{
		if (!_globalInsertHook)
			return;

		UnhookWindowsHookEx(_globalInsertHook);
		_globalInsertHook = nullptr;
	}

	bool IoInputSubsystem::PumpGlobalInsertCapture(actions::KeyAction& action) noexcept
	{
		const auto down = _globalInsertDown.load(std::memory_order_acquire);
		const auto last = _globalInsertLastDispatchedDown.load(std::memory_order_acquire);
		if (down == last)
			return false;

		_globalInsertLastDispatchedDown.store(down, std::memory_order_release);
		action.KeyChar = VK_INSERT;
		action.KeyActionType = down ? actions::KeyAction::KEY_DOWN : actions::KeyAction::KEY_UP;
		action.IsSystem = false;
		action.Modifiers = base::Action::MODIFIER_NONE;
		return true;
	}

	LRESULT CALLBACK IoInputSubsystem::_LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept
	{
		if (nCode == HC_ACTION)
		{
			const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
			if (kb && kb->vkCode == VK_INSERT)
			{
				if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
					_globalInsertDown.store(true, std::memory_order_release);
				else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
					_globalInsertDown.store(false, std::memory_order_release);
			}
		}

		return CallNextHookEx(_globalInsertHook, nCode, wParam, lParam);
	}

	IoInputSubsystem::PumpResult IoInputSubsystem::PumpMidi(std::vector<std::shared_ptr<Station>>& stations,
		std::uint64_t audioSampleCounter,
		const audio::AudioStreamParams& streamParams,
		std::mutex& audioMutex)
	{
		std::scoped_lock lock(audioMutex);
		PumpResult result;
		auto midiSummary = _midiRouter.PumpMidi(stations,
			static_cast<std::uint32_t>(audioSampleCounter),
			_userConfig,
			streamParams);
		if (midiSummary.Activated) result.Activated = true;
		if (midiSummary.Ditched) result.Ditched = true;
		return result;
	}

	IoInputSubsystem::PumpResult IoInputSubsystem::PumpSerial(std::vector<std::shared_ptr<Station>>& stations,
		const audio::AudioStreamParams& streamParams,
		std::mutex& audioMutex)
	{
		std::scoped_lock lock(audioMutex);
		PumpResult result;
		auto serialSummary = _midiRouter.PumpSerial(stations, _userConfig, streamParams);
		if (serialSummary.Activated) result.Activated = true;
		if (serialSummary.Ditched) result.Ditched = true;
		return result;
	}

	actions::ActionResult IoInputSubsystem::HandleAutomationKey(const actions::KeyAction& action,
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		const std::vector<unsigned char>& hoverPath,
		const std::shared_ptr<engine::LoopTake>& hoveredTake)
	{
		return _midiRouter.HandleAutomationKey(action, stations, hoverPath, hoveredTake);
	}

	void IoInputSubsystem::RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger)
	{
		_midiRouter.RegisterTrigger(deviceName, std::move(trigger));
	}
}
