#include "stdafx.h"
#include "IoInputSubsystem.h"

#include <iostream>

using namespace engine;

namespace io
{
	HHOOK IoInputSubsystem::_globalKeyHook = nullptr;
	std::atomic<bool> IoInputSubsystem::_globalInsertDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalInsertLastDispatchedDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalPageUpDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalPageUpLastDispatchedDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalPageDownDown{ false };
	std::atomic<bool> IoInputSubsystem::_globalPageDownLastDispatchedDown{ false };

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
		CloseGlobalKeyCapture();
		_midiRouter.CloseSerial();
		_midiRouter.CloseMidi();
	}

	bool IoInputSubsystem::InitGlobalKeyCapture()
	{
		if (_globalKeyHook)
			return true;

		const auto insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
		_globalInsertDown.store(insertDown, std::memory_order_release);
		_globalInsertLastDispatchedDown.store(insertDown, std::memory_order_release);

		const auto pageUpDown = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
		_globalPageUpDown.store(pageUpDown, std::memory_order_release);
		_globalPageUpLastDispatchedDown.store(pageUpDown, std::memory_order_release);

		const auto pageDownDown = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
		_globalPageDownDown.store(pageDownDown, std::memory_order_release);
		_globalPageDownLastDispatchedDown.store(pageDownDown, std::memory_order_release);

		_globalKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, _LowLevelKeyboardProc,
			GetModuleHandle(nullptr), 0);
		if (!_globalKeyHook)
		{
			std::cerr << "[Input] Global key hook install failed, error="
				<< GetLastError() << std::endl;
			return false;
		}

		return true;
	}

	void IoInputSubsystem::CloseGlobalKeyCapture()
	{
		if (!_globalKeyHook)
			return;

		UnhookWindowsHookEx(_globalKeyHook);
		_globalKeyHook = nullptr;
	}

	bool IoInputSubsystem::PumpGlobalKeyCapture(actions::KeyAction& action) noexcept
	{
		const auto pumpOne = [&action](unsigned int vk,
			std::atomic<bool>& down,
			std::atomic<bool>& lastDispatched) noexcept
		{
			const auto curDown = down.load(std::memory_order_acquire);
			const auto lastDown = lastDispatched.load(std::memory_order_acquire);
			if (curDown == lastDown)
				return false;

			lastDispatched.store(curDown, std::memory_order_release);
			action.KeyChar = vk;
			action.KeyActionType = curDown ? actions::KeyAction::KEY_DOWN : actions::KeyAction::KEY_UP;
			action.IsSystem = false;
			action.Modifiers = base::Action::MODIFIER_NONE;
			return true;
		};

		if (pumpOne(VK_INSERT, _globalInsertDown, _globalInsertLastDispatchedDown))
			return true;
		if (pumpOne(VK_PRIOR, _globalPageUpDown, _globalPageUpLastDispatchedDown))
			return true;
		if (pumpOne(VK_NEXT, _globalPageDownDown, _globalPageDownLastDispatchedDown))
			return true;

		return false;
	}

	LRESULT CALLBACK IoInputSubsystem::_LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept
	{
		if (nCode == HC_ACTION)
		{
			const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
			if (kb)
			{
				std::atomic<bool>* keyDown = nullptr;
				switch (kb->vkCode)
				{
				case VK_INSERT:
					keyDown = &_globalInsertDown;
					break;
				case VK_PRIOR:
					keyDown = &_globalPageUpDown;
					break;
				case VK_NEXT:
					keyDown = &_globalPageDownDown;
					break;
				default:
					break;
				}

				if (keyDown)
				{
					if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
						keyDown->store(true, std::memory_order_release);
					else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
						keyDown->store(false, std::memory_order_release);
				}
			}
		}

		return CallNextHookEx(_globalKeyHook, nCode, wParam, lParam);
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

	actions::ActionResult IoInputSubsystem::HandleChannelOverrideKey(const actions::KeyAction& action,
		const std::vector<std::shared_ptr<engine::Station>>& stations)
	{
		return _midiRouter.HandleChannelOverrideKey(action, stations);
	}

	void IoInputSubsystem::SetForcedChannelOverride(std::uint8_t forcedChannelOverride,
		const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept
	{
		_midiRouter.SetForcedChannelOverride(forcedChannelOverride, stations);
	}

	std::uint8_t IoInputSubsystem::ForcedChannelOverride() const noexcept
	{
		return _midiRouter.ForcedChannelOverride();
	}

	void IoInputSubsystem::RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger)
	{
		_midiRouter.RegisterTrigger(deviceName, std::move(trigger));
	}
}
