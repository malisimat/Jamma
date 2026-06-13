#include "stdafx.h"
#include "WindowSubsystem.h"
#include <algorithm>
#include <iostream>
#include "../utils/PathUtils.h"

namespace engine
{
	WindowSubsystem::~WindowSubsystem()
	{
		CloseAllVstEditorWindows();
	}

	void WindowSubsystem::CloseAllVstEditorWindows()
	{
		for (auto& window : _vstEditorWindows)
		{
			if (window)
				window->Destroy();
		}
		_vstEditorWindows.clear();
	}

	void WindowSubsystem::PruneClosedVstEditorWindows()
	{
		_vstEditorWindows.erase(std::remove_if(_vstEditorWindows.begin(), _vstEditorWindows.end(), [](const std::unique_ptr<graphics::VstEditorWindow>& window) {
			return !window || !window->IsOpen();
		}), _vstEditorWindows.end());
	}

	actions::ActionResult WindowSubsystem::EatAction()
	{
		actions::ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}

	actions::ActionResult WindowSubsystem::HandleVstInsert(const std::wstring& pluginPath,
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

	actions::ActionResult WindowSubsystem::HandleVstEditorOpen(const std::shared_ptr<base::GuiElement>& hovering,
		base::SelectDepth depth,
		const std::vector<std::shared_ptr<Station>>& stations)
	{
		PruneClosedVstEditorWindows();

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

	bool WindowSubsystem::OpenVstEditorForPlugin(const std::shared_ptr<vst::IVstPlugin>& plugin)
	{
		if (!plugin || !plugin->IsLoaded())
			return false;

		auto window = std::make_unique<graphics::VstEditorWindow>();
		const auto hInstance = GetModuleHandle(nullptr);
		if (!window->Create(hInstance, plugin))
			return false;

		_vstEditorWindows.push_back(std::move(window));
		return true;
	}

	bool WindowSubsystem::TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex)
	{
		if (!loop)
			return false;

		auto plugin = loop->GetVstPlugin(pluginIndex);
		if (!plugin || !plugin->IsLoaded())
			return false;

		return OpenVstEditorForPlugin(plugin);
	}

	bool WindowSubsystem::TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex)
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

	bool WindowSubsystem::TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
		base::SelectDepth depth,
		size_t pluginIndex)
	{
		if (!hovering)
			return false;

		switch (depth)
		{
		case base::SelectDepth::DEPTH_STATION:
		{
			auto station = std::dynamic_pointer_cast<Station>(hovering);
			return TryOpenVstEditorForStation(station, pluginIndex);
		}
		case base::SelectDepth::DEPTH_LOOPTAKE:
		{
			auto take = std::dynamic_pointer_cast<LoopTake>(hovering);
			if (!take)
				return false;

			for (const auto& loop : take->GetLoops())
			{
				if (TryOpenVstEditorForLoop(loop, pluginIndex))
					return true;
			}

			return false;
		}
		case base::SelectDepth::DEPTH_LOOP:
		{
			auto loop = std::dynamic_pointer_cast<Loop>(hovering);
			return TryOpenVstEditorForLoop(loop, pluginIndex);
		}
		default:
			return false;
		}
	}
}
