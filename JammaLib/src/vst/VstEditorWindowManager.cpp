#include "stdafx.h"
#include "VstEditorWindowManager.h"
#include <algorithm>
#include <iostream>
#include "../utils/PathUtils.h"

using namespace engine;

namespace vst
{
	VstEditorWindowManager::~VstEditorWindowManager()
	{
		CloseAllVstEditorWindows();
	}

	void VstEditorWindowManager::CloseAllVstEditorWindows()
	{
		std::vector<std::shared_ptr<graphics::VstEditorWindow>> windows;
		{
			std::scoped_lock lock(_vstEditorWindowsMutex);
			windows = std::move(_vstEditorWindows);
			_vstEditorWindows.clear();
		}

		for (auto& window : windows)
		{
			if (window)
				window->Destroy();
		}
	}

	void VstEditorWindowManager::PruneClosedVstEditorWindows()
	{
		std::vector<std::shared_ptr<graphics::VstEditorWindow>> retired;
		{
			std::scoped_lock lock(_vstEditorWindowsMutex);
			for (auto& window : _vstEditorWindows)
				if (window && !window->IsOpen()) retired.push_back(std::move(window));
			std::erase_if(_vstEditorWindows, [](const auto& window) { return !window; });
		}
		// Releasing the last plugin reference can run third-party teardown.
		// Keep it outside the manager mutex, even for an already closed HWND.
	}

	actions::ActionResult VstEditorWindowManager::EatAction()
	{
		actions::ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}

	void VstEditorWindowManager::SetLogging(base::LoggingConfig config) noexcept
	{
		_loggingConfig = std::move(config);
	}

	actions::ActionResult VstEditorWindowManager::HandleVstInsert(const std::wstring& pluginPath,
		base::SelectDepth depth,
		const std::shared_ptr<base::GuiElement>& hovering,
		const std::function<void()>& onCommit)
	{
		if (!hovering)
		{
			std::cout << "VST insert: no hovered target" << std::endl;
			return actions::ActionResult::NoAction();
		}

		std::cout << "VST insert request: depth=" << static_cast<int>(depth)
			<< ", path=" << utils::EncodeUtf8(pluginPath) << std::endl;

		switch (depth)
		{
		case base::SelectDepth::DEPTH_STATION:
		{
			auto station = std::dynamic_pointer_cast<Station>(hovering);
			if (!station)
				return actions::ActionResult::NoAction();

			std::cout << "VST insert target: station '" << station->Name() << "' (busChannels=" << station->NumBusChannels() << ")" << std::endl;

			if (station->IsRemote())
			{
				std::cout << "VST insert: remote stations are read-only" << std::endl;
				return actions::ActionResult::NoAction();
			}

			station->LoadVstPlugin(pluginPath);
			if (onCommit)
				onCommit();
			break;
		}
		case base::SelectDepth::DEPTH_LOOPTAKE:
		{
			auto take = std::dynamic_pointer_cast<LoopTake>(hovering);
			if (!take)
				return actions::ActionResult::NoAction();

			std::cout << "VST insert target: looptake (numLoops=" << take->GetLoops().size() << ")" << std::endl;

			take->LoadVstPlugin(pluginPath);
			if (onCommit)
				onCommit();
			break;
		}
		case base::SelectDepth::DEPTH_LOOP:
		{
			auto loop = std::dynamic_pointer_cast<Loop>(hovering);
			if (!loop)
				return actions::ActionResult::NoAction();

			std::cout << "VST insert target: single loop" << std::endl;

			loop->LoadVstPlugin(pluginPath);
			if (onCommit)
				onCommit();
			break;
		}
		default:
			return actions::ActionResult::NoAction();
		}

		std::cout << "VST inserted: " << utils::EncodeUtf8(pluginPath) << std::endl;
		return EatAction();
	}

	actions::ActionResult VstEditorWindowManager::HandleVstEditorOpen(const std::shared_ptr<base::GuiElement>& hovering,
		base::SelectDepth depth,
		const std::vector<std::shared_ptr<Station>>& stations)
	{
		PruneClosedVstEditorWindows();

		if (_loggingConfig.Ui == "verbose")
		{
			auto hoverStation = std::dynamic_pointer_cast<Station>(hovering);
			std::cout << "VST editor open: hovering=" << (hoverStation ? "Station '" + hoverStation->Name() + "'" : hovering ? "non-station" : "null") << ", depth=" << static_cast<int>(depth) << std::endl;
		}

		if (TryOpenVstEditorForHover(hovering, depth, 0u))
			return EatAction();

		for (const auto& station : stations)
		{
			if (TryOpenVstEditorForStation(station, 0u))
				return EatAction();
		}

		std::cout << "VST editor open: no loaded plugin found" << std::endl;
		return actions::ActionResult::NoAction();
	}

