#include "AutomationPlayback.h"
#include <cmath>
#include "MidiLoop.h"
#include "MidiRouter.h"
#include "../engine/LoopTake.h"
#include "../vst/VstChain.h"

using midi::AutomationPlayback;

void AutomationPlayback::Rebuild(const std::vector<std::shared_ptr<engine::LoopTake>>& takes,
	const std::shared_ptr<vst::VstChain>& stationChain)
{
	// Scene-serialized non-audio ownership. Resolve observers against owning
	// chains before dereferencing; snapshots pin processors, loops and anchors.
	auto snapshot = std::make_shared<AutomationDispatchSnapshot>();
	snapshot->Generation = _nextAutomationDispatchGeneration++;

	for (const auto& take : takes)
	{
		if (!take)
			continue;

		for (const auto& midiLoop : take->GetMidiLoopSnapshot())
		{
			if (!midiLoop)
				continue;

			for (std::size_t laneIdx = 0u; laneIdx < midi::MidiLoop::MaxAutomationLanes; ++laneIdx)
			{
				if (snapshot->Count >= MaxAutomationDispatches)
					break;

				auto& lane = midiLoop->GetLane(laneIdx);
				if (!lane.Mapping.IsActive())
					continue;
				auto& target = lane.Mapping.SavedTarget;
				std::size_t pluginIndex = 0u;
				std::shared_ptr<vst::IVstPlugin> plugin;
				std::string scope;
				unsigned int loopIndex = 0u;
				if ((target.Scope.empty() || target.Scope == "station") && stationChain)
				{
					plugin = stationChain->FindPlugin(lane.Mapping.TargetPlugin, target.PluginId, pluginIndex);
					if (plugin) scope = "station";
				}
				if (!plugin && (target.Scope.empty() || target.Scope == "take"))
				{
					plugin = take->FindVstPlugin(lane.Mapping.TargetPlugin, target.PluginId, pluginIndex);
					if (plugin) scope = "take";
				}
				if (!plugin && (target.Scope.empty() || target.Scope == "loop"))
				{
					const auto& loops = take->GetLoops();
					for (std::size_t index = 0u; index < loops.size(); ++index)
					{
						if (!loops[index]) continue;
						plugin = loops[index]->FindVstPlugin(lane.Mapping.TargetPlugin, target.PluginId, pluginIndex);
						if (plugin) { scope = "loop"; loopIndex = static_cast<unsigned int>(index); break; }
					}
				}
				if (!plugin) continue; // Retain saved identity/curve, publish no unsafe observer.
				lane.Mapping.TargetPlugin = plugin.get();
				target = {scope, plugin->StateId(), static_cast<unsigned int>(pluginIndex), loopIndex};

				const auto count = snapshot->Count++;
				snapshot->Plugins[count] = plugin;
				snapshot->Takes[count] = take;
				snapshot->Loops[count] = midiLoop;
				auto& entry = snapshot->Entries[count];
				entry.plugin = plugin.get();
				entry.paramIdx = lane.Mapping.TargetParameterIndex;
				entry.loop = midiLoop.get();
				entry.laneIdx = static_cast<std::uint8_t>(laneIdx);
				entry.loopLengthSamps = midiLoop->LoopLengthSamps();
				entry.automationGlobalSampleOrigin = midiLoop->AutomationGlobalSampleOrigin();
				entry.anchorCorrection = take->MidiAnchorCorrectionPtr();
			}
		}
	}

	_automationDispatch.store(std::move(snapshot), std::memory_order_release);
}

void AutomationPlayback::ProcessBlock(std::uint32_t blockStartSample,
	std::uint32_t numSamps) noexcept
{
	const auto snapshot = _automationDispatch.borrow();
	if (!snapshot)
		return;

	// Detect a newly published list and reset per-entry playback state so
	// stale cursors and last-values from the previous list are never used.
	if (snapshot->Generation != _automationDispatchGeneration)
	{
		_automationDispatchGeneration = snapshot->Generation;
		for (auto& s : _automationPlaybackState)
		{
			s.cursorIdx = 0u;
			s.stateGeneration = 0u;
			s.lastValue = -2.0f;
		}
	}

	const auto count = snapshot->Count;
	auto dispatchSample = blockStartSample + ((numSamps > 0u) ? (numSamps - 1u) : 0u);

	for (std::size_t i = 0u; i < count; ++i)
	{
		const auto& entry = snapshot->Entries[i];
		if (!entry.plugin || !entry.loop)
			continue;

		// Editor-driven recording leaves a short per-parameter cool-down so a just
		// dragged parameter is not snapped back to its recorded curve the instant
		// automation record is released. Only the matching (plugin, parameter) pair
		// is held off; all other automation plays normally. Sample-domain deadline,
		// so the audio thread never reads a wall clock.
		if (midi::MidiRouter::IsParameterSuppressed(entry.plugin, entry.paramIdx, dispatchSample))
			continue;

		// Apply the live anchor correction from the owning LoopTake. The frozen
		// automationGlobalSampleOrigin was baked at dispatch rebuild; the correction accumulates
		// remote NINJAM wrap re-anchor deltas without requiring a rebuild.
		const std::int32_t correction = entry.anchorCorrection
			? entry.anchorCorrection->load(std::memory_order_relaxed) : 0;
		const auto effectiveAutomationGlobalSampleOrigin = entry.automationGlobalSampleOrigin
			+ static_cast<std::uint32_t>(correction);
		const double frac = (entry.loopLengthSamps > 0u)
			? std::fmod(
				static_cast<double>(dispatchSample - effectiveAutomationGlobalSampleOrigin),
				static_cast<double>(entry.loopLengthSamps))
					/ static_cast<double>(entry.loopLengthSamps)
			: 0.0;

		auto& state = _automationPlaybackState[i];
		const float val = entry.loop->GetAutomationValueAtCursor(entry.laneIdx, frac, state.cursorIdx);

		const auto generation = entry.plugin->AutomationGeneration();
		if (generation != state.stateGeneration || std::abs(val - state.lastValue) > AutomationEpsilon)
		{
			vst::IVstPlugin::PluginCallGuard call(*entry.plugin);
			if (!call) continue;
			entry.plugin->SetParameter(entry.paramIdx, val);
			state.lastValue = val;
			state.stateGeneration = generation;
		}
	}
}

void AutomationPlayback::ClearAfterReadersStopped()
{
	_automationDispatch.store(nullptr);
	_automationDispatch.collect();
}
