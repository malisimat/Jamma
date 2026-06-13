#include "stdafx.h"
#include "WindowSubsystem.h"
#include <algorithm>

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