	bool VstEditorWindowManager::OpenVstEditorForPlugin(const std::shared_ptr<vst::IVstPlugin>& plugin)
	{
		const auto ownedPlugin = plugin;
		if (!ownedPlugin || !ownedPlugin->IsLoaded())
			return false;

		// Only inspect ownership under the mutex. Native activation and plugin
		// close calls can synchronously pump messages back into this manager.
		struct PendingOpenRelease
		{
			std::mutex& Mutex;
			std::vector<const IVstPlugin*>& Pending;
			const IVstPlugin* Plugin;
			~PendingOpenRelease()
			{
				std::scoped_lock lock(Mutex);
				std::erase(Pending, Plugin);
			}
		};
		HWND existingHwnd = nullptr;
		{
			std::scoped_lock lock(_vstEditorWindowsMutex);
			if (std::find(_pendingEditorOpens.begin(), _pendingEditorOpens.end(), ownedPlugin.get()) != _pendingEditorOpens.end())
				return false; // The original request owns creation until it returns.
			_pendingEditorOpens.push_back(ownedPlugin.get());
			for (const auto& window : _vstEditorWindows)
				if (window && window->IsOpen() && window->Plugin().get() == ownedPlugin.get())
				{
					existingHwnd = window->EditorHwnd();
					break;
				}
		}
		PendingOpenRelease pendingRelease{_vstEditorWindowsMutex, _pendingEditorOpens, ownedPlugin.get()};
		if (existingHwnd)
		{
			if (IsIconic(existingHwnd)) ShowWindow(existingHwnd, SW_RESTORE);
			SetForegroundWindow(existingHwnd);
			return true;
		}

		auto window = std::make_shared<graphics::VstEditorWindow>();
		const auto hInstance = GetModuleHandle(nullptr);
		if (!window->Create(hInstance, ownedPlugin))
			return false;

		{
			std::scoped_lock lock(_vstEditorWindowsMutex);
			for (const auto& existingWindow : _vstEditorWindows)
				if (existingWindow && existingWindow->IsOpen() && existingWindow->Plugin().get() == ownedPlugin.get())
				{
					existingHwnd = existingWindow->EditorHwnd();
					break;
				}
			if (!existingHwnd) _vstEditorWindows.push_back(std::move(window));
		}
		if (existingHwnd)
		{
			window->Destroy();
			if (IsIconic(existingHwnd)) ShowWindow(existingHwnd, SW_RESTORE);
			SetForegroundWindow(existingHwnd);
		}
		return true;
	}

	bool VstEditorWindowManager::TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex)
	{
		if (!loop)
			return false;

		auto plugin = loop->GetVstPlugin(pluginIndex);
		if (!plugin || !plugin->IsLoaded())
			return false;

		return OpenVstEditorForPlugin(plugin);
	}

	bool VstEditorWindowManager::TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex)
	{
		if (!station || station->IsRemote())
			return false;

		auto stationPlugin = station->GetVstPlugin(pluginIndex);
		if (stationPlugin && stationPlugin->IsLoaded())
			return OpenVstEditorForPlugin(stationPlugin);

		for (const auto& take : station->GetLoopTakes())
		{
			for (const auto& loop : take->GetLoops())
			{
				if (TryOpenVstEditorForLoop(loop, pluginIndex))
					return true;
			}
		}

		return false;
	}

	bool VstEditorWindowManager::TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
		base::SelectDepth depth,
		size_t pluginIndex)
	{
		if (!hovering)
			return false;
		(void)depth;

		for (auto current = hovering; current; current = current->Parent())
		{
			if (auto loop = std::dynamic_pointer_cast<Loop>(current))
			{
				if (TryOpenVstEditorForLoop(loop, pluginIndex))
					return true;
				continue;
			}

			if (auto take = std::dynamic_pointer_cast<LoopTake>(current))
			{
				for (const auto& loop : take->GetLoops())
				{
					if (TryOpenVstEditorForLoop(loop, pluginIndex))
						return true;
				}
				continue;
			}

			if (auto station = std::dynamic_pointer_cast<Station>(current))
			{
				if (TryOpenVstEditorForStation(station, pluginIndex))
					return true;
			}
		}

		return false;
	}
}

void vst::VstEditorWindowManager::CloseVstEditorsForStation(const std::shared_ptr<engine::Station>& station)
{
	if (!station) return;
	const auto owns = [&](const vst::IVstPlugin* plugin) {
		for (size_t i = 0u; auto current = station->GetVstPlugin(i); ++i) if (current.get() == plugin) return true;
		for (const auto& take : station->GetLoopTakeSnapshot())
		{
			if (take->OwnsPlugin(plugin)) return true;
			for (const auto& loop : take->GetLoops())
				for (size_t i = 0u; auto current = loop->GetVstPlugin(i); ++i) if (current.get() == plugin) return true;
		}
		return false;
	};
	std::vector<std::shared_ptr<graphics::VstEditorWindow>> closing;
	{
		std::scoped_lock lock(_vstEditorWindowsMutex);
		for (auto it = _vstEditorWindows.begin(); it != _vstEditorWindows.end();)
		{
			if (*it && owns((*it)->Plugin().get()))
			{ closing.push_back(std::move(*it)); it = _vstEditorWindows.erase(it); }
			else ++it;
		}
	}
	// DestroyWindow/plugin close can reenter Scene focus handling synchronously.
	for (const auto& window : closing) window->Destroy();
}

void vst::VstEditorWindowManager::CloseVstEditorsForTake(const std::shared_ptr<engine::LoopTake>& take)
{
	if (!take) return;
	const auto owns = [&](const vst::IVstPlugin* plugin) {
		if (take->OwnsPlugin(plugin)) return true;
		for (const auto& loop : take->GetLoops())
			for (size_t i = 0u; auto current = loop->GetVstPlugin(i); ++i) if (current.get() == plugin) return true;
		return false;
	};
	std::vector<std::shared_ptr<graphics::VstEditorWindow>> closing;
	{
		std::scoped_lock lock(_vstEditorWindowsMutex);
		for (auto it = _vstEditorWindows.begin(); it != _vstEditorWindows.end();)
		{
			if (*it && owns((*it)->Plugin().get()))
			{ closing.push_back(std::move(*it)); it = _vstEditorWindows.erase(it); }
			else ++it;
		}
	}
	// DestroyWindow/plugin close can reenter Scene focus handling synchronously.
	for (const auto& window : closing) window->Destroy();
}
