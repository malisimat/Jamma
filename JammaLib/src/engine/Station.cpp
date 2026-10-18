#include "Station.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include "../midi/MidiBlockTiming.h"
#include "../midi/MidiRouter.h"
#include "../io/NativeMidiSidecar.h"
#include "../utils/MathUtils.h"

using namespace engine;
using namespace audio;
using namespace actions;
using namespace base;
using namespace midi;
using resources::ResourceLib;
using utils::Size2d;
using gui::GuiRackParams;
using gui::GuiToggleParams;

void Station::_DrainVstChain(std::shared_ptr<vst::VstChain> chain)
{
	if (!chain)
		return;
	for (size_t i = 0; i < chain->NumPlugins(); ++i)
	{
		auto plugin = chain->GetPlugin(i);
		if (plugin)
			vst::QueueForUiThreadDestroy(std::move(plugin));
	}
	chain.reset();
}

unsigned int Station::_ResolveSampleRate(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (params.has_value() && (params.value().SampleRate > 0u))
		return params.value().SampleRate;

	if (cfg.has_value() && (cfg.value().Audio.SampleRate > 0u))
		return cfg.value().Audio.SampleRate;

	return constants::DefaultSampleRate;
}

void Station::_TrySeedClockFromFirstLoop(const std::shared_ptr<utils::Timer>& clock,
	unsigned long loopLengthSamps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params,
	const std::string& takeId)
{
	if (!clock)
		return;

	auto policyCfg = cfg.value_or(io::UserConfig());
	const auto sampleRate = _ResolveSampleRate(cfg, params);
	if (auto timing = policyCfg.DeduceLoopTiming(loopLengthSamps, sampleRate); timing.has_value())
	{
		const auto quantisation = policyCfg.Loop.SeedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE;
		clock->SetQuantisation(timing->GrainSamps, quantisation);
		clock->SetSeedSourceLength(static_cast<unsigned long>(timing->GrainSamps) * timing->LoopGrains);
		// Structural recording completion runs on the job owner, never OnTick.
		std::cout << "[Quantisation] recording master established: take=" << takeId
			<< " authority=local source=default physical=" << loopLengthSamps
			<< " logical=" << clock->SeedSourceLength() << " sr=" << sampleRate
			<< " grain=" << timing->GrainSamps << " grains=" << timing->LoopGrains
			<< " bpm=" << 60.0 * sampleRate / timing->GrainSamps
			<< " policyMinMs=" << policyCfg.Loop.SeedGrainMinMs
			<< " policyMaxMs=" << policyCfg.Loop.SeedGrainTargetMaxMs
			<< " rounding=floor-grain,exact-product" << std::endl;
	}
}

Station::Station(StationParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	_flipTakeBuffer(false),
	_flipAudioBuffer(false),
	_name(std::make_shared<const std::string>(params.Name)),
	_id(params.Id.empty() ? utils::GetGuid() : params.Id),
	_fadeSamps(params.FadeSamps),
	_lastBufSize(constants::MaxBlockSize),
	_clock(std::shared_ptr<utils::Timer>()),
	_quantisationModel(std::make_shared<QuantisationModel>()),
	_quantisationDivisionModel(std::make_shared<QuantisationDivisionModel>()),
	_stationModel(std::make_shared<graphics::StationModel>()),
	_guiRack(nullptr),
	_masterMixer(nullptr),
	_mixerToggle(nullptr),
	_routerToggle(nullptr),
	_router(nullptr),
	_loopTakes(),
	_backLoopTakes(),
	_loopTakeSnapshot(nullptr),
	_audioMixers(),
	_backAudioMixers(),
	_audioBuffers(),
	_backAudioBuffers(),
	_vstChain(nullptr),
	_backVstChain(nullptr),
	_flipVstChain(false),
	_pendingVstLoads(),
	_pendingVstUnloads(),
	_vstPathsMutex(),
	_vstPluginPaths(),
	_immediateLiveMidiIngress(),
	_syntheticLiveMidiIngress(),
	_allowedMidiChannels(),
	_allowedMidiChannelMask(0u),
	_midiVstRoutes(nullptr),
	_retainedMidiVstRoutes(),
	_sampleRate(44100.0f),
	_blockSize(512u),
	_vstBlockScratch(),
	_vstBlockPtrs(),
	_pendingQuantisationParams(std::nullopt),
	_pendingQuantisationConfirm(false),
	_quantisationOverlayAlpha(0.0f)
{
	_masterMixer = std::make_shared<AudioMixer>(mixerParams);
	_stationModel->SetParams(params.StationLevelFallRate);
	_guiRack = std::make_shared<gui::GuiRack>(_GetRackParams(params.Size));

	//_children.push_back(_masterMixer);
	_children.push_back(_guiRack);

	SetNumBusChannels(_DefaultNumBusChannels);

	_WireVuSliders();
	_PublishLoopTakeSnapshot();
	_PublishAudioState();
}

Station::~Station()
{
	_DrainVstChain(_vstChain.load(std::memory_order_acquire));
	_DrainVstChain(_backVstChain);
}

std::optional<std::shared_ptr<Station>> Station::FromFile(StationParams stationParams,
	AudioMixerParams mixerParams,
	io::JamFile::Station stationStruct,
	std::wstring dir)
{
	std::cout << "[LOAD] Constructing station " << stationParams.Index << " '" << stationStruct.Name
		<< "' (takes=" << stationStruct.LoopTakes.size() << ", VSTs=" << stationStruct.VstChain.size() << ")." << std::endl;
	stationParams.Name = stationStruct.Name;
	stationParams.Id = stationStruct.Id;
	auto station = std::make_shared<Station>(stationParams, mixerParams);
	station->SetStationPhaseOffsetSamps(stationStruct.StationPhaseOffsetSamps);
	station->SetAllowedMidiChannels(stationStruct.AllowedMidiChannels);
	station->_rackRenderPolicy = stationStruct.AdditiveVstRack.value_or(stationStruct.VstChain.empty())
		? vst::VstChain::RenderPolicy::AdditiveInstruments : vst::VstChain::RenderPolicy::LegacySerial;
	station->_liveVstChannelRoutes = stationStruct.LiveVstRoutes;
	std::vector<std::string> identities;
	station->_rackInactive = stationStruct.VstChain.size() > MaxRackPlugins;
	for (std::size_t index = 0; index < stationStruct.VstChain.size(); ++index)
	{
		auto& entry = stationStruct.VstChain[index];
		// Legacy entries without identity receive a deterministic local identity.
		if (entry.Id.empty()) entry.Id = stationStruct.Id + ":vst:" + std::to_string(index);
		if (!io::NativeMidiSidecar::ValidatePluginIdentity(entry.Id) || std::find(identities.begin(), identities.end(), entry.Id) != identities.end()) station->_rackInactive = true;
		identities.push_back(entry.Id);
	}
	for (const auto& route : station->_liveVstChannelRoutes)
	{
		if (route.PluginIds.size() > MaxRackPlugins) station->_rackInactive = true;
		for (const auto& identity : route.PluginIds)
			if (identity.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(identity)) station->_rackInactive = true;
	}
	if (station->_rackInactive) station->_rackDiagnostic = "Rack paused: remove excess/invalid entries or cables, then choose Resume rack. Saved data is preserved.";
	// Legacy automation sidecars contain indices only. Give saved processors
	// deterministic IDs before reconstruction, including unavailable Take/Loop
	// slots, so later compaction cannot change the meaning of those bindings.
	for (std::size_t takeIndex = 0u; takeIndex < stationStruct.LoopTakes.size(); ++takeIndex)
	{
		auto& savedTake = stationStruct.LoopTakes[takeIndex];
		if (savedTake.Id.empty()) savedTake.Id = !savedTake.Name.empty() ? savedTake.Name
			: stationStruct.Id + ":take:" + std::to_string(takeIndex);
		for (std::size_t pluginIndex = 0u; pluginIndex < savedTake.VstChain.size(); ++pluginIndex)
			if (savedTake.VstChain[pluginIndex].Id.empty()) savedTake.VstChain[pluginIndex].Id
				= savedTake.Id + ":vst:" + std::to_string(pluginIndex);
		for (std::size_t loopIndex = 0u; loopIndex < savedTake.Loops.size(); ++loopIndex)
			for (std::size_t pluginIndex = 0u; pluginIndex < savedTake.Loops[loopIndex].VstChain.size(); ++pluginIndex)
				if (savedTake.Loops[loopIndex].VstChain[pluginIndex].Id.empty()) savedTake.Loops[loopIndex].VstChain[pluginIndex].Id
					= savedTake.Id + ":loop:" + std::to_string(loopIndex) + ":vst:" + std::to_string(pluginIndex);
	}
	for (const auto& vstEntry : stationStruct.VstChain)
	{
		if (station->_rackInactive)
		{
			station->_vstSavedEntries.push_back(vstEntry);
			station->_vstPluginPaths.push_back(utils::DecodeUtf8(vstEntry.Path));
			continue;
		}
		std::cout << "[LOAD] Station '" << stationStruct.Name << "': loading VST " << vstEntry.Path << std::endl;
		if (!station->LoadVstPluginSynchronously(utils::DecodeUtf8(vstEntry.Path), vstEntry.DecodeState(), vstEntry.Bypass, vstEntry.Id))
		{
			// Restore the station and rig target even when this optional processor is missing.
			std::cout << "[LOAD] Station '" << stationStruct.Name << "': failed VST " << vstEntry.Path
				<< "; continuing without it." << std::endl;
		}
	}

	auto numTakes = (unsigned int)stationStruct.LoopTakes.size();
	Size2d gap = { 4, 4 };
	auto takeHeight = numTakes > 0 ?
		stationParams.Size.Height - ((2 + numTakes - 1) * gap.Height) / numTakes :
		stationParams.Size.Height - (2 * gap.Height);
	takeHeight = takeHeight < 1u ? 1u : takeHeight;
	Size2d takeSize = { stationParams.Size.Width - (2 * gap.Width), takeHeight };
	LoopTakeParams takeParams;
	takeParams.Size = { 80, 80 };
	
	auto takeCount = 0u;
	for (auto takeStruct : stationStruct.LoopTakes)
	{
		takeParams.ModelPosition = { (float)gap.Width, (float)(takeCount * takeHeight + gap.Height), 0.0 };
		// Legacy descriptors without identity receive one during import. Keep
		// the display name separate from job deduplication/group identity.
		// Legacy JAM manifests stored take identity in Name. Retain it so trigger
		// histories and source references survive migration to the explicit Id.
		takeParams.Id = !takeStruct.Id.empty() ? takeStruct.Id :
			!takeStruct.Name.empty() ? takeStruct.Name : utils::GetGuid();
		auto take = LoopTake::FromFile(takeParams, takeStruct, dir);
		
		if (take.has_value())
		{
			station->AddTake(take.value());
			// AddTake establishes the station bus count. Saved routes are applied
			// afterwards so its normal one-to-one defaults cannot overwrite them.
			if (takeStruct.HasAudioRoutes && !take.value()->RestoreAudioRoutes(takeStruct.AudioRoutes))
			{
				std::cout << "Load: invalid audio routes in take " << takeStruct.Name << std::endl;
				return std::nullopt;
			}
		}

		takeCount++;
	}
	if (stationStruct.HasAudioRoutes && !station->RestoreAudioRoutes(stationStruct.AudioRoutes))
	{
		std::cout << "Load: invalid audio routes in station " << stationStruct.Name << std::endl;
		return std::nullopt;
	}
	if (!station->RestoreMixerLevels(stationStruct.MasterLevel, stationStruct.BusLevels))
		return std::nullopt;
	// Empty stations are valid saved state: a station may contain its VST chain,
	// routing configuration, or simply be ready for a new take. Only malformed
	// station data should prevent the station from being reconstructed.
	for (const auto& take : station->GetLoopTakes())
	{
		if (!take)
			continue;
		const auto savedTake = std::find_if(stationStruct.LoopTakes.begin(), stationStruct.LoopTakes.end(),
			[&](const auto& saved) { return saved.Id == take->Id(); });

		for (const auto& binding : take->PendingAutomationBindings())
		{
			auto midiLoops = take->GetMidiLoopSnapshot();
			if (binding.MidiStreamIndex >= midiLoops.size() || binding.LaneIndex >= midi::MidiLoop::MaxAutomationLanes)
				return std::nullopt;
			if (!midiLoops[binding.MidiStreamIndex]) return std::nullopt;
			auto& savedTarget = midiLoops[binding.MidiStreamIndex]->GetLane(binding.LaneIndex).Mapping.SavedTarget;
			// Migrate old station indices through the saved chain even if the rack
			// is inactive/missing: compaction must never redirect this lane later.
			if (savedTarget.PluginId.empty() && binding.TargetScope == "station"
				&& binding.TargetPluginIndex < stationStruct.VstChain.size())
				savedTarget.PluginId = stationStruct.VstChain[binding.TargetPluginIndex].Id;
			if (savedTarget.PluginId.empty() && savedTake != stationStruct.LoopTakes.end())
			{
				if (binding.TargetScope == "take" && binding.TargetPluginIndex < savedTake->VstChain.size())
					savedTarget.PluginId = savedTake->VstChain[binding.TargetPluginIndex].Id;
				else if (binding.TargetScope == "loop" && binding.TargetLoopIndex < savedTake->Loops.size()
					&& binding.TargetPluginIndex < savedTake->Loops[binding.TargetLoopIndex].VstChain.size())
					savedTarget.PluginId = savedTake->Loops[binding.TargetLoopIndex].VstChain[binding.TargetPluginIndex].Id;
			}

			// Keep raw malformed rack metadata, scope/index and curve, but migrate
			// its legacy positional target to an explicit valid unavailable ID.
			if (savedTarget.PluginId.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(savedTarget.PluginId))
				savedTarget.PluginId = "unavailable:" + utils::GetGuid();
			std::shared_ptr<vst::IVstPlugin> plugin;
			if (binding.TargetScope == "station")
				plugin = station->GetVstPlugin(binding.TargetPluginIndex);
			else if (binding.TargetScope == "take")
				plugin = take->GetVstPlugin(binding.TargetPluginIndex);
			else if (binding.TargetScope == "loop")
			{
				const auto& loops = take->GetLoops();
				if (binding.TargetLoopIndex < loops.size() && loops[binding.TargetLoopIndex])
					plugin = loops[binding.TargetLoopIndex]->GetVstPlugin(binding.TargetPluginIndex);
			}

			if (plugin && !savedTarget.PluginId.empty() && plugin->StateId() != savedTarget.PluginId) plugin.reset();
			if (!plugin || !midiLoops[binding.MidiStreamIndex]->BindAutomationLaneTarget(
				binding.LaneIndex, plugin.get(), plugin->StateId()))
			{
				std::cout << "[LOAD] Station '" << stationStruct.Name
					<< "': retaining unavailable automation target." << std::endl;
			}
		}
		take->ClearPendingAutomationBindings();
	}
	// Publish the completed pointer bindings as one immutable dispatch list before
	// this startup-built station is made visible to audio.
	station->RebuildAutomationDispatch();

	if (!stationStruct.MidiRoutes.empty())
	{
		std::size_t outputCount = 0u;
		for (const auto& take : station->GetLoopTakes())
			if (take)
				outputCount += take->GetMidiLoopSnapshot().size();

		midi::MidiVstRoutingSnapshot routes;
		routes.PluginByMidiOutput.assign(outputCount, midi::MidiVstRoutingSnapshot::NoPlugin);
		for (const auto& route : stationStruct.MidiRoutes)
		{
			if ((route.PluginIndex >= stationStruct.VstChain.size() && route.PluginIndex != MidiVstRoutingSnapshot::MissingPlugin)
				|| (!route.IsLive && route.OutputIndex >= outputCount))
			{
				std::cout << "Load: invalid MIDI VST route in station " << stationStruct.Name << std::endl;
				return std::nullopt;
			}
			if (route.IsLive)
			{
				if (!stationStruct.HasLiveVstRoutes)
					for (auto& channel : station->_liveVstChannelRoutes)
					{
						channel.Explicit = true;
						if (route.PluginIndex < stationStruct.VstChain.size())
							channel.PluginIds = {stationStruct.VstChain[route.PluginIndex].Id};
					}
			}
			else
				routes.PluginByMidiOutput[route.OutputIndex] = route.PluginIndex;
		}
		if (!station->RestoreMidiVstRoutes(routes, stationStruct.VstChain.size()))
		{
			std::cout << "Load: failed MIDI VST routes for station " << stationStruct.Name << std::endl;
			return std::nullopt;
		}
	}
	// Validate the complete pending take hierarchy before Scene admits audio.
	station->RefreshRackLatency();
	std::cout << "[LOAD] Constructed station '" << stationStruct.Name << "'." << std::endl;

	return station;
}

std::vector<std::vector<unsigned long>> Station::SnapshotAudioRoutesForExport() const
{
	std::vector<std::vector<unsigned long>> routes;
	for (const auto& mixer : _audioMixers)
	{
		std::vector<unsigned long> destinations;
		if (mixer)
		{
			auto params = mixer->GetBehaviourParams();
			if (const auto* wire = std::get_if<audio::WireMixBehaviourParams>(&params))
				for (auto channel : wire->Channels) destinations.push_back(channel);
			else if (const auto* merge = std::get_if<audio::MergeMixBehaviourParams>(&params))
				for (auto channel : merge->Channels) destinations.push_back(channel);
		}
		routes.push_back(std::move(destinations));
	}
	return routes;
}

double Station::MasterLevelForExport() const
{
	return _masterMixer ? _masterMixer->UnmutedLevel() : AudioMixer::DefaultLevel;
}

std::vector<double> Station::BusLevelsForExport() const
{
	std::vector<double> levels;
	levels.reserve(_audioMixers.size());
	for (const auto& mixer : _audioMixers)
		levels.push_back(mixer ? mixer->UnmutedLevel() : AudioMixer::DefaultLevel);
	return levels;
}

bool Station::RestoreMixerLevels(double masterLevel, const std::vector<double>& busLevels)
{
	if (!std::isfinite(masterLevel) || std::any_of(busLevels.begin(), busLevels.end(),
		[](double level) { return !std::isfinite(level); }))
		return false;
	if (busLevels.size() > (std::max)(_audioMixers.size(), _backAudioMixers.size()))
		return false;
	if (_masterMixer)
		_masterMixer->SetUnmutedLevel(masterLevel);
	if (_guiRack)
	{
		if (const auto slider = _guiRack->GetMasterSlider())
			slider->SetValue(masterLevel, true);
	}
	for (std::size_t index = 0u; index < busLevels.size(); ++index)
	{
		if (index < _audioMixers.size() && _audioMixers[index]) _audioMixers[index]->SetUnmutedLevel(busLevels[index]);
		if (index < _backAudioMixers.size() && _backAudioMixers[index]) _backAudioMixers[index]->SetUnmutedLevel(busLevels[index]);
		if (_guiRack)
		{
			if (const auto slider = _guiRack->GetChannelSlider(static_cast<unsigned int>(index)))
				slider->SetValue(busLevels[index], true);
		}
	}
	return true;
}

bool Station::RestoreAudioRoutes(const std::vector<std::vector<unsigned long>>& routes)
{
	const auto inputCount = (std::max)(_audioMixers.size(), _backAudioMixers.size());
	if (routes.size() > inputCount)
		return false;

	unsigned long requiredOutputCount = 0u;
	for (const auto& destinations : routes)
		for (auto output : destinations)
		{
			if (output >= io::JamFile::MaxAudioRouteChannels)
				return false;
			requiredOutputCount = (std::max)(requiredOutputCount, output + 1u);
		}
	if (requiredOutputCount > _guiRack->NumOutputChannels())
		_guiRack->SetNumOutputChannels(static_cast<unsigned int>(requiredOutputCount));

	std::vector<std::vector<unsigned int>> validated(inputCount);
	for (std::size_t input = 0u; input < routes.size(); ++input)
		for (auto output : routes[input])
		{
			if (std::find(validated[input].begin(), validated[input].end(), output) != validated[input].end())
				return false;
			validated[input].push_back(static_cast<unsigned int>(output));
		}

	_guiRack->ClearRoutes();
	for (std::size_t input = 0u; input < validated.size(); ++input)
	{
		for (auto output : validated[input])
			_guiRack->AddRoute(static_cast<unsigned int>(input), static_cast<unsigned int>(output));
	}
	for (std::size_t input = 0u; input < _audioMixers.size(); ++input)
		_audioMixers[input]->SetChannels(validated[input]);
	for (std::size_t input = 0u; input < _backAudioMixers.size(); ++input)
		_backAudioMixers[input]->SetChannels(validated[input]);
	return true;
}

AudioMixerParams Station::GetMixerParams(utils::Size2d stationSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, stationSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

void Station::SetSize(utils::Size2d size)
{
	GuiElement::SetSize(size);

	_ArrangeChildren();
}

graphics::CameraBounds Station::CameraDeckBoundsInWorld() const
{
	if (!_stationModel) return {};
	const auto deck = _stationModel->CameraLocalBounds().Transformed(
		{ 0.0f, _StationModelYOffset, 0.01f }, 1.0f);
	return deck.Transformed(PresentationModelPosition(), static_cast<float>(ModelScale()));
}

float Station::CameraTakeVisualScale() const noexcept
{
	return _masterMixer ? static_cast<float>(_masterMixer->UnmutedLevel()) : 1.0f;
}

bool Station::AllTakesMuted() const
{
	const auto& takes = GetLoopTakes();
	return !takes.empty() && std::all_of(takes.begin(), takes.end(),
		[](const auto& take) { return take && take->IsMuted(); });
}

void Station::Draw3d(base::DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (!_isVisible)
		return;

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	auto pos = PresentationModelPosition();
	auto scale = ModelScale();

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	// Draw halo deck before children so the rack/loops render on top.
	const auto stationVisualScale = _masterMixer ? static_cast<float>(_masterMixer->UnmutedLevel()) : 1.0f;
	for (auto& take : GetLoopTakes())
		take->SetParentVisualScale(stationVisualScale);
	if (_stationModel)
	{
		_stationModel->SetMuted(AllTakesMuted());
		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(0.0f, _StationModelYOffset, 0.01f)));
		const auto stationPeak = _masterMixer ? _masterMixer->VuPeakLevel() : 0.0f;
		_stationModel->SetStationState(GlobalId(), IsSelected(), _isPicking3d, stationPeak,
			static_cast<std::uint8_t>(GetVisualState()));
		_stationModel->Draw3d(ctx, 1, pass);
		glCtx.PopMvp();
	}

	for (auto& child : _children)
		child->Draw3d(ctx, 1, pass);

	if ((_quantisationModel || _quantisationDivisionModel) && (base::PASS_SCENE == pass))
	{
		// Refresh quantisation overlay state on the render thread (safe for
		// geometry and instance-buffer allocation).
		RefreshQuantisationOverlayFromClock();

		// Draw the semi-transparent quantisation overlay without writing to the
		// depth buffer (so it blends with already-rendered geometry and does not
		// occlude subsequent draws).  Face culling eliminates back-facing gate
		// panels that would otherwise accumulate a second alpha layer at the same
		// screen pixel, which pushes effective opacity well above the intended 70%.
		GLboolean prevDepthMask = GL_TRUE;
		glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
		const GLboolean wasCulling = glIsEnabled(GL_CULL_FACE);
		GLint prevCullFaceMode = GL_BACK;
		glGetIntegerv(GL_CULL_FACE_MODE, &prevCullFaceMode);

		glDepthMask(GL_FALSE);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);

		if (_quantisationModel)
			_quantisationModel->Draw3d(ctx, numInstances, pass);
		if (_quantisationDivisionModel)
			_quantisationDivisionModel->Draw3d(ctx, numInstances, pass);

		glDepthMask(prevDepthMask);
		glCullFace(prevCullFaceMode);
		if (!wasCulling)
			glDisable(GL_CULL_FACE);
	}

	glCtx.PopMvp();
	glCtx.PopMvp();
}

utils::Position3d Station::TopCapModelPosition() const
{
	auto position = PresentationModelPosition();
	position.Y += static_cast<float>(ModelScale() * _StationModelYOffset);
	return position;
}

utils::Position3d Station::PresentationModelPosition() const
{
	auto position = ModelPosition();
	position.X += _presentationOffsetX;
	return position;
}

void Station::BeginPresentationMove(float startX)
{
	_presentationOffsetX = _presentationStartOffsetX = startX - ModelPosition().X;
	_presentationElapsed = 0.0f;
}

bool Station::AdvancePresentationMove(float elapsedSeconds)
{
	if (!IsPresentationMoving()) return false;
	_presentationElapsed += std::clamp(elapsedSeconds, 0.0f, 0.05f);
	const float t = std::min(1.0f, _presentationElapsed / 0.25f);
	const float eased = t * t * (3.0f - 2.0f * t);
	_presentationOffsetX = _presentationStartOffsetX * (1.0f - eased);
	if (t >= 1.0f) FinishPresentationMove();
	return true; // Includes settling frame, so a stationary cursor is repicked.
}

void Station::FinishPresentationMove() noexcept
{
	_presentationOffsetX = _presentationStartOffsetX = _presentationElapsed = 0.0f;
}

utils::Position2d Station::Position() const
{
	return _modelScreenPos;
}

unsigned int Station::NumInputChannels(base::Audible::AudioSourceType source) const
{
	auto takeChans = 0u;
	auto maxChans = 0u;

	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		for (auto& take : _loopTakes)
		{
			takeChans = take->NumInputChannels(source);
			if (takeChans > maxChans)
				maxChans = takeChans;
		}
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		return NumBusChannels();
	}

	return maxChans;
}

unsigned int Station::NumOutputChannels(base::Audible::AudioSourceType source) const
{
	auto takeChans = 0u;
	auto maxChans = 0u;

	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		for (auto& take : _loopTakes)
		{
			takeChans = take->NumInputChannels(source);
			if (takeChans > maxChans)
				maxChans = takeChans;
		}
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		return NumBusChannels();
	}

	return maxChans;
}

void Station::Zero(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	if ((Audible::AUDIOSOURCE_LOOPS == source) || (Audible::AUDIOSOURCE_MIXER == source))
	{
		for (auto& channel : state->AudioBuffers)
		{
			if (channel)
				channel->Zero(numSamps);
		}
	}

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->Zero(numSamps, source);
}

// Only called when outputting to DAC
void Station::WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps,
	std::uint32_t blockStartSample, const audio::DacRenderContext* dac)
{
	auto ptr = Sharable::shared_from_this();
	auto state = _BorrowAudioState();
	if (!state)
		return;
	auto stationSink = std::dynamic_pointer_cast<MultiAudioSink>(ptr);
	for (const auto& take : state->OwnedLoopTakes)
	{
		if (!take)
			continue;

		take->WriteBlock(stationSink, trigger, indexOffset, numSamps, trigger ? nullptr : dac);
	}

	auto sampsToRead = (numSamps <= constants::MaxBlockSize) ? numSamps : constants::MaxBlockSize;
	auto masterLevel = static_cast<float>(_masterMixer->Level());
	auto masterPeak = 0.0f;
	const auto channelCount = (state->AudioBuffers.size() < state->AudioMixers.size()) ? state->AudioBuffers.size() : state->AudioMixers.size();

	_PrepareVstScratch(*state, sampsToRead);

	const auto& chain = state->MidiChain;
	const bool vstActive = (channelCount > 0u) && chain && chain->NumPlugins() > 0u && (state->VstBlockPtrs.size() >= channelCount);

	_RunVstBlock(chain.get(), *state, vstActive,
		static_cast<unsigned int>(channelCount), sampsToRead, blockStartSample);

	// Drive any wired parameter automation. Runs independently of vstActive so a
	// bypassed/idle chain still receives recorded parameter motion. Flat loop over
	// the pre-baked dispatch list — no weak_ptr locks, no shared_ptr chasing.
	_RunAutomationDispatch(blockStartSample, sampsToRead);

	if (channelCount == 0u)
	{
		_masterMixer->UpdateVu(0.0f, sampsToRead);
		_masterMixer->Offset(sampsToRead);
		return;
	}

	for (auto i = 0u; i < channelCount; i++)
	{
		auto* scratch = state->VstBlockPtrs[i];
		for (auto samp = 0u; samp < sampsToRead; samp++)
		{
			scratch[samp] *= masterLevel;
			auto absSamp = std::abs(scratch[samp]);
			if (absSamp > masterPeak)
				masterPeak = absSamp;
		}

		state->AudioMixers[i]->WriteBlock(dest, scratch, sampsToRead);
	}

	_masterMixer->UpdateVu(masterPeak, sampsToRead);
	_masterMixer->Offset(sampsToRead);
}

int Station::CurrentVstLatencySamps() const noexcept
{
	const auto chain = _vstChain.load(std::memory_order_acquire);
	return chain && !_rackInactive && !_rackLatencySuspended ? chain->GetLatencySamples() : 0;
}

void Station::PollPlaybackPluginChanges() const noexcept
{
	const auto state = _AudioStateSnapshot();
	if (!state) return;
	if (state->MidiChain) state->MidiChain->PollPendingChanges();
	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->PollPlaybackPluginChanges();
}

unsigned int Station::RecordedPlaybackLatencySamps() const noexcept
{
	const auto stationLatency = static_cast<unsigned int>(std::max(0, CurrentVstLatencySamps()));
	const auto state = _AudioStateSnapshot();
	if (!state) return stationLatency;
	auto longest = static_cast<std::uint64_t>(stationLatency);
	if (state->MidiChain)
		for (std::size_t index = 0; index < state->MidiDestinations.size(); ++index)
			longest = std::max(longest, static_cast<std::uint64_t>(std::max(0, state->MidiChain->GetLatencySamplesFrom(index))));
	for (const auto& take : state->OwnedLoopTakes)
		if (take) longest = std::max(longest,
			static_cast<std::uint64_t>(stationLatency) + take->RecordedPlaybackLatencySamps());
	return static_cast<unsigned int>(std::min(longest,
		static_cast<std::uint64_t>((std::numeric_limits<unsigned int>::max)())));
}

bool Station::CanConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps) const noexcept
{
	constexpr unsigned int MaxCompensationSamps = 262144u;
	const auto stationLatency = static_cast<unsigned int>(std::max(0, CurrentVstLatencySamps()));
	const auto state = _AudioStateSnapshot();
	if (!state) return true;
	for (const auto& take : state->OwnedLoopTakes)
		if (take && !take->CanConfigureRecordedPlaybackCompensation(
			referenceLatencySamps, stationLatency)) return false;
	if (!state->MidiChain) return true;
	for (auto index = std::size_t{ 0u }; index < state->MidiDestinations.size(); ++index)
	{
		const auto remaining = static_cast<unsigned int>(std::max(0, state->MidiChain->GetLatencySamplesFrom(index)));
		if (remaining > referenceLatencySamps
			|| referenceLatencySamps - remaining > MaxCompensationSamps) return false;
	}
	return true;
}

bool Station::ConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps)
{
	if (!CanConfigureRecordedPlaybackCompensation(referenceLatencySamps)) return false;
	const auto stationLatency = static_cast<unsigned int>(std::max(0, CurrentVstLatencySamps()));
	const auto state = _AudioStateSnapshot();
	if (!state) return true;
	for (const auto& take : state->OwnedLoopTakes)
		if (take && !take->ConfigureRecordedPlaybackCompensation(
			referenceLatencySamps, stationLatency)) return false;
	return ConfigureRecordedMidiCompensation(referenceLatencySamps);
}

bool Station::ConfigureRecordedMidiCompensation(unsigned int referenceLatencySamps) noexcept
{
	constexpr unsigned int MaxCompensationSamps = 262144u;
	const auto state = _AudioStateSnapshot();
	if (!state || !state->MidiChain) return true;
	for (auto index = std::size_t{ 0u }; index < state->MidiDestinations.size(); ++index)
	{
		const auto remaining = std::max(0, state->MidiChain->GetLatencySamplesFrom(index));
		if (static_cast<unsigned int>(remaining) > referenceLatencySamps
			|| referenceLatencySamps - static_cast<unsigned int>(remaining) > MaxCompensationSamps)
			return false;
	}
	for (auto index = std::size_t{ 0u }; index < state->MidiDestinations.size(); ++index)
	{
		const auto remaining = std::max(0, state->MidiChain->GetLatencySamplesFrom(index));
		const auto delay = referenceLatencySamps - static_cast<unsigned int>(remaining);
		auto& destination = *state->MidiDestinations[index];
		if (destination.CompensationDelaySamps.load(std::memory_order_relaxed) != delay)
		{
			destination.CompensationDelaySamps.store(delay, std::memory_order_release);
			destination.CompensationEpoch.fetch_add(1u, std::memory_order_release);
		}
	}
	return true;
}

void Station::_PrepareMidiDestinations(AudioState& state)
{
	state.MidiChain = (_rackInactive || _rackLatencySuspended) ? nullptr : _vstChain.load(std::memory_order_acquire);
	const auto previous = _AudioStateSnapshot();
	auto queueRemovedBindings = [&]() {
		if (!previous) return;
		for (const auto& routes : previous->MidiPlaybackRoutes)
			for (const auto& binding : routes)
			{
				if (!binding || binding->ReleaseQueued
					|| binding->ReleaseConsumed.load(std::memory_order_acquire)) continue;
				bool stillRouted = false;
				for (const auto& currentRoutes : state.MidiPlaybackRoutes)
					for (const auto& current : currentRoutes) stillRouted |= current == binding;
				if (stillRouted
					|| std::find(_pendingLostPlaybackMidi.begin(), _pendingLostPlaybackMidi.end(), binding)
						!= _pendingLostPlaybackMidi.end()) continue;
				binding->EmitSourceLossRelease = true;
				_pendingLostPlaybackMidi.push_back(binding);
			}
		_FlushPendingLostPlaybackMidi();
	};
	auto retainRetired = [&]() {
		auto retain = [&](const std::shared_ptr<MidiDestination>& candidate) {
			if (!candidate
				|| std::find(state.MidiDestinations.begin(), state.MidiDestinations.end(), candidate) != state.MidiDestinations.end()
				|| std::find(state.RetiredMidiDestinations.begin(), state.RetiredMidiDestinations.end(), candidate) != state.RetiredMidiDestinations.end()) return;
			state.RetiredMidiDestinations.push_back(candidate);
		};
		if (previous)
		{
			for (const auto& destination : previous->MidiDestinations) retain(destination);
			for (const auto& destination : previous->RetiredMidiDestinations)
				if (!destination->RetirementReleased.load(std::memory_order_acquire)) retain(destination);
		}
		for (const auto& binding : _midiPlaybackBindings)
			if (binding->QueuedEvents.load(std::memory_order_acquire) > 0u) retain(binding->Destination);
	};
	if (!state.MidiChain)
	{
		retainRetired();
		queueRemovedBindings();
		for (const auto& binding : _midiPlaybackBindings)
			binding->RoutingActive.store(false, std::memory_order_release);
		return;
	}
	std::erase_if(_midiDestinations, [](const auto& entry) { return entry.expired(); });
	std::vector<std::shared_ptr<MidiDestination>> destinations;
	for (std::size_t index = 0u; index < state.MidiChain->NumPlugins(); ++index)
	{
		const auto plugin = state.MidiChain->GetPlugin(index);
		std::shared_ptr<MidiDestination> destination;
		for (const auto& weak : _midiDestinations)
			if (const auto existing = weak.lock(); existing && existing->Plugin == plugin && !existing->RetirementReleased.load(std::memory_order_acquire))
				{ destination = existing; break; }
		if (!destination)
		{
			destination = std::make_shared<MidiDestination>();
			destination->Plugin = plugin;
			destination->RenderChannels = plugin && plugin->HostedChannelCount() > 0u ? plugin->HostedChannelCount()
				: std::max(1u, static_cast<unsigned int>(state.AudioBuffers.size()));
			_midiDestinations.push_back(destination);
		}
		destinations.push_back(std::move(destination));
	}
	const auto* routes = _midiVstRoutes.load(std::memory_order_acquire);
	// Preserve the existing broadcast fallback, including partially mapped routes.
	auto resolve = [&](unsigned int output) {
		const auto pluginIndex = routes ? routes->PluginForOutput(output) : MidiVstRoutingSnapshot::NoPlugin;
		if (pluginIndex == MidiVstRoutingSnapshot::MissingPlugin)
			return std::vector<std::shared_ptr<MidiDestination>>{};
		if (pluginIndex < destinations.size())
			return std::vector<std::shared_ptr<MidiDestination>>{ destinations[pluginIndex] };
		return destinations;
	};
	state.LiveMidiDestinations = resolve(LiveMidiOutputIndex);
	for (auto channel = 0u; channel < 16u; ++channel)
	{
		const auto& route = _liveVstChannelRoutes[channel];
		auto& prepared = state.LiveChannelDestinations[channel];
		for (const auto& destination : state.LiveMidiDestinations)
		{
			if (!destination->Plugin) continue;
			const auto capabilities = destination->Plugin->MidiInput();
			// Legacy adapters/test processors retain broadcast compatibility. New
			// explicit cables require discovered capability at the edit boundary.
			if ((capabilities.Known && !capabilities.Notes && !capabilities.Controllers) || (!capabilities.Known && _rackRenderPolicy != vst::VstChain::RenderPolicy::LegacySerial)) continue;
			if (!route.Explicit || std::find(route.PluginIds.begin(), route.PluginIds.end(),
				destination->Plugin->StateId()) != route.PluginIds.end()) prepared.push_back(destination);
		}
		// Explicit live routing replaces only the legacy live selection.
		if (route.Explicit)
			for (const auto& destination : destinations)
			{
				if (!destination->Plugin || std::find(prepared.begin(), prepared.end(), destination) != prepared.end()) continue;
				const auto capability = destination->Plugin->MidiInput();
				if (_rackRenderPolicy != vst::VstChain::RenderPolicy::LegacySerial && (!capability.Known || (!capability.Notes && !capability.Controllers))) continue;
				if (std::find(route.PluginIds.begin(), route.PluginIds.end(), destination->Plugin->StateId()) != route.PluginIds.end())
					prepared.push_back(destination);
			}
	}
	// Route history matters within the current chain. Removed plugins are unloaded
	// by the structural owner; do not repin their destinations on every publication.
	state.MidiDestinations = destinations;
	state.LiveDestinationChannels.assign(destinations.size(), 0u);
	for (auto index = 0u; index < destinations.size(); ++index)
		for (auto channel = 0u; channel < 16u; ++channel)
			if (std::find(state.LiveChannelDestinations[channel].begin(), state.LiveChannelDestinations[channel].end(), destinations[index]) != state.LiveChannelDestinations[channel].end())
				state.LiveDestinationChannels[index] |= static_cast<std::uint16_t>(1u << channel);
	for (const auto& take : state.OwnedLoopTakes)
		for (const auto& loop : take->GetMidiLoopSnapshot())
		{
			std::vector<std::shared_ptr<MidiPlaybackBinding>> bindings;
			for (const auto& destination : resolve(static_cast<unsigned int>(state.MidiPlaybackRoutes.size())))
			{
				std::shared_ptr<MidiPlaybackBinding> binding;
				for (const auto& existing : _midiPlaybackBindings)
					if (existing->Take == take &&
						existing->Loop == loop && existing->Destination == destination)
						{ binding = existing; break; }
				if (!binding)
				{
					binding = std::make_shared<MidiPlaybackBinding>();
					binding->Take = take;
					binding->Loop = loop;
					binding->Destination = destination;
					_midiPlaybackBindings.push_back(binding);
				}
				bindings.push_back(std::move(binding));
			}
			state.MidiPlaybackRoutes.push_back(std::move(bindings));
		}
	for (const auto& binding : _midiPlaybackBindings)
	{
		bool active = false;
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& route : routes)
				active |= route == binding;
		binding->RoutingActive.store(active, std::memory_order_release);
	}
	retainRetired();
	queueRemovedBindings();
}

bool Station::_TryDeliverMidi(MidiDestination& destination, const MidiEvent& event,
	bool isRealtime, MidiPlaybackBinding* owner) noexcept
{
	if (isRealtime) return owner
		? _TryDeliverRecordedMidiNow(destination, event, *owner, true)
		: _TryDeliverMidiNow(destination, event, true);
	const auto delay = destination.ActiveCompensationDelaySamps;
	if (delay == 0u) return owner
		? _TryDeliverRecordedMidiNow(destination, event, *owner, false)
		: _TryDeliverMidiNow(destination, event, false);
	if (!destination.Plugin || !destination.Plugin->IsLoaded()) return event.IsNoteOff();
	const auto& scheduled = destination.Scheduled;
	const auto count = destination.ScheduledCount;
	const bool release = event.IsNoteOff() || (event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 < 64u);
	if (count >= MidiDestination::ScheduledCapacity
		|| (!release && count >= MidiDestination::ScheduledCapacity
			- MidiDestination::ScheduledReleaseReserve)) return false;
	MidiDestination::ScheduledMidi entry;
	entry.Event = event;
	entry.Event.sampleOffset += delay; // Modular sample-clock arithmetic.
	entry.IsRealtime = isRealtime;
	entry.Owner = owner;
	if (owner) owner->QueuedEvents.fetch_add(1u, std::memory_order_release);
	entry.Sequence = ++destination.ScheduleSequence;
	auto precedes = [](const MidiDestination::ScheduledMidi& left,
		const MidiDestination::ScheduledMidi& right) noexcept {
		const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
			right.Event.sampleOffset);
		if (delta != 0) return delta < 0;
		if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
		return left.Sequence < right.Sequence;
	};
	auto index = destination.ScheduledCount++;
	while (index > 0u)
	{
		const auto parent = (index - 1u) / 2u;
		if (!precedes(entry, scheduled[parent])) break;
		destination.Scheduled[index] = scheduled[parent];
		index = parent;
	}
	destination.Scheduled[index] = entry;
	return true;
}

void Station::_DrainScheduledMidi(const AudioState& state,
	std::uint32_t blockStartSample, unsigned int numSamps) noexcept
{
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination) continue;
		auto precedes = [](const MidiDestination::ScheduledMidi& left,
			const MidiDestination::ScheduledMidi& right) noexcept {
			const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
				right.Event.sampleOffset);
			if (delta != 0) return delta < 0;
			if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
			return left.Sequence < right.Sequence;
		};
		while (destination->ScheduledCount > 0u
			&& midi::ClassifyMidiSampleInBlock(destination->Scheduled[0].Event.sampleOffset,
				blockStartSample, numSamps) != midi::MidiBlockSamplePosition::Future)
		{
			auto entry = destination->Scheduled[0];
			auto last = destination->Scheduled[--destination->ScheduledCount];
			auto parent = std::size_t{ 0u };
			while (parent * 2u + 1u < destination->ScheduledCount)
			{
				auto child = parent * 2u + 1u;
				if (child + 1u < destination->ScheduledCount
					&& precedes(destination->Scheduled[child + 1u], destination->Scheduled[child])) ++child;
				if (!precedes(destination->Scheduled[child], last)) break;
				destination->Scheduled[parent] = destination->Scheduled[child];
				parent = child;
			}
			if (destination->ScheduledCount > 0u) destination->Scheduled[parent] = last;
			if (midi::ClassifyMidiSampleInBlock(entry.Event.sampleOffset,
				blockStartSample, numSamps) == midi::MidiBlockSamplePosition::Late)
				entry.Event.sampleOffset = blockStartSample;
			const bool delivered = entry.Owner
				? _TryDeliverRecordedMidiNow(*destination, entry.Event, *entry.Owner, entry.IsRealtime)
				: _TryDeliverMidiNow(*destination, entry.Event, entry.IsRealtime);
			if (!delivered && entry.Event.IsNoteOn() && entry.Owner)
			{
				if (entry.Owner->Loop) entry.Owner->Loop->RequestHeldFlush();
				_RecoverCancelledScheduledMidi(*destination,
					_CancelScheduledMidiForOwner(*destination, entry.Owner));
			}
			if (entry.Owner) entry.Owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
		}
	}
}

std::bitset<midi::MidiNote::TotalNoteSlots> Station::_CancelScheduledMidiForOwner(MidiDestination& destination,
	MidiPlaybackBinding* owner) noexcept
{
	std::bitset<midi::MidiNote::TotalNoteSlots> affected;
	if (!owner || destination.ScheduledCount == 0u) return affected;
	owner->QueuedEvents.fetch_add(1u, std::memory_order_release); // Pin through heap compaction.
	_DropRecordedAudibleOwner(destination, *owner);
	for (auto index = std::size_t{ 0u }; index < destination.ScheduledCount;)
	{
		if (destination.Scheduled[index].Owner != owner) { ++index; continue; }
		const auto& removed = destination.Scheduled[index];
		if (removed.Event.IsNoteOn() || removed.Event.IsNoteOff())
			affected.set(midi::MidiNote::NoteSlot(removed.Event.Channel(), removed.Event.data1));
		owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
		destination.Scheduled[index] = destination.Scheduled[--destination.ScheduledCount];
	}
	auto precedes = [](const MidiDestination::ScheduledMidi& left,
		const MidiDestination::ScheduledMidi& right) noexcept {
		const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
			right.Event.sampleOffset);
		if (delta != 0) return delta < 0;
		if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
		return left.Sequence < right.Sequence;
	};
	for (auto root = destination.ScheduledCount / 2u; root > 0u;)
	{
		--root;
		const auto entry = destination.Scheduled[root];
		auto parent = root;
		while (parent * 2u + 1u < destination.ScheduledCount)
		{
			auto child = parent * 2u + 1u;
			if (child + 1u < destination.ScheduledCount
				&& precedes(destination.Scheduled[child + 1u], destination.Scheduled[child])) ++child;
			if (!precedes(destination.Scheduled[child], entry)) break;
			destination.Scheduled[parent] = destination.Scheduled[child];
			parent = child;
		}
		destination.Scheduled[parent] = entry;
	}
	owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
	return affected;
}

void Station::_RecoverCancelledScheduledMidi(MidiDestination& destination,
	const std::bitset<midi::MidiNote::TotalNoteSlots>& affected) noexcept
{
	if (affected.none()) return;
	destination.RecoveringCompensationChange = true;
	for (auto slot = std::size_t{ 0u }; slot < midi::MidiNote::TotalNoteSlots; ++slot)
		if (affected.test(slot) && destination.PluginHeld.test(slot)
			&& destination.PlaybackOwners[slot] == 0u
			&& destination.RecordedAudibleOwners[slot] == 0u
			&& !destination.LiveHeld.test(slot))
			destination.PendingReleases.set(slot);
}

void Station::_InvalidateScheduledMidiAtBlockStart(const AudioState& state, bool forceCancel) noexcept
{
	// Retired plugins are outside the active chain; their future notes must never drain.
	for (const auto& destination : state.RetiredMidiDestinations)
	{
		if (!destination) continue;
		for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
		{
			if (auto* owner = destination->Scheduled[index].Owner)
			{
				if (owner->Loop) owner->Loop->RequestHeldFlush();
				_DropRecordedAudibleOwner(*destination, *owner);
				owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
			}
		}
		destination->ScheduledCount = 0u;
	}
	for (const auto& routes : state.MidiPlaybackRoutes)
		for (const auto& binding : routes)
		{
			if (!binding || !binding->Loop || !binding->Destination) continue;
			const auto revision = binding->Loop->PublishedRevisionAtAudioBoundary();
			const auto relocation = binding->Loop->RelocationGenerationAtAudioBoundary();
			const bool changed = (binding->HasObservedSourceState
				&& (static_cast<bool>(revision) != binding->HasObservedSourceRevision
					|| (revision && *revision != binding->ObservedSourceRevision)))
				|| relocation != binding->ObservedRelocationGeneration;
			binding->HasObservedSourceState = true;
			binding->HasObservedSourceRevision = revision.has_value();
			if (revision) binding->ObservedSourceRevision = *revision;
			binding->ObservedRelocationGeneration = relocation;
			binding->CancelScheduledThisBlock = changed || binding->Take->IsMuted()
				|| binding->Take->IsCaptureCancelled();
			if (changed) binding->Loop->RequestHeldFlush();
		}
	// A removed route can still own future entries in a destination retained by the new graph.
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination) continue;
		const bool bypassed = destination->Plugin && destination->Plugin->IsBypassed();
		const bool bypassStarted = bypassed && !destination->LastBypassed;
		if (bypassStarted)
		{
			destination->PendingReleases |= destination->LiveHeld;
			destination->LiveHeld.reset();
			destination->PendingSustainRelease |= destination->LiveSustain;
			destination->LiveSustain.reset();
		}
		destination->LastBypassed = bypassed;
		std::bitset<midi::MidiNote::TotalNoteSlots> affected;
		auto retained = std::size_t{ 0u };
		for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
		{
			const auto entry = destination->Scheduled[index];
			auto* owner = entry.Owner;
			if (owner && (forceCancel || bypassStarted
				|| !owner->RoutingActive.load(std::memory_order_acquire)
				|| owner->CancelScheduledThisBlock))
			{
				if (owner->Loop) owner->Loop->RequestHeldFlush();
				_DropRecordedAudibleOwner(*destination, *owner);
				if (entry.Event.IsNoteOn() || entry.Event.IsNoteOff())
					affected.set(midi::MidiNote::NoteSlot(entry.Event.Channel(), entry.Event.data1));
				owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
			}
			else destination->Scheduled[retained++] = entry;
		}
		if (retained != destination->ScheduledCount)
		{
			destination->ScheduledCount = retained;
			auto precedes = [](const MidiDestination::ScheduledMidi& left,
				const MidiDestination::ScheduledMidi& right) noexcept {
				const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset, right.Event.sampleOffset);
				if (delta != 0) return delta < 0;
				if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
				return left.Sequence < right.Sequence;
			};
			for (auto root = retained / 2u; root > 0u;)
			{
				--root;
				const auto entry = destination->Scheduled[root];
				auto parent = root;
				while (parent * 2u + 1u < retained)
				{
					auto child = parent * 2u + 1u;
					if (child + 1u < retained && precedes(destination->Scheduled[child + 1u], destination->Scheduled[child])) ++child;
					if (!precedes(destination->Scheduled[child], entry)) break;
					destination->Scheduled[parent] = destination->Scheduled[child];
					parent = child;
				}
				destination->Scheduled[parent] = entry;
			}
			_RecoverCancelledScheduledMidi(*destination, affected);
		}
	}
}

void Station::_DropRecordedAudibleOwner(MidiDestination& destination, MidiPlaybackBinding& owner,
	bool emitRelease) noexcept
{
	for (auto channel = 0u; channel < 16u; ++channel)
		if (owner.AudibleSustain.test(channel))
		{
			owner.AudibleSustain.reset(channel);
			if (destination.RecordedSustainOwners[channel]) --destination.RecordedSustainOwners[channel];
			if (!destination.RecordedSustainOwners[channel] && !destination.LiveSustain.test(channel))
				destination.PendingSustainRelease.set(channel);
		}
	for (auto index = std::size_t{ 0u }; index < owner.AudibleSlotCount; ++index)
	{
		const auto slot = owner.AudibleSlots[index];
		owner.AudibleHeld.reset(slot);
		if (destination.RecordedAudibleOwners[slot] > 0u)
			--destination.RecordedAudibleOwners[slot];
		if (emitRelease && destination.RecordedAudibleOwners[slot] == 0u
			&& !destination.LiveHeld.test(slot) && destination.PluginHeld.test(slot))
			destination.PendingReleases.set(slot);
	}
	owner.AudibleSlotCount = 0u;
}

bool Station::_TryDeliverRecordedMidiNow(MidiDestination& destination, const MidiEvent& event,
	MidiPlaybackBinding& owner, bool isRealtime) noexcept
{
	if (event.MessageType() == 0xb0u && event.data1 == 64u)
	{
		const auto channel = event.Channel();
		const bool down = event.data2 >= 64u;
		if (down)
		{
			if (!_TryDeliverMidiNow(destination, event, isRealtime)) return false;
			if (!owner.AudibleSustain.test(channel))
			{
				owner.AudibleSustain.set(channel);
				++destination.RecordedSustainOwners[channel];
			}
		}
		else if (owner.AudibleSustain.test(channel))
		{
			if (destination.RecordedSustainOwners[channel] == 1u && !destination.LiveSustain.test(channel)
				&& !_TryDeliverMidiNow(destination, event, isRealtime)) return false;
			owner.AudibleSustain.reset(channel);
			--destination.RecordedSustainOwners[channel];
		}
		return true;
	}
	if (!event.IsNoteOn() && !event.IsNoteOff())
		return _TryDeliverMidiNow(destination, event, isRealtime);
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	if (event.IsNoteOn())
	{
		if (!_TryDeliverMidiNow(destination, event, isRealtime)) return false;
		if (!owner.AudibleHeld.test(slot))
		{
			owner.AudibleHeld.set(slot);
			owner.AudibleSlotPositions[slot] = static_cast<std::uint16_t>(owner.AudibleSlotCount);
			owner.AudibleSlots[owner.AudibleSlotCount++] = static_cast<std::uint16_t>(slot);
			++destination.RecordedAudibleOwners[slot];
		}
		return true;
	}
	if (!owner.AudibleHeld.test(slot)) return true;
	if (destination.RecordedAudibleOwners[slot] == 1u && !destination.LiveHeld.test(slot)
		&& !_TryDeliverMidiNow(destination, event, isRealtime)) return false;
	owner.AudibleHeld.reset(slot);
	const auto index = owner.AudibleSlotPositions[slot];
	const auto moved = owner.AudibleSlots[--owner.AudibleSlotCount];
	owner.AudibleSlots[index] = moved;
	owner.AudibleSlotPositions[moved] = index;
	--destination.RecordedAudibleOwners[slot];
	return true;
}

bool Station::_TryDeliverMidiNow(MidiDestination& destination, const MidiEvent& event,
	bool isRealtime) noexcept
{
	if (!destination.Plugin || !destination.Plugin->IsLoaded()) return event.IsNoteOff();
	if (destination.Plugin->IsBypassed() && (event.IsNoteOn() || (event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 >= 64u))) return false;
	const bool sustain = event.MessageType() == 0xb0u && event.data1 == 64u;
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	if (event.IsNoteOn() && destination.PendingReleases.test(slot))
	{
		const auto release = MidiEvent::MakeNoteOff(event.sampleOffset, event.Channel(), event.data1,
			destination.PendingReleaseVelocity[slot]);
		if (!destination.Plugin->TrySendMidiEvent(release, true)) return false;
		destination.PendingReleaseVelocity[slot] = 0u;
		destination.PendingReleases.reset(slot);
		destination.PluginHeld.reset(slot);
		destination.BlockTouched.set(slot);
	}
	if (!destination.Plugin->TrySendMidiEvent(event, isRealtime))
	{
		if (sustain && event.data2 < 64u)
		{
			destination.PendingSustainRelease.set(event.Channel());
			return true;
		}
		if (event.IsNoteOff())
		{
			destination.PendingReleaseVelocity[slot] = event.data2;
			destination.PendingReleases.set(slot);
			return true; // Retained for bounded retry at the next audio block.
		}
		return false;
	}
	if (event.IsNoteOn()) destination.PluginHeld.set(slot);
	if (sustain)
	{
		destination.PluginSustain.set(event.Channel(), event.data2 >= 64u);
		if (event.data2 < 64u) destination.PendingSustainRelease.reset(event.Channel());
	}
	else if (event.IsNoteOff())
	{
		destination.PluginHeld.reset(slot);
		destination.PendingReleaseVelocity[slot] = 0u;
		destination.PendingReleases.reset(slot);
	}
	if (event.IsNoteOn() || event.IsNoteOff()) destination.BlockTouched.set(slot);
	return true;
}

void Station::_DrainPendingMidiReleases(const AudioState& state, std::uint32_t sample) noexcept
{
	constexpr unsigned int MaxRecoveryReleasesPerDestination = 64u;
	for (const auto& destination : state.MidiDestinations)
	{
		if (destination && destination->Plugin)
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->PendingSustainRelease.test(channel))
					_TryDeliverMidiNow(*destination, {sample, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, true);
		if (!destination || !destination->Plugin || !destination->Plugin->IsLoaded()
			|| destination->PendingReleases.none()) continue;
		auto budget = MaxRecoveryReleasesPerDestination;
		auto scanned = std::size_t{ 0u };
		while (budget > 0u && scanned < midi::MidiNote::TotalNoteSlots)
		{
			const auto slot = destination->ReleaseCursor;
			if (destination->PendingReleases.test(slot))
			{
				const auto release = MidiEvent::MakeNoteOff(sample,
					static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u),
					destination->PendingReleaseVelocity[slot]);
				if (!destination->Plugin->TrySendMidiEvent(release, true)) break;
				destination->PendingReleaseVelocity[slot] = 0u;
				destination->PendingReleases.reset(slot);
				destination->PluginHeld.reset(slot);
				destination->BlockTouched.set(slot);
				--budget;
			}
			destination->ReleaseCursor = (slot + 1u) % midi::MidiNote::TotalNoteSlots;
			++scanned;
		}
	}
}

void Station::_RecoverSkippedMidiBlocks(const AudioState& state) noexcept
{
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination || !destination->Plugin || destination->Plugin->MidiBlockWasProcessed()) continue;
		// All accepted events in this block were discarded by the adapter.
		destination->PendingReleases |= destination->BlockHeldAtStart | destination->BlockTouched;
		destination->PendingSustainRelease |= destination->BlockSustainAtStart | destination->PluginSustain;
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& binding : routes)
				if (binding && binding->Destination == destination)
					_DropRecordedAudibleOwner(*destination, *binding);
		destination->PluginHeld.reset();
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& binding : routes)
				if (binding && binding->Destination == destination && binding->Loop)
					binding->Loop->RequestHeldFlush();
	}
}

void Station::_DispatchLiveMidi(const AudioState& state, const MidiEvent& event,
	bool sourceLossNoteOff, std::uint32_t liveNoteOnRevision) noexcept
{
	if (event.MessageType() == 0xb0u && event.data1 == 64u)
	{
		const auto channel = event.Channel();
		if (event.data2 < 64u)
		{
			for (const auto& destination : state.MidiDestinations)
				if (destination->LiveSustain.test(channel))
				{
					if (sourceLossNoteOff && static_cast<std::int32_t>(destination->LiveSustainRevision[channel] - liveNoteOnRevision) > 0) continue;
					destination->LiveSustain.reset(channel);
					if (!destination->RecordedSustainOwners[channel]) _TryDeliverMidi(*destination, event, true);
				}
		}
		else
			for (const auto& destination : state.LiveChannelDestinations[channel])
				if (_TryDeliverMidi(*destination, event, true))
				{
					destination->LiveSustain.set(channel);
					destination->LiveSustainRevision[channel] = liveNoteOnRevision;
				}
		return;
	}
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	auto hasNewLiveOwner = [&](const MidiDestination& destination) noexcept {
		return destination.LiveHeld.test(slot) &&
			static_cast<std::int32_t>(destination.LiveNoteOnRevision[slot] - liveNoteOnRevision) > 0;
	};
	if (event.IsNoteOff())
	{
		for (const auto& destination : state.MidiDestinations)
		{
			const bool wasHeld = destination->LiveHeld.test(slot);
			if (sourceLossNoteOff && hasNewLiveOwner(*destination)) continue;
			destination->LiveHeld.reset(slot);
			if (wasHeld && destination->RecordedAudibleOwners[slot] == 0u &&
				std::find(state.LiveChannelDestinations[event.Channel()].begin(), state.LiveChannelDestinations[event.Channel()].end(), destination) == state.LiveChannelDestinations[event.Channel()].end())
				_TryDeliverMidi(*destination, event, true);
		}
	}
	for (const auto& destination : state.LiveChannelDestinations[event.Channel()])
	{
		if (event.IsNoteOff() && (destination->RecordedAudibleOwners[slot] != 0u
			|| (sourceLossNoteOff && hasNewLiveOwner(*destination))
			|| (destination->BlockTouched.test(slot) && !destination->PluginHeld.test(slot)))) continue;
		if (_TryDeliverMidi(*destination, event, true) && event.IsNoteOn())
		{
			destination->PendingLiveReset.reset(slot);
			destination->LiveHeld.set(slot);
			destination->LiveNoteOnRevision[slot] = liveNoteOnRevision;
		}
	}
}

void Station::MidiPlaybackSink::OnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept
{
	(void)TryOnEvent(outputIndex, event);
}

bool Station::MidiPlaybackSink::TryOnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept
{
	if (outputIndex >= _state.MidiPlaybackRoutes.size()) return false;
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	// Every destination must retain a release before the source forgets it.
	// Attacks still establish source state if any destination accepts them.
	bool accepted = event.IsNoteOff();
	for (const auto& binding : _state.MidiPlaybackRoutes[outputIndex])
	{
		if (binding->Take->IsSourceLossCancelled() && event.IsNoteOn()) continue;
		auto& destination = *binding->Destination;
		const bool suppressSource = binding->Take->IsMuted() || binding->Take->IsSourceLossCancelled();
		if (suppressSource && event.IsNoteOff())
			Station::_RecoverCancelledScheduledMidi(destination,
				Station::_CancelScheduledMidiForOwner(destination, binding.get()));
		if (event.IsNoteOff() && !binding->Held.test(slot))
			continue;
		const bool immediateRelease = event.IsNoteOff() && destination.RecoveringCompensationChange;
		const bool delivered = Station::_TryDeliverMidi(destination, event,
			suppressSource || immediateRelease, binding.get());
		if (event.IsNoteOff()) accepted &= delivered;
		else accepted |= delivered;
		if (event.IsNoteOn() && delivered && !binding->Held.test(slot))
		{
			binding->Held.set(slot);
			++destination.PlaybackOwners[slot];
			binding->HasHeld.store(true, std::memory_order_release);
		}
		else if (event.IsNoteOff() && delivered && binding->Held.test(slot))
		{
			binding->Held.reset(slot);
			--destination.PlaybackOwners[slot];
			binding->HasHeld.store(binding->Held.any(), std::memory_order_release);
		}
	}
	return accepted;
}

bool Station::_ReleaseLostPlaybackMidi(MidiPlaybackBinding& binding, std::uint32_t sample,
	unsigned int& budget, unsigned int& slotBudget) noexcept
{
	auto& destination = *binding.Destination;
	_DropRecordedAudibleOwner(destination, binding, false);
	const auto cancelled = _CancelScheduledMidiForOwner(destination, &binding);
	if (binding.EmitSourceLossRelease) _RecoverCancelledScheduledMidi(destination, cancelled);
	if (binding.Held.none())
	{
		binding.HasHeld.store(false, std::memory_order_release);
		return true;
	}
	while (binding.ReleaseSlot < midi::MidiNote::TotalNoteSlots && budget > 0u && slotBudget > 0u)
	{
		const auto slot = binding.ReleaseSlot++;
		--slotBudget;
		if (!binding.Held.test(slot)) continue;
		--budget;
		binding.Held.reset(slot);
		--destination.PlaybackOwners[slot];
		if (binding.EmitSourceLossRelease && destination.PlaybackOwners[slot] == 0u
			&& destination.RecordedAudibleOwners[slot] == 0u && !destination.LiveHeld.test(slot))
		{
			destination.PendingReleaseVelocity[slot] = 0u;
			destination.PendingReleases.reset(slot); // This explicit release supersedes queued recovery.
			_TryDeliverMidi(destination, MidiEvent::MakeNoteOff(sample,
				static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
		}
	}
	if (binding.ReleaseSlot < midi::MidiNote::TotalNoteSlots) return false;
	binding.HasHeld.store(false, std::memory_order_release);
	return true;
}

void Station::_QueueLostPlaybackMidi(const std::shared_ptr<LoopTake>& take, bool emitRelease)
{
	for (const auto& binding : _midiPlaybackBindings)
		if (binding->Take == take &&
			!binding->ReleaseQueued && !binding->ReleaseConsumed.load(std::memory_order_acquire) &&
			std::find(_pendingLostPlaybackMidi.begin(), _pendingLostPlaybackMidi.end(), binding) == _pendingLostPlaybackMidi.end())
		{
			binding->EmitSourceLossRelease = emitRelease;
			_pendingLostPlaybackMidi.push_back(binding);
		}
	_FlushPendingLostPlaybackMidi();
}

void Station::_FlushPendingLostPlaybackMidi()
{
	std::erase_if(_pendingLostPlaybackMidi, [](const auto& binding) {
		return binding->ReleaseConsumed.load(std::memory_order_acquire);
	});
	for (const auto& binding : _pendingLostPlaybackMidi)
	{
		if (binding->ReleaseQueued) continue;
		auto event = MidiEvent::MakeNoteOff(_hasLastSyntheticLiveMidiSample ? _lastSyntheticLiveMidiSample : 0u, 0u, 0u);
		if (!_TryEnqueueSyntheticMidi({ event, binding.get() })) return;
		binding->ReleaseQueued = true;
	}
}

void Station::_PrepareVstScratch(const AudioState& state, unsigned int sampsToRead) noexcept
{
	const auto channelCount = std::min(state.AudioBuffers.size(), state.AudioMixers.size());
	for (auto channel = channelCount; channel < state.VstBlockPtrs.size(); ++channel)
		std::fill_n(state.VstBlockPtrs[channel], sampsToRead, 0.0f);
	for (auto i = 0u; i < channelCount; i++)
	{
		auto* scratch = (i < state.VstBlockPtrs.size()) ? state.VstBlockPtrs[i] : nullptr;
		if (!scratch)
			continue;

		const auto& source = state.AudioBuffers[i];
		source->Delay(sampsToRead);
		auto sourcePtr = source->PlaybackRead(scratch, sampsToRead);
		if (sourcePtr != scratch)
			std::copy(sourcePtr, sourcePtr + sampsToRead, scratch);
	}
}

void Station::_RunVstBlock(vst::VstChain* chain,
	const AudioState& state,
	bool vstActive,
	unsigned int channelCount,
	unsigned int sampsToRead,
	std::uint32_t blockStartSample) noexcept
{
	for (const auto& destination : state.RetiredMidiDestinations)
		if (destination->Plugin && !destination->RetirementReleased.load(std::memory_order_acquire))
		{
			vst::IVstPlugin::PluginCallGuard call(*destination->Plugin, true);
			if (call) destination->Plugin->BeginMidiBlock(blockStartSample, sampsToRead);
			destination->BlockHeldAtStart = destination->PluginHeld;
			destination->BlockSustainAtStart = destination->PluginSustain;
			destination->PendingReleases |= destination->PluginHeld;
			destination->PendingSustainRelease |= destination->PluginSustain;
			destination->LiveHeld.reset();
			destination->LiveSustain.reset();
		}
	if (vstActive)
	{
		vst::HostTimeState hostTime;
		hostTime.sampleRate = static_cast<double>(_sampleRate);
		hostTime.isPlaying  = true;
		if (_clock)
		{
			// NINJAM replaces the musical Timer epoch to establish remote phase.
			// VST project time must remain continuous across that normal follow
			// operation; scene time is the callback-owned monotonic ruler.
			hostTime.samplePos = _clock->SceneSamplePos();
		}
		else
		{
			hostTime.samplePos = static_cast<std::uint64_t>(blockStartSample);
		}
		if (_clock)
		{
			const auto musicalPosition = _clock->CurrentMusicalPosition(
				static_cast<unsigned int>(_sampleRate));
			if (musicalPosition.IsValid)
			{
				hostTime.ppqPos = musicalPosition.Ppq;
				hostTime.hasPpqPos = true;
				hostTime.musicalPositionChanged = musicalPosition.PositionChanged;
				hostTime.tempo = musicalPosition.Tempo;
				hostTime.bpi = musicalPosition.BeatsPerInterval;
			}
		}
		chain->UpdateHostTime(hostTime);
		chain->BeginMidiBlock(blockStartSample, sampsToRead);
		// A route/mask publication releases only this live owner's contribution.
		// Recorded owners keep their notes and pedal state on the same processor.
		for (auto index = 0u; index < state.MidiDestinations.size(); ++index)
		{
			const auto& destination = state.MidiDestinations[index];
			const auto effective = static_cast<std::uint16_t>(state.LiveDestinationChannels[index] & _allowedMidiChannelMask.load(std::memory_order_acquire));
			const auto removed = static_cast<std::uint16_t>(destination->AppliedLiveChannels & ~effective);
			destination->AppliedLiveChannels = effective;
			if (!removed) continue;
			for (auto channel = 0u; channel < 16u; ++channel)
			{
				if (!(removed & (1u << channel))) continue;
				if (destination->LiveSustain.test(channel))
				{
					destination->LiveSustain.reset(channel);
					if (!destination->RecordedSustainOwners[channel]) destination->PendingSustainRelease.set(channel);
				}
				for (auto note = 0u; note < 128u; ++note)
				{
					const auto slot = midi::MidiNote::NoteSlot(static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(note));
					if (!destination->LiveHeld.test(slot)) continue;
					destination->LiveHeld.reset(slot);
					if (!destination->RecordedAudibleOwners[slot]) destination->PendingReleases.set(slot);
				}
			}
		}
		for (const auto& destination : state.MidiDestinations)
		{
			destination->RecoveringCompensationChange = false;
			const auto epoch = destination->CompensationEpoch.load(std::memory_order_acquire);
			if (epoch != destination->AppliedCompensationEpoch)
			{
				const auto recordedHeld = destination->PluginHeld & ~destination->LiveHeld;
				for (const auto& routes : state.MidiPlaybackRoutes)
					for (const auto& binding : routes)
						if (binding && binding->Destination == destination)
							_DropRecordedAudibleOwner(*destination, *binding);
				const bool affected = destination->ScheduledCount > 0u || recordedHeld.any();
				for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
					if (auto* owner = destination->Scheduled[index].Owner)
						owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
				destination->ScheduledCount = 0u;
				destination->PendingReleases |= recordedHeld;
				destination->ActiveCompensationDelaySamps = destination->CompensationDelaySamps.load(std::memory_order_acquire);
				destination->AppliedCompensationEpoch = epoch;
				destination->RecoveringCompensationChange = affected;
				if (affected)
					for (const auto& routes : state.MidiPlaybackRoutes)
						for (const auto& binding : routes)
							if (binding && binding->Destination == destination && binding->Loop)
								binding->Loop->RequestHeldFlush();
			}
			destination->BlockHeldAtStart = destination->PluginHeld;
			destination->BlockSustainAtStart = destination->PluginSustain;
			destination->BlockTouched.reset();
		}
		_InvalidateScheduledMidiAtBlockStart(state);
		_DrainPendingMidiReleases(state, blockStartSample);
	}

	constexpr auto MaxLiveMidiEventsPerBlock = 64u;
	// Reset releases are staged while quiescent, then delivered after the plugin
	// begins its block. Sending them on the UI thread would be cleared by Begin.
	unsigned int resetReleaseBudget = MaxLiveMidiEventsPerBlock;
	auto resetScanBudget = midi::MidiNote::TotalNoteSlots;
	if (vstActive)
		for (const auto& destination : state.MidiDestinations)
		{
			if (resetScanBudget == 0u || resetReleaseBudget == 0u) break;
			if (destination->PendingLiveReset.none()) continue;
			for (std::size_t slot = 0; slot < midi::MidiNote::TotalNoteSlots && resetReleaseBudget > 0u && resetScanBudget > 0u; ++slot)
			{
				--resetScanBudget;
				if (!destination->PendingLiveReset.test(slot)) continue;
				destination->PendingLiveReset.reset(slot);
				--resetReleaseBudget;
				if (destination->RecordedAudibleOwners[slot] == 0u && !destination->LiveHeld.test(slot)
					&& destination->PluginHeld.test(slot))
					_TryDeliverMidiNow(*destination, MidiEvent::MakeNoteOff(blockStartSample,
						static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
			}
		}
	auto releaseSlotBudget = static_cast<unsigned int>(midi::MidiNote::TotalNoteSlots);
	for (auto dispatched = 0u; dispatched < MaxLiveMidiEventsPerBlock; ++dispatched)
	{
		MidiEvent immediate{};
		SyntheticMidi syntheticEntry{};
		const auto hasImmediate = _immediateLiveMidiIngress.Peek(immediate);
		const auto hasSynthetic = _syntheticLiveMidiIngress.Peek(syntheticEntry);
		const auto& synthetic = syntheticEntry.Event;
		const auto immediatePosition = hasImmediate
			? midi::ClassifyMidiSampleInBlock(immediate.sampleOffset, blockStartSample, sampsToRead)
			: midi::MidiBlockSamplePosition::Future;
		const auto syntheticPosition = hasSynthetic
			? midi::ClassifyMidiSampleInBlock(synthetic.sampleOffset, blockStartSample, sampsToRead)
			: midi::MidiBlockSamplePosition::Future;
		if (immediatePosition == midi::MidiBlockSamplePosition::Future
			&& syntheticPosition == midi::MidiBlockSamplePosition::Future)
			break;

		const auto immediateDelta = midi::MidiSampleDelta(immediate.sampleOffset, blockStartSample);
		const auto syntheticDelta = midi::MidiSampleDelta(synthetic.sampleOffset, blockStartSample);
		bool chooseSynthetic = immediatePosition == midi::MidiBlockSamplePosition::Future;
		if (!chooseSynthetic && syntheticPosition != midi::MidiBlockSamplePosition::Future)
		{
			chooseSynthetic = syntheticDelta < immediateDelta
				|| (syntheticDelta == immediateDelta
					&& (synthetic.IsNoteOff() != immediate.IsNoteOff()
						? synthetic.IsNoteOff()
						: true));
		}

		MidiEvent liveMidi{};
		if (chooseSynthetic)
		{
			if (syntheticEntry.Release)
			{
				auto budget = MaxLiveMidiEventsPerBlock - dispatched;
				if (!_ReleaseLostPlaybackMidi(*syntheticEntry.Release, blockStartSample, budget, releaseSlotBudget)) break;
				dispatched += std::max(1u, MaxLiveMidiEventsPerBlock - dispatched - budget) - 1u;
				_syntheticLiveMidiIngress.Pop(syntheticEntry);
				// Last borrower access: job reclamation follows this acknowledgement.
				syntheticEntry.Release->ReleaseConsumed.store(true, std::memory_order_release);
				continue;
			}
			_syntheticLiveMidiIngress.Pop(syntheticEntry);
			liveMidi = syntheticEntry.Event;
		}
		else
			_immediateLiveMidiIngress.Pop(liveMidi);

		liveMidi.sampleOffset = midi::RebaseMidiSampleForVstBlock(liveMidi.sampleOffset,
			blockStartSample,
			blockStartSample);
		const bool release = liveMidi.IsNoteOff() || (liveMidi.MessageType() == 0xb0u && liveMidi.data1 == 64u && liveMidi.data2 < 64u);
		if (vstActive && (!MembershipRemoving() || release) && (AcceptsLiveMidiChannel(liveMidi.Channel()) || release))
		{
			auto revision = syntheticEntry.LiveNoteOnRevision;
			if (liveMidi.MessageType() == 0xb0u && liveMidi.data1 == 64u && liveMidi.data2 >= 64u)
				revision = _liveSustainRevisions[liveMidi.Channel()].fetch_add(1u, std::memory_order_release) + 1u;
			if (liveMidi.IsNoteOn())
				revision = _liveNoteOnRevisions[midi::MidiNote::NoteSlot(liveMidi.Channel(), liveMidi.data1)].fetch_add(1u, std::memory_order_release) + 1u;
			_DispatchLiveMidi(state, liveMidi, chooseSynthetic && syntheticEntry.SourceLossNoteOff, revision);
		}
	}

	// Removed processors retain their callback-owned release recovery until a
	// processed block accepts it. Their output is discarded before destruction.
	bool allRetiredReleased = !state.RetiredMidiDestinations.empty();
	for (const auto& destination : state.RetiredMidiDestinations)
	{
		if (destination->RetirementReleased.load(std::memory_order_acquire)) continue;
		const auto& plugin = destination->Plugin;
		if (plugin && plugin->IsLoaded() && !plugin->IsBypassed())
		{
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->PendingSustainRelease.test(channel))
					_TryDeliverMidiNow(*destination, {blockStartSample, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, true);
			auto budget = 64u;
			for (auto slot = 0u; slot < midi::MidiNote::TotalNoteSlots && budget; ++slot)
				if (destination->PendingReleases.test(slot))
				{
					--budget;
					_TryDeliverMidiNow(*destination, MidiEvent::MakeNoteOff(blockStartSample,
						static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
				}
			vst::IVstPlugin::PluginCallGuard call(*plugin, true);
			if (call)
			{
				for (auto channel = 0u; channel < destination->RenderChannels; ++channel)
					std::fill_n(state.InstrumentPtrs[channel], sampsToRead, 0.0f);
				plugin->ProcessBlockMulti(state.InstrumentPtrs.data(), static_cast<int>(destination->RenderChannels), sampsToRead);
			}
			if (!plugin->MidiBlockWasProcessed())
			{
				destination->PendingReleases |= destination->PluginHeld | destination->BlockHeldAtStart;
				destination->PendingSustainRelease |= destination->PluginSustain | destination->BlockSustainAtStart;
				allRetiredReleased = false;
				continue;
			}
		}
		// Bypassed/unloaded removed processors are silent and terminal; teardown
		// itself ends their voices, without touching any surviving processor.
		// A latency suspension may later reuse this instance. Preserve its existing
		// guarded reset receipt so bypass cannot resurrect old voices on resume.
		if (plugin && (plugin->IsBypassed() || !plugin->IsLoaded())) plugin->RequestMidiPanic();
		const bool released = !plugin || !plugin->IsLoaded() || plugin->IsBypassed()
			|| (destination->PendingReleases.none() && destination->PendingSustainRelease.none());
		destination->RetirementReleased.store(released, std::memory_order_release);
		allRetiredReleased &= released;
	}
	if (allRetiredReleased) state.RetiredMidiQueueCancelled.store(true, std::memory_order_release);
	if (!vstActive)
	{
		_InvalidateScheduledMidiAtBlockStart(state, true);
		return;
	}

	auto midiOutputIndex = 0u;
	MidiPlaybackSink midiSink(state);
	for (const auto& take : state.OwnedLoopTakes)
	{
		if (!take)
			continue;
		midiOutputIndex += take->ReadMidiBlock(blockStartSample,
			sampsToRead,
			midiSink,
			midiOutputIndex);
	}

	_DrainScheduledMidi(state, blockStartSample, sampsToRead);
	chain->ProcessRack(state.VstBlockPtrs.data(), state.InstrumentPtrs.data(), static_cast<int>(state.VstRenderChannels), sampsToRead);
	_RecoverSkippedMidiBlocks(state);
}

bool Station::_TryEnqueueOrderedLiveMidi(midi::MidiQueue<1024>& queue,
	bool& hasLastSample,
	std::uint32_t& lastSample,
	const midi::MidiEvent& event) noexcept
{
	auto queuedEvent = event;
	if (hasLastSample
		&& midi::MidiSampleDelta(queuedEvent.sampleOffset, lastSample) < 0)
	{
		queuedEvent.sampleOffset = lastSample;
	}

	if (!queue.Push(queuedEvent))
		return false;

	lastSample = queuedEvent.sampleOffset;
	hasLastSample = true;
	return true;
}

void Station::RebuildAutomationDispatch()
{
	// Scene-serialized non-audio ownership. Resolve observers against owning
	// chains before dereferencing; snapshots pin processors, loops and anchors.
	auto snapshot = std::make_shared<AutomationDispatchSnapshot>();
	snapshot->Generation = _nextAutomationDispatchGeneration++;
	const auto stationChain = _vstChain.load(std::memory_order_acquire);

	const auto takes = GetLoopTakeSnapshot();
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

std::shared_ptr<midi::MidiLoop> Station::_LastRecordedMidiLoop(const std::shared_ptr<LoopTake>& take)
{
	std::shared_ptr<midi::MidiLoop> last;
	if (!take)
		return last;

	for (const auto& loop : take->GetMidiLoopSnapshot())
	{
		if (loop && loop->LoopLengthSamps() > 0u)
			last = loop;
	}
	return last;
}

std::int32_t Station::ResolveMidiAnchorCorrectionFor(const midi::MidiLoop* loop) const noexcept
{
	if (!loop)
		return 0;

	const auto takes = GetLoopTakeSnapshot();
	for (const auto& take : takes)
	{
		if (!take)
			continue;
		for (const auto& midiLoop : take->GetMidiLoopSnapshot())
		{
			if (midiLoop.get() == loop)
				return take->MidiAnchorCorrection();
		}
	}
	return 0;
}

std::shared_ptr<midi::MidiLoop> Station::ResolveEditorAutomationLoop(const vst::IVstPlugin* plugin) const
{
	if (!plugin)
		return nullptr;

	const auto takes = GetLoopTakeSnapshot();

	// Take-level ownership: record into the owning take's last recorded loop.
	for (const auto& take : takes)
	{
		if (take && take->OwnsPlugin(plugin))
		{
			if (auto loop = _LastRecordedMidiLoop(take))
				return loop;
		}
	}

	// Station-level ownership: record into the station's last recorded loop
	// (the most recently created MIDI loop across all takes).
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (chain && chain->ContainsPlugin(plugin))
	{
		std::shared_ptr<midi::MidiLoop> last;
		for (const auto& take : takes)
		{
			if (auto loop = _LastRecordedMidiLoop(take))
				last = loop;
		}
		return last;
	}

	return nullptr;
}

void Station::_RunAutomationDispatch(std::uint32_t blockStartSample,
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

void Station::EndMultiPlay(unsigned int numSamps)
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->EndMultiPlay(numSamps);

	for (auto& buffer : state->AudioBuffers)
	{
		buffer->EndWrite(numSamps, true);
		buffer->EndPlay(numSamps);
	}
}

void Station::ApplyFirstMasterRecordingAtAudioBoundary(utils::Timer& clock, std::uint32_t blockStartSample) noexcept
{
	if (MembershipRemoving()) return;
	auto request = _firstMasterRecording.borrow();
	if (!request || request->Clock.get() != &clock
		|| request->Timing.Version <= _firstMasterRecordingConsumed.load(std::memory_order_relaxed)) return;
	auto state = _BorrowAudioState();
	if (!state.ReadSucceeded()) return;
	const auto take = request->Take.get();
	const bool present = state && std::any_of(state->OwnedLoopTakes.begin(), state->OwnedLoopTakes.end(),
		[take](const auto& member) { return member.get() == take; });
	if (take->IsCaptureCancelled() || !take->FirstMasterActivationPending())
	{
		_firstMasterRecordingScene.store(clock.SceneSamplePos(), std::memory_order_relaxed);
		_firstMasterRecordingPhase.store(take->MidiPlayIndex(), std::memory_order_relaxed);
		_firstMasterRecordingStatus.store(0u, std::memory_order_relaxed);
		clock.ConsumeRecordingSeedAtAudioBoundary(request->Timing.Version);
		_firstMasterRecordingConsumed.store(request->Timing.Version, std::memory_order_release);
		return;
	}
	// Activate transport while the audio tail is still recording. Playback checks
	// source readiness per range; full capture completion must not gate it.
	const auto takeState = take->TakeState();
	if (!present || (takeState != LoopTake::STATE_PLAYING
		&& takeState != LoopTake::STATE_PLAYINGRECORDING)) return;
	// Trigger stamps are device samples; Scene/transport are separate rulers.
	// A timestamp may be just ahead of this boundary; preserve that signed age.
	const auto rawOrigin = take->CaptureTiming().TriggerSample.value_or(
		request->Timing.RecordOrigin + static_cast<std::uint32_t>(take->CaptureTiming().DelaySamps));
	const auto elapsed = static_cast<std::int32_t>(blockStartSample - rawOrigin);
	auto phase = static_cast<std::int64_t>(elapsed) % request->Timing.LengthSamps;
	if (phase < 0) phase += request->Timing.LengthSamps;
	if (!take->ActivateFirstMasterMidiAtAudioBoundary(static_cast<unsigned long>(phase),
		rawOrigin)) return;
	const bool installMaster = clock.RecordingSeedReadyAtAudioBoundary(request->Timing.Version)
		&& clock.SeedSourceLength() == 0ul;
	if (installMaster)
	{
		clock.SetQuantisation(request->Timing.GrainSamps, request->Timing.Type);
		clock.SetSeedSourceLength(request->Timing.LengthSamps);
		// This take establishes master zero at its compensated content origin.
		clock.SetMasterLoopSampleOffset(take->MidiPlayIndex());
	}
	_firstMasterRecordingScene.store(clock.SceneSamplePos(), std::memory_order_relaxed);
	_firstMasterRecordingPhase.store(take->MidiPlayIndex(), std::memory_order_relaxed);
	_firstMasterRecordingStatus.store(installMaster ? 1u : 2u, std::memory_order_relaxed);
	clock.ConsumeRecordingSeedAtAudioBoundary(request->Timing.Version);
	_firstMasterRecordingConsumed.store(request->Timing.Version, std::memory_order_release);
}

void Station::BeginCaptureAudioBoundary(std::uint32_t blockStartSample,
    std::uint64_t sceneSample, std::uint64_t absoluteSample,
    std::optional<std::uint32_t> earliestPhysicalSample) noexcept
{
    if (earliestPhysicalSample)
    {
        _captureEarliestPhysicalSample.store(*earliestPhysicalSample, std::memory_order_relaxed);
        _captureHistoryEpochKnown.store(true, std::memory_order_release);
    }
    if (_midiFormatEpochPending.exchange(false, std::memory_order_acq_rel))
    {
        _midiFormatEpochSample.store(blockStartSample, std::memory_order_relaxed);
        _midiFormatEpochKnown.store(true, std::memory_order_release);
    }
    _captureBoundaryRevision.fetch_add(1u, std::memory_order_seq_cst);
    _captureAbsoluteBoundary.store(absoluteSample, std::memory_order_seq_cst);
    _captureDeviceBoundary.store(blockStartSample, std::memory_order_seq_cst);
    _captureBoundaryRevision.fetch_add(1u, std::memory_order_seq_cst);
    auto state = _BorrowAudioState();
    if (!state) return;
    for (const auto& take : state->OwnedLoopTakes)
        if (take) take->BeginCaptureAudioBoundary(blockStartSample, sceneSample, absoluteSample);
}

void Station::ApplyAcceptedTimingCorrection(long long deltaSamps,
	std::uint64_t generation, bool relocateMidi) noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->ApplyAcceptedTimingCorrection(deltaSamps, generation, relocateMidi);
}

void Station::CaptureSceneAnchors(std::uint64_t sceneCoordinateSamps) noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->CaptureSceneAnchors(sceneCoordinateSamps);
}

void Station::InvalidateSceneAnchors() noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->InvalidateSceneAnchors();
}

void Station::ResetTimingEpoch() noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;
	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->ResetTimingEpoch();
}

void Station::CaptureMappedSourceAnchors(std::int64_t sourceCoordinateSamps) noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;
	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->CaptureMappedSourceAnchors(sourceCoordinateSamps);
}

void Station::RestoreMappedSourceCoordinate(std::int64_t sourceCoordinateSamps) noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;
	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->RestoreMappedSourceCoordinate(sourceCoordinateSamps);
}

void Station::SetLocalTransportOffsetSamps(long long targetSamps) noexcept
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->SetLocalTransportOffsetSamps(targetSamps);
}

void Station::OnBlockWriteChannel(unsigned int channel,
	const base::AudioWriteRequest& request,
	int writeOffset)
{
	// Base class routes bus sources to _audioBuffers via _InputChannel
	// (no-op for record sources since _InputChannel returns nullptr)
	MultiAudioSink::OnBlockWriteChannel(channel, request, writeOffset);

	// Fan out record sources to all loop takes
	// (during recording, loop takes may use any/all ADC channels)
	switch (request.source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		{
			auto state = _BorrowAudioState();
			if (!state)
				break;

			for (const auto& take : state->OwnedLoopTakes)
				if (take) take->OnBlockWriteChannel(channel, request, writeOffset);
		}
		break;
	}
}

void Station::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
	{
		if (take)
		{

			take->EndMultiWrite(numSamps, updateIndex, source);
		}
	}
}

void Station::SetSelectDepth(base::SelectDepth depth)
{
	Jammable::SetSelectDepth(depth);
	const bool atStation = (depth == base::SelectDepth::DEPTH_STATION);
	if (_guiRack)
		_guiRack->SetVisible(atStation);
	if (_stationModel)
		_stationModel->SetVisible(atStation);
}

ActionResult Station::OnAction(KeyAction action)
{
	return ActionResult::NoAction();
}

ActionResult Station::OnAction(GuiAction action)
{
	if (MembershipRemoving()) return ActionResult::NoAction();
	const bool isRackAction =
		(action.ElementType == GuiAction::ACTIONELEMENT_RACK) ||
		(action.ElementType == GuiAction::ACTIONELEMENT_ROUTER) ||
		(action.ElementType == GuiAction::ACTIONELEMENT_TOGGLE) ||
		(action.ElementType == GuiAction::ACTIONELEMENT_MIDIQUANTISATION);

	auto res = isRackAction ? ActionResult::NoAction() : GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
		return res;

	switch (action.ElementType)
	{
	case GuiAction::ACTIONELEMENT_MIDIQUANTISATION:
		if (_receiver)
			_receiver->OnAction(action);
		break;
	case GuiAction::ACTIONELEMENT_TOGGLE:
		// Legacy: GuiRack handles toggles internally and does not forward them,
		// so this case is not reached. _router is nullptr and would crash if called.
		if (auto toggleState = std::get_if<GuiAction::GuiInt>(&action.Data))
		{
			auto visible = ((int)GuiToggleParams::TOGGLE_ON) == toggleState->Value;

			if (_router && action.Index == 2)
				_router->SetVisible(visible);
		}

		break;
	case GuiAction::ACTIONELEMENT_ROUTER:
		// Legacy: GuiRack now converts ROUTER→RACK before forwarding,
		// so this case is not reached in normal flows.
		if (auto chans = std::get_if<GuiAction::GuiConnections>(&action.Data))
		{
			for (auto chan = 0u; chan < _audioMixers.size(); chan++)
			{
				std::vector<std::pair<unsigned int, unsigned int>> chanConnections;
				std::copy_if(chans->Connections.begin(), chans->Connections.end(), std::back_inserter(chanConnections),
					[chan](const std::pair<unsigned int, unsigned int>& pair) {
						return pair.first == chan;
					});

				std::vector<unsigned int> secondElements;
				std::transform(chanConnections.begin(), chanConnections.end(), std::back_inserter(secondElements),
					[](const std::pair<unsigned int, unsigned int>& pair) {
						return pair.second;
					});
				_audioMixers[chan]->SetChannels(secondElements);
			}
		}

		break;
	case GuiAction::ACTIONELEMENT_RACK:
		if (auto i = std::get_if<GuiAction::GuiInt>(&action.Data))
		{
			if (action.Index == gui::GuiRack::RackStateNotificationIndex)
			{
				if (i->Value == gui::GuiRackParams::RACK_ROUTER)
					_CollapseOtherTakeRouters();
				else if (i->Value == gui::GuiRackParams::RACK_CHANNELS)
					_CollapseOtherTakeRoutersToChannels();
			}
			else
				SetNumBusChannels(i->Value);
		}
		else if (auto chans = std::get_if<GuiAction::GuiConnections>(&action.Data))
		{
			for (auto chan = 0u; chan < _audioMixers.size(); chan++)
			{
				std::vector<std::pair<unsigned int, unsigned int>> chanConnections;
				std::copy_if(chans->Connections.begin(), chans->Connections.end(), std::back_inserter(chanConnections),
					[chan](const std::pair<unsigned int, unsigned int>& pair) {
						return pair.first == chan;
					});

				std::vector<unsigned int> secondElements;
				std::transform(chanConnections.begin(), chanConnections.end(), std::back_inserter(secondElements),
					[](const std::pair<unsigned int, unsigned int>& pair) {
						return pair.second;
					});
				_audioMixers[chan]->SetChannels(secondElements);
			}
		}
		else if (auto d = std::get_if<GuiAction::GuiDouble>(&action.Data))
		{
			GuiAction mixerAction = action;
			mixerAction.ElementType = GuiAction::ACTIONELEMENT_SLIDER;

			if (0 == action.Index)
				_masterMixer->OnAction(mixerAction);
			else if ((action.Index > 0) && ((action.Index - 1) < _audioMixers.size()))
				_audioMixers[action.Index - 1]->OnAction(mixerAction);
		}

		break;
	}

	return res;
}

ActionResult Station::OnAction(TriggerAction action)
{
	if (MembershipRemoving()) return ActionResult::NoAction();
	// Allow completion and punch actions to finish after a station is hidden or disabled.
	const auto isStart = action.ActionType == TriggerAction::TRIGGER_REC_START ||
		action.ActionType == TriggerAction::TRIGGER_OVERDUB_START;
	if (isStart && (!_isEnabled || !_isVisible))
		return ActionResult::NoAction();

	auto resolveMidiRecordChannels = [this, &action]() {
		std::vector<unsigned int> midiChannels;
		// Station channel permissions do not supply a trigger recording source.
		if (action.MidiInputDevices.empty())
			return midiChannels;
		const auto mask = _allowedMidiChannelMask.load(std::memory_order_acquire);
		for (std::uint8_t channel = 0u; channel < 16u; ++channel)
		{
			const auto bit = static_cast<std::uint16_t>(1u << channel);
			if ((mask & bit) == 0u)
				continue;

			midiChannels.push_back(channel);
		}
		return midiChannels;
	};

	ActionResult res;
	res.IsEaten = false;
	unsigned long completedLength = 0ul;
	long long completedErrorSamps = 0;

	auto loopTake = _TryGetTake(action.TargetId);
	const auto priorAudioLoops = loopTake ? loopTake.value()->GetLoops().size() : 0u;
	const auto priorMidiLoops = loopTake ? loopTake.value()->GetMidiLoops().size() : 0u;
	const auto pendingSeed = _clock ? _clock->PendingRecordingSeedOnOwner() : nullptr;
	// Job decisions use reserved geometry until its callback activation. This
	// avoids treating a second completion as another first master in that gap.
	auto recordingClock = _clock;
	if (pendingSeed)
	{
		recordingClock = std::make_shared<utils::Timer>();
		recordingClock->SetQuantisation(pendingSeed->GrainSamps, pendingSeed->Type);
		recordingClock->SetSeedSourceLength(pendingSeed->LengthSamps);
	}
	const auto pendingElapsed = pendingSeed ? static_cast<std::int32_t>(
		_captureDeviceBoundary.load(std::memory_order_acquire) - pendingSeed->RecordOrigin) : 0;
	const auto transportStart = static_cast<std::int64_t>(pendingSeed ?
		(static_cast<std::uint64_t>((std::max)(0, pendingElapsed))) :
		(_clock ? _clock->AbsoluteSamplePos() : 0ul))
		+ static_cast<std::int64_t>(TransportOffsetSamps());
	const auto transportStartSamps = transportStart < 0 ? 0ull :
		static_cast<std::uint64_t>(transportStart);
    auto resolveCaptureTiming = [&]() -> std::optional<TriggerCaptureTiming> {
        const auto rate = _ResolveSampleRate(action.GetUserConfig(), action.GetAudioParams());
        const auto delay = io::UserConfig::TriggerPreDelaySamples(action.PreDelayMs, rate);
        const auto capacity = io::UserConfig::TriggerHistorySamples(rate);
        if (!delay || !capacity) return std::nullopt;
        TriggerCaptureTiming timing;
        timing.TriggerSample = action.MidiSample.value_or(_captureDeviceBoundary.load(std::memory_order_acquire));
        timing.DelaySamps = *delay;
        if (_midiFormatEpochKnown.load(std::memory_order_acquire))
        {
            bool observed = false;
            for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
            {
                const auto before = _captureBoundaryRevision.load(std::memory_order_seq_cst);
                if (before & 1u) continue;
                const auto device = _captureDeviceBoundary.load(std::memory_order_seq_cst);
                const auto absolute = _captureAbsoluteBoundary.load(std::memory_order_seq_cst);
                const auto after = _captureBoundaryRevision.load(std::memory_order_seq_cst);
                if (before != after) continue;
                const auto transportAbsolute = pendingSeed
                    ? midi::MidiSampleDelta(device, pendingSeed->RecordOrigin)
                    : static_cast<std::int64_t>(absolute);
                timing.ContentTransportOrigin = transportAbsolute
                    - midi::MidiSampleDelta(device, *timing.TriggerSample) - timing.DelaySamps;
                observed = true;
                break;
            }
            if (!observed) return std::nullopt;
        }
        timing.CapacitySamps = *capacity;
        auto inputLatency = action.GetAudioParams() ? action.GetAudioParams()->InputLatency : 0u;
        if (inputLatency == 0u && action.GetUserConfig()) inputLatency = action.GetUserConfig()->Audio.LatencyIn;
        timing.SourceAgeSamps = (std::max)(inputLatency, constants::MaxLoopFadeSamps + *capacity);
        if (timing.SourceAgeSamps > static_cast<unsigned int>((std::numeric_limits<std::int32_t>::max)()))
            return std::nullopt;
        const auto guard = static_cast<std::uint32_t>((static_cast<std::uint64_t>(rate) * 10u + 999u) / 1000u);
        timing.SeamSamps = *delay > 0 ? (std::min)(*capacity, static_cast<unsigned int>(*delay) + guard) : 0u;
        if (!action.InputChannels.empty() && _captureHistoryEpochKnown.load(std::memory_order_acquire) &&
            midi::MidiSampleDelta(*timing.TriggerSample - timing.SeamSamps,
                _captureEarliestPhysicalSample.load(std::memory_order_relaxed)) < 0)
        {
            std::cout << "[Audio] Capture rejected: requested trigger history predates the active stream\n";
            return std::nullopt;
        }
        if (!action.MidiInputDevices.empty() && _midiFormatEpochKnown.load(std::memory_order_acquire) &&
            midi::MidiSampleDelta(*timing.TriggerSample - timing.SeamSamps,
                _midiFormatEpochSample.load(std::memory_order_relaxed)) < 0)
        {
            std::cout << "[MIDI] Capture rejected: requested history predates the active format\n";
            return std::nullopt;
        }
        if (!action.MidiInputDevices.empty() && _midiFormatEpochKnown.load(std::memory_order_acquire) &&
            midi::MidiSampleDelta(_captureDeviceBoundary.load(std::memory_order_acquire), *timing.TriggerSample) > static_cast<std::int64_t>(rate))
        {
            std::cout << "[MIDI] Capture rejected: trigger activation exceeded one-second grace\n";
            return std::nullopt;
        }
        return timing;
    };
	switch (action.ActionType)
	{
	case TriggerAction::TRIGGER_REC_START:
	{
		auto midiInputChannels = resolveMidiRecordChannels();
        const auto resolvedTiming = resolveCaptureTiming();
        if (!resolvedTiming) return ActionResult::NoAction();
        const auto timing = *resolvedTiming;
        const auto earliest = *timing.TriggerSample - timing.SeamSamps;
        std::vector<std::pair<std::string, MidiNoteSnapshot>> heldSnapshot;
        std::vector<RecentPhysicalMidi> replay;
        if (!midiInputChannels.empty())
        {
            std::scoped_lock lock(_liveHeldMidiMutex);
            // Admission includes one second of structural/debounce grace. Missing
            // retained events fail explicitly, never substitute a held attack.
            if (_midiHistoryDroppedThroughSample &&
                midi::MidiSampleDelta(*_midiHistoryDroppedThroughSample, earliest) >= 0)
            {
                std::cout << "[MIDI] Capture rejected: physical history overflow\n";
                return ActionResult::NoAction();
            }
            heldSnapshot = _liveHeldMidi;
            for (auto it = _recentPhysicalMidi.rbegin(); it != _recentPhysicalMidi.rend(); ++it)
            {
                if (midi::MidiSampleDelta(it->Event.sampleOffset, earliest) < 0) continue;
                replay.push_back(*it);
                const auto channel = it->Event.Channel();
                const auto note = it->Event.data1;
                auto restore = [&](const std::string& device, std::uint8_t velocity) {
                    auto found = std::find_if(heldSnapshot.begin(), heldSnapshot.end(),
                        [&](const auto& item) { return item.first == device; });
                    if (found == heldSnapshot.end()) return;
                    if (velocity) found->second.Set(channel, note, velocity);
                    else found->second.Clear(channel, note);
                };
                restore("", it->PriorAnyVelocity);
                if (!it->Device.empty()) restore(it->Device, it->PriorDeviceVelocity);
            }
        }
        std::stable_sort(replay.begin(), replay.end(), [earliest](const auto& a, const auto& b) {
            const auto left = midi::MidiSampleDelta(a.Event.sampleOffset, earliest);
            const auto right = midi::MidiSampleDelta(b.Event.sampleOffset, earliest);
            if (left != right) return left < right;
            if (a.Device != b.Device) return a.Device < b.Device;
            return a.Sequence < b.Sequence;
        });
        auto newLoopTake = AddTake();
        newLoopTake->ConfigureTriggerCapture(timing);
        newLoopTake->Record(action.InputChannels, Name(), midiInputChannels,
            action.MidiInputDevices, std::move(heldSnapshot), transportStartSamps, timing.TriggerSample);
        for (const auto& entry : replay)
            newLoopTake->RecordMidiEvent(entry.Event, entry.Device, *timing.TriggerSample);
        if (!midiInputChannels.empty()) _midiCaptureTargets.push_back(newLoopTake);
		if (pendingSeed && !IsRemote())
			newLoopTake->SetMidiBaseGrid(static_cast<std::uint32_t>(pendingSeed->LengthSamps),
				static_cast<std::uint32_t>(pendingSeed->LengthSamps / pendingSeed->GrainSamps));
		res.SourceId = "";
		res.TargetId = newLoopTake->Id();
		res.TriggerTargetTake = newLoopTake;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		_SetTakePresentation(*newLoopTake, LoopTake::PresentationMode::Record);
		newLoopTake->PublishCameraLayerStart();
		break;
	}
	case TriggerAction::TRIGGER_REC_END:
	{
		auto loopLength = action.SampleCount;
		std::optional<std::uint32_t> firstMasterStopSample;
		const bool clockWasSeeded = recordingClock && recordingClock->SeedSourceLength() > 0ul;
		// Every first local master uses trigger edges, excluding START handoff delay.
		// Later takes retain their existing callback/counting duration policy.
		if (!clockWasSeeded && !IsRemote() && loopTake
			&& _clock
			&& loopTake.value()->MidiRecordStartSample() && action.MidiSample)
		{
			const auto triggerElapsed = midi::MidiSampleDelta(
				*action.MidiSample, *loopTake.value()->MidiRecordStartSample());
			if (triggerElapsed > 0)
			{
				loopLength = static_cast<unsigned long>(triggerElapsed);
				firstMasterStopSample = action.MidiSample;
			}
		}

		const auto requestedLength = loopLength;
		if (0 == loopLength)
		{
			if (loopTake.has_value())
				_DitchLoopTake(loopTake.value());
			res.DitchResult = loopTake ? actions::DitchDisposition::Removed :
				actions::DitchDisposition::AlreadyAbsent;

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_DITCH;
		}
		else
		{
			auto errorSamps = 0;
			std::shared_ptr<const utils::Timer::RecordingSeed> firstSeed;
			std::optional<std::uint32_t> midiInitialPhase;
			const auto clockBefore = _clock ? _clock->ObserveTransport() : utils::Timer::TransportObservation{};
			const auto grainBefore = _clock ? _clock->QuantiseSamps() : 0u;
			auto cfg = action.GetUserConfig();
			auto streamParams = action.GetAudioParams();
			if (_clock)
			{
				if (clockWasSeeded)
				{
					auto [quantisedLength, err] = recordingClock->QuantiseLength(action.SampleCount);
					loopLength = quantisedLength;
					errorSamps = err;
				}
				else if (firstMasterStopSample && loopTake)
				{
					// Infer off callback without touching the live Timer. Publish the
					// complete geometry and trigger origin after take completion.
					auto previewClock = std::make_shared<utils::Timer>();
					_TrySeedClockFromFirstLoop(previewClock, loopLength, cfg, streamParams, loopTake.value()->Id());
					loopLength = previewClock->SeedSourceLength();
                    if (!loopTake.value()->AdmitTriggerCaptureLength(loopLength))
                    {
                        _DitchLoopTake(loopTake.value());
                        return ActionResult::NoAction();
                    }
					firstSeed = _clock->ReserveRecordingSeedOnOwner({ 0u, previewClock->QuantiseSamps(),
						loopLength, previewClock->Quantisation(), *loopTake.value()->MidiRecordStartSample()
                            - static_cast<std::uint32_t>(loopTake.value()->CaptureTiming().DelaySamps) });
					if (firstSeed && !IsRemote())
						loopTake.value()->SetMidiBaseGrid(static_cast<std::uint32_t>(loopLength),
							static_cast<std::uint32_t>(loopLength / firstSeed->GrainSamps));
					errorSamps = static_cast<int>(static_cast<std::int64_t>(requestedLength) - loopLength);
					if (loopLength > 0ul)
					{
						const auto elapsed = static_cast<std::int32_t>(static_cast<std::uint32_t>(
							_captureDeviceBoundary.load(std::memory_order_acquire)) - *loopTake.value()->MidiRecordStartSample());
						auto phase = static_cast<std::int64_t>(elapsed) % loopLength;
						if (phase < 0) phase += loopLength;
						midiInitialPhase = static_cast<std::uint32_t>(phase);
					}
				}
				else
				{
                    auto previewClock = std::make_shared<utils::Timer>();
                    _TrySeedClockFromFirstLoop(previewClock, loopLength, cfg, streamParams,
                        loopTake.has_value() ? loopTake.value()->Id() : "none");
                    loopLength = previewClock->SeedSourceLength();
                    if (loopTake && !loopTake.value()->AdmitTriggerCaptureLength(loopLength))
                    {
                        _DitchLoopTake(loopTake.value());
                        return ActionResult::NoAction();
                    }
                    if (loopTake)
                    {
                        const auto origin = loopTake.value()->CaptureTiming().TriggerSample.value_or(
                            _captureDeviceBoundary.load(std::memory_order_acquire));
                        firstSeed = _clock->ReserveRecordingSeedOnOwner({ 0u, previewClock->QuantiseSamps(),
                            loopLength, previewClock->Quantisation(), origin
                                - static_cast<std::uint32_t>(loopTake.value()->CaptureTiming().DelaySamps) });
                        if (firstSeed && !IsRemote())
                            loopTake.value()->SetMidiBaseGrid(static_cast<std::uint32_t>(loopLength),
                                static_cast<std::uint32_t>(loopLength / firstSeed->GrainSamps));
                    }
                    else
                    {
                        _clock->SetQuantisation(previewClock->QuantiseSamps(), previewClock->Quantisation());
                        _clock->SetSeedSourceLength(loopLength);
                    }
				}
			}
			auto playPos = cfg.has_value() ?
				cfg.value().OverdubPlayPos(errorSamps, loopLength) :
				0;
			auto endRecordSamps = cfg.has_value() ?
				cfg.value().EndRecordingSamps(errorSamps) :
				0;

            if (loopTake && !loopTake.value()->AdmitTriggerCaptureLength(loopLength))
            {
                _DitchLoopTake(loopTake.value());
                return ActionResult::NoAction();
            }
			if (loopTake.has_value())
			{
                loopTake.value()->SetTriggerCaptureStop(action.MidiSample.value_or(
                    _captureDeviceBoundary.load(std::memory_order_acquire)));
                const auto midiBefore = loopTake.value()->MidiPlayIndex();
				const auto midiPlayErrorSamps = midiInitialPhase ? static_cast<int>(*midiInitialPhase) : errorSamps;
				loopTake.value()->Play(playPos, loopLength, endRecordSamps, midiPlayErrorSamps, static_cast<bool>(firstSeed));
				if (firstSeed)
					_firstMasterRecording.store(std::make_shared<const FirstMasterRecording>(
						FirstMasterRecording{ *firstSeed, loopTake.value(), _clock }));
				if (_loggingConfig.Event == "verbose")
				{
					const auto clockAfter = _clock ? _clock->ObserveTransport() : utils::Timer::TransportObservation{};
					const auto grid = loopTake.value()->ResolvedMidiQuantisation();
					std::cout << "[NINJAM][RecordEnd] station=\"" << Name() << "\" take=" << loopTake.value()->Id()
						<< " requested=" << action.SampleCount << " length=" << loopLength
						<< " recorded=" << loopTake.value()->NumRecordedSamps()
						<< " clockBefore=" << clockBefore.MasterPhaseSamps << '/' << clockBefore.MasterLengthSamps
						<< " clockAfter=" << clockAfter.MasterPhaseSamps << '/' << clockAfter.MasterLengthSamps
						<< " grainBefore=" << grainBefore << " grainAfter=" << (_clock ? _clock->QuantiseSamps() : 0u)
						<< " scene=" << clockAfter.SceneSamplePos << " midiBefore=" << midiBefore
						<< " midiAfter=" << loopTake.value()->MidiPlayIndex() << " midiError=" << midiPlayErrorSamps
						<< " endTail=" << endRecordSamps
						<< " gridEnabled=" << grid.Enabled << " gridInterval=" << grid.GridInterval()
						<< " gridDivisions=" << grid.GridDivisions() << " remoteGrid=" << grid.HasRemoteGrid()
						<< " gridOrigin=" << grid.RemoteOriginSamps
						<< " captureTransportStart=" << loopTake.value()->MidiQuantisationTransportStartSamps()
						<< " gridOffset=" << grid.PhaseOffsetSamps << '\n';
				}
			}
			completedLength = loopLength;
			completedErrorSamps = static_cast<long long>(requestedLength) - static_cast<long long>(loopLength);

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;

		}
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_START:
	{
		auto midiInputChannels = resolveMidiRecordChannels();
		auto sourceLoopTake = _loopTakes.empty() ? std::shared_ptr<LoopTake>() : _loopTakes.back();
        auto sourceId = sourceLoopTake ? sourceLoopTake->Id() : "";
        if (sourceLoopTake && !midiInputChannels.empty() && !sourceLoopTake->GetMidiLoops().empty()
            && !sourceLoopTake->HasMidiDevicePhaseObservation())
        {
            std::cout << "[MIDI] Overdub rejected: source has no coherent device phase observation\n";
            return ActionResult::NoAction();
        }

        auto newLoopTake = AddTake();
        const auto timing = resolveCaptureTiming();
        if (!timing) return ActionResult::NoAction();
        newLoopTake->ConfigureTriggerCapture(*timing);
		newLoopTake->Overdub(action.InputChannels,
			Name(),
			midiInputChannels,
			action.MidiInputDevices,
			sourceLoopTake,
			transportStartSamps);
		newLoopTake->SetActiveBounce(sourceLoopTake, action.OverdubWriter);
        if (!midiInputChannels.empty()) _midiCaptureTargets.push_back(newLoopTake);

		res.SourceId = sourceId;
		res.TargetId = newLoopTake->Id();
		res.TriggerSourceTake = sourceLoopTake;
		res.TriggerTargetTake = newLoopTake;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		_SetTakePresentation(*newLoopTake, LoopTake::PresentationMode::Overdub);
		newLoopTake->PublishCameraLayerStart();
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_END:
	{
		auto loopLength = action.SampleCount;
		const auto requestedLength = loopLength;

		if (0 == loopLength)
		{
			if (loopTake.has_value())
				_DitchLoopTake(loopTake.value());
			res.DitchResult = loopTake ? actions::DitchDisposition::Removed :
				actions::DitchDisposition::AlreadyAbsent;

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_DITCH;
		}
		else
		{
			auto errorSamps = 0;
			auto cfg = action.GetUserConfig();
			auto streamParams = action.GetAudioParams();

			if (_clock)
			{
				if (recordingClock->IsQuantisable())
				{
					auto [quantisedLength, err] = recordingClock->QuantiseLength(action.SampleCount);
					loopLength = quantisedLength;
					errorSamps = err;
				}
				else
				{
                    auto previewClock = std::make_shared<utils::Timer>();
                    _TrySeedClockFromFirstLoop(previewClock, action.SampleCount, cfg, streamParams,
                        loopTake.has_value() ? loopTake.value()->Id() : "none");
                    loopLength = previewClock->SeedSourceLength();
                    if (loopTake && !loopTake.value()->AdmitTriggerCaptureLength(loopLength))
                    { _DitchLoopTake(loopTake.value()); return ActionResult::NoAction(); }
                    _clock->SetQuantisation(previewClock->QuantiseSamps(), previewClock->Quantisation());
                    _clock->SetSeedSourceLength(loopLength);
				}
			}
			auto outLatency = streamParams.has_value() ?
				streamParams.value().OutputLatency :
				0u;

			if (0u == outLatency)
			{
				outLatency = cfg.has_value() ?
					cfg.value().Audio.LatencyOut :
					0u;
			}

			auto playPos = cfg.has_value() ?
				// Keep the capture cursor in hardware time. Recorded playback plugin
				// latency is compensated at rendering, not in this source position.
				cfg.value().LoopPlayPos(errorSamps, loopLength, outLatency) :
				0;
			auto endRecordSamps = cfg.has_value() ?
				cfg.value().EndRecordingSamps(errorSamps) :
				0;

			if (loopTake.has_value())
			{
                if (!loopTake.value()->AdmitTriggerCaptureLength(loopLength))
                { _DitchLoopTake(loopTake.value()); return ActionResult::NoAction(); }
                loopTake.value()->SetTriggerCaptureStop(action.MidiSample.value_or(_captureDeviceBoundary.load(std::memory_order_acquire)));
                loopTake.value()->Play(playPos, loopLength, endRecordSamps, errorSamps, false, outLatency);
			}
			completedLength = loopLength;
			completedErrorSamps = static_cast<long long>(requestedLength) - static_cast<long long>(loopLength);

			auto sourceLoopTake = _TryGetTake(action.SourceId);
			if (sourceLoopTake.has_value() && loopTake.has_value())
				loopTake.value()->AcquireReplacementSourceMute(sourceLoopTake.value());

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		}
		break;
	}
	case TriggerAction::TRIGGER_PUNCHIN_START:
		if (action.ApplyToTargetTake && action.ApplyToTargetMidi && loopTake.has_value())
		{
			for (const auto& event : loopTake.value()->BuildMidiPunchInLiveTransitionEvents(static_cast<std::uint32_t>(action.SampleCount)))
				TryEnqueueSyntheticLiveMidi(event);
		}

		if (action.ApplyToTargetTake && loopTake.has_value())
			loopTake.value()->PunchIn(action.ApplyToTargetAudio, action.ApplyToTargetMidi, action.MidiSample);

		if (action.ApplyToSourceTake)
		{
			if (auto sourceLoopTake = _TryGetTake(action.SourceId); sourceLoopTake.has_value())
				sourceLoopTake.value()->Mute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		if (loopTake && loopTake.value()->GetPresentation().Mode == LoopTake::PresentationMode::Overdub)
			_SetTakePresentation(*loopTake.value(), LoopTake::PresentationMode::Punch);
		break;
	case TriggerAction::TRIGGER_PUNCHIN_END:
		if (action.ApplyToTargetTake && action.ApplyToTargetMidi && loopTake.has_value())
		{
			for (const auto& event : loopTake.value()->BuildMidiPunchOutLiveTransitionEvents(static_cast<std::uint32_t>(action.SampleCount)))
				TryEnqueueSyntheticLiveMidi(event);
		}

		if (action.ApplyToTargetTake && loopTake.has_value())
			loopTake.value()->PunchOut(action.ApplyToTargetAudio, action.ApplyToTargetMidi, action.MidiSample);

		if (action.ApplyToSourceTake)
		{
			if (auto sourceLoopTake = _TryGetTake(action.SourceId); sourceLoopTake.has_value())
				sourceLoopTake.value()->UnMute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		if (loopTake && loopTake.value()->GetPresentation().Mode == LoopTake::PresentationMode::Punch)
			_SetTakePresentation(*loopTake.value(), LoopTake::PresentationMode::Overdub);
		break;
	case TriggerAction::TRIGGER_DITCH:
		if (loopTake.has_value())
		{
			_DitchLoopTake(loopTake.value(), !action.IsSourceLossCancellation);
			res.DitchResult = actions::DitchDisposition::Removed;
		}
		else
			res.DitchResult = actions::DitchDisposition::AlreadyAbsent;

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DITCH;
		break;
	case TriggerAction::TRIGGER_DITCH_UNMUTE:
		if (loopTake.has_value())
		{
			loopTake.value()->UnMute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		break;
	}
	// Applied structural outcome: publish on the serialized owner, never audio.
	if (res.IsEaten && res.ResultType == actions::ActionResultType::ACTIONRESULT_DITCH
		&& !action.IsSourceLossCancellation)
		if (auto publisher = _cameraRecordingPublisher.lock())
			publisher->ObserveDitch(std::dynamic_pointer_cast<Station>(Sharable::shared_from_this()), utils::Timer::GetTime());
	if (res.IsEaten && loopTake)
		loopTake.value()->PublishCameraCaptureObservation();
	if (res.IsEaten)
	{
		const char* actionLabel = nullptr;
		switch (action.ActionType)
		{
		case TriggerAction::TRIGGER_REC_START: actionLabel = "record-start"; break;
		case TriggerAction::TRIGGER_REC_END: actionLabel = "record-end"; break;
		case TriggerAction::TRIGGER_OVERDUB_START: actionLabel = "overdub-start"; break;
		case TriggerAction::TRIGGER_OVERDUB_END: actionLabel = "overdub-end"; break;
		case TriggerAction::TRIGGER_PUNCHIN_START: actionLabel = "punch-in"; break;
		case TriggerAction::TRIGGER_PUNCHIN_END: actionLabel = "punch-out"; break;
		case TriggerAction::TRIGGER_DITCH: actionLabel = "ditch"; break;
		case TriggerAction::TRIGGER_OVERDUB_DITCH: actionLabel = "overdub-ditch"; break;
		case TriggerAction::TRIGGER_DITCH_UNMUTE: break; // Internal restoration action; intentionally not logged.
		default: break;
		}
		if (actionLabel)
		{
			auto target = loopTake ? *loopTake : std::shared_ptr<LoopTake>();
			if (!target && !res.TargetId.empty())
			{
				if (auto created = _TryGetTake(res.TargetId); created)
					target = *created;
			}
			const auto state = GetVisualState();
			const char* stateLabel = "idle";
			switch (state)
			{
			case StationVisualState::STATIONSTATE_RECORDING: stateLabel = "recording"; break;
			case StationVisualState::STATIONSTATE_ENDRECORDING: stateLabel = "ending-recording"; break;
			case StationVisualState::STATIONSTATE_PLAYING: stateLabel = "playing"; break;
			case StationVisualState::STATIONSTATE_OVERDUBBING: stateLabel = "overdubbing"; break;
			case StationVisualState::STATIONSTATE_PUNCHIN: stateLabel = "punch-in"; break;
			default: break;
			}
			const bool isPlay = completedLength > 0ul && loopTake.has_value();
			std::cout << (isPlay ? "[Loop Play] station=\"" : "[Station] station=\"")
				<< Name() << "\" action=" << actionLabel
				<< " state=" << stateLabel
				<< " take=" << (target ? target->Id() : action.TargetId)
				<< " length=" << completedLength;
			if (isPlay)
			{
				const auto grainSamps = _clock ? _clock->QuantiseSamps() : 0u;
				std::cout << " errorSamps=" << completedErrorSamps
					<< " grainSamps=" << grainSamps;
				if (grainSamps > 0u && completedLength % grainSamps == 0ul)
					std::cout << " grains=" << completedLength / grainSamps;
				if (_clock)
				{
					const auto bpm = _clock->CurrentMusicalPosition(
						_ResolveSampleRate(action.GetUserConfig(), action.GetAudioParams()));
					if (bpm.IsValid)
						std::cout << " bpm=" << bpm.Tempo;
				}
			}
			if (target)
			{
				const bool ditched = res.ResultType == actions::ACTIONRESULT_DITCH;
				std::cout << " audioLoops=" << (ditched ? priorAudioLoops : target->GetLoops().size())
					<< " midiLoops=" << (ditched ? priorMidiLoops : target->GetMidiLoops().size());
			}
			if (!res.SourceId.empty() || !action.SourceId.empty())
				std::cout << " source=" << (res.SourceId.empty() ? action.SourceId : res.SourceId);
			std::cout << '\n';
		}
	}

	return res;
}

StationVisualState Station::GetVisualState() const noexcept
{
	// Only job/UI callers acquire weak takes. The callback neither walks mutable
	// membership nor owns a display cache; retired audio states retain its borrowers.
	const auto snapshot = _LoopTakeSnapshotState();
	StationVisualState visual = StationVisualState::STATIONSTATE_DEFAULT;
	std::uint64_t newestSerial = 0u;
	bool hasActive = false;
	bool hasTail = false;
	if (snapshot)
	{
		for (const auto& weakTake : *snapshot)
		{
			const auto take = weakTake.lock();
			if (!take)
				continue;
			const auto presentation = take->GetPresentation();
			if (presentation.Mode == LoopTake::PresentationMode::Tail)
			{
				hasTail = true;
				continue;
			}
			StationVisualState mode;
			switch (presentation.Mode)
			{
			case LoopTake::PresentationMode::Record: mode = StationVisualState::STATIONSTATE_RECORDING; break;
			case LoopTake::PresentationMode::Overdub: mode = StationVisualState::STATIONSTATE_OVERDUBBING; break;
			case LoopTake::PresentationMode::Punch: mode = StationVisualState::STATIONSTATE_PUNCHIN; break;
			default: continue;
			}
			if (!hasActive || presentation.Serial > newestSerial)
			{
				visual = mode;
				newestSerial = presentation.Serial;
				hasActive = true;
			}
		}
	}
	return hasActive ? visual : hasTail ? StationVisualState::STATIONSTATE_ENDRECORDING
		: StationVisualState::STATIONSTATE_DEFAULT;
}

void Station::_SetTakePresentation(LoopTake& take, LoopTake::PresentationMode mode) noexcept
{
	// A repeated punch command is a no-op and must not steal display recency.
	if (take.GetPresentation().Mode != mode)
		take.SetPresentation(mode, _nextPresentationSerial++);
	else if (take.GetPresentation().Serial == 0u)
		take.SetPresentation(mode, _nextPresentationSerial++);
}

void Station::OnTick(Time curTime,
	unsigned int samps,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
}

void Station::Reset()
{
	FlushLiveHeldMidiNotes();
	for (const auto& take : GetLoopTakeSnapshot())
	{
		take->CancelCapture();
		_QueueLostPlaybackMidi(take, false);
		take->DetachGuiOwnershipForRetirement();
	}
	Jammable::Reset();
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		_liveHeldMidi.clear();
		for (const auto& pedal : _liveSustainMidi) _pendingLostSustain |= pedal.second;
		_liveSustainMidi.clear();
        _recentPhysicalMidi.clear();
        _midiHistoryDroppedThroughSample.reset();
		_anonymousLiveHeldMidi = {};
		_pendingLostLiveMidiNoteOffs.reset();
	}
	if (_stationModel)
		_stationModel->ResetStationLevel();

	for (auto& take : _loopTakes)
	{
		auto child = std::find(_children.begin(), _children.end(), take);
		if (_children.end() != child)
			_children.erase(child);
	}
	_loopTakes.clear();
	_backLoopTakes.clear();
	// Commit the cleared take list to the audio snapshot as well.
	_flipTakeBuffer = true;
	_changesMade = true;
	_PublishLoopTakeSnapshot();
	_loopTakeRevision.fetch_add(1u, std::memory_order_release);

}

std::shared_ptr<LoopTake> Station::AddTake()
{
	LoopTakeParams takeParams;
	takeParams.Id = Name() + "-TK-" + utils::GetGuid();
	takeParams.Size = { 100,100 };
	takeParams.FadeSamps = _fadeSamps;

	MergeMixBehaviourParams mergeParams;
	auto mixerParams = LoopTake::GetMixerParams(takeParams.Size, mergeParams);
	auto take = std::make_shared<LoopTake>(takeParams, mixerParams);
	AddTake(take);

	return take;
}

void Station::SetRemoteMidiQuantisationGrid(const RemoteTransportGeometry& geometry,
	std::int64_t originSamps, bool resetBaseGrid)
{
	_remoteMidiGrid = geometry;
	_remoteMidiGridOriginSamps = originSamps;
	const auto apply = [&](const auto& takes) {
		for (const auto& take : takes)
			if (take)
			{
				take->SetRemoteMidiQuantisationGrid(geometry, originSamps);
				if (resetBaseGrid) take->SetMidiBaseGrid(0u, 0u);
			}
	};
	apply(_loopTakes);
	apply(_backLoopTakes);
}

void Station::SetCameraRecordingPublisher(const std::shared_ptr<CameraRecordingPublisher>& publisher)
{
	_cameraRecordingPublisher = publisher;
	for (const auto& take : _loopTakes) take->SetCameraRecordingPublisher(publisher);
	for (const auto& take : _backLoopTakes) take->SetCameraRecordingPublisher(publisher);
}

void Station::AddTake(std::shared_ptr<LoopTake> take)
{
	const std::weak_ptr<Station> weakStation = std::dynamic_pointer_cast<Station>(ActionReceiver::shared_from_this());
	take->SetAutomationTopologyChangedCallback([weakStation] {
		if (const auto station = weakStation.lock())
		{
			station->_automationDispatchDirty = true;
			station->_changesMade = true;
		}
	});
	take->SetCameraRecordingPublisher(_cameraRecordingPublisher);
	take->SetupBuffers(_lastBufSize);
	take->SetNumBusChannels(NumBusChannels());
	take->SetSelectDepth(CurrentSelectDepth());
	take->SetLogging(_loggingConfig);
	take->SetReceiver(ActionReceiver::shared_from_this());
	take->SetGlobalMidiQuantState(_globalMidiQuantState);
	take->SetRemoteMidiQuantisationGrid(_remoteMidiGrid, _remoteMidiGridOriginSamps);
	take->SetInitialLocalTransportOffsetSamps(TransportOffsetSamps());
	_backLoopTakes.push_back(take);
	_ApplyMidiQuantisationPhaseOffset();
	_ArrangeChildren();
	_flipTakeBuffer = true;
	_changesMade = true;
	_PublishLoopTakeSnapshot();
	_loopTakeRevision.fetch_add(1u, std::memory_order_release);
}

bool Station::DitchTake(const std::shared_ptr<LoopTake>& take) noexcept
{
	if (!take) return false;
	const auto found = std::find(_backLoopTakes.begin(), _backLoopTakes.end(), take);
	if (found == _backLoopTakes.end()) return false;
	auto target = *found;
	_DitchLoopTake(target);
	return true;
}

std::vector<std::shared_ptr<LoopTake>> Station::GetLoopTakeSnapshot() const
{
	std::vector<std::shared_ptr<LoopTake>> takes;
	auto snapshot = _LoopTakeSnapshotState();
	if (!snapshot)
		return takes;

	takes.reserve(snapshot->size());
	for (const auto& weakTake : *snapshot)
	{
		if (auto take = weakTake.lock())
			takes.push_back(std::move(take));
	}

	return takes;
}

unsigned int Station::NumTakes() const
{
	return _HasUncommittedChanges() ?
		(unsigned int)_backLoopTakes.size() :
		(unsigned int)_loopTakes.size();
}

std::string Station::Name() const
{
	return *_name.load(std::memory_order_acquire);
}

void Station::SetName(std::string name)
{
	_name.store(std::make_shared<const std::string>(std::move(name)), std::memory_order_release);
}

void Station::SetClock(std::shared_ptr<utils::Timer> clock)
{
	_clock = clock;
}

void Station::SetQuantisationParams(std::optional<engine::QuantisationParams> params,
	bool confirm)
{
	if (!_quantisationModel)
		return;

	if (!params.has_value())
	{
		_pendingQuantisationParams = std::nullopt;
		_pendingQuantisationConfirm = false;
		return;
	}

	_pendingQuantisationParams = engine::QuantisationParams{
		params->SeedSamps,
		params->MasterSamps
	};
	_pendingQuantisationConfirm = _pendingQuantisationConfirm || confirm;
}

void Station::ClearQuantisationParams()
{
	SetQuantisationParams(std::nullopt);
}

void Station::SetQuantisationOverlayAlpha(float alpha) noexcept
{
	_quantisationOverlayAlpha = std::clamp(alpha, 0.0f, 1.0f);
	if (_quantisationModel)
		_quantisationModel->SetOverlayAlpha(_quantisationOverlayAlpha);
	if (_quantisationDivisionModel)
		_quantisationDivisionModel->SetOverlayAlpha(_quantisationOverlayAlpha);
}

void Station::SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state) noexcept
{
	if (_globalMidiQuantState == state)
		return;

	_globalMidiQuantState = state;
	for (auto& take : _loopTakes)
	{
		if (take)
			take->SetGlobalMidiQuantState(state);
	}
	for (auto& take : _backLoopTakes)
	{
		if (take)
			take->SetGlobalMidiQuantState(state);
	}
}

void Station::SetTransportOffsetLoopFrac(double loopFrac) noexcept
{
	const auto signedLoopFrac = std::isfinite(loopFrac) ?
		std::clamp(loopFrac, -1.0, 1.0) : 0.0;
	_transportOffsetLoopFrac.store(signedLoopFrac,
		std::memory_order_release);
}

std::int32_t Station::TransportOffsetSamps() const noexcept
{
	const auto loopFrac = _transportOffsetLoopFrac.load(std::memory_order_acquire);
	if (std::abs(loopFrac) < 1.0e-9)
		return 0;

	const auto masterLoopSamps = _clock ? _clock->SeedSourceLength() : 0ul;
	if (masterLoopSamps == 0ul)
		return 0;

	auto offset = static_cast<std::int64_t>(std::llround(loopFrac * static_cast<double>(masterLoopSamps)));
	if (offset > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
		offset = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
	else if (offset < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()))
		offset = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());

	return static_cast<std::int32_t>(offset);
}

void Station::SetGlobalPhaseOffsetSamps(std::int32_t offsetSamps) noexcept
{
	if (_globalPhaseOffsetSamps == offsetSamps)
		return;

	_globalPhaseOffsetSamps = offsetSamps;
	_ApplyMidiQuantisationPhaseOffset();
}

void Station::SetStationPhaseOffsetSamps(std::int32_t offsetSamps) noexcept
{
	if (_stationPhaseOffsetSamps == offsetSamps)
		return;

	_stationPhaseOffsetSamps = offsetSamps;
	_ApplyMidiQuantisationPhaseOffset();
}

void Station::_ApplyMidiQuantisationPhaseOffset() noexcept
{
	auto combined = static_cast<std::int64_t>(_globalPhaseOffsetSamps)
		+ static_cast<std::int64_t>(_stationPhaseOffsetSamps);
	if (combined > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
		combined = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
	else if (combined < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()))
		combined = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());

	const auto inherited = static_cast<std::int32_t>(combined);
	for (auto& take : _loopTakes)
	{
		if (take)
			take->SetMidiQuantisationInheritedPhaseOffset(inherited);
	}
	for (auto& take : _backLoopTakes)
	{
		if (take)
			take->SetMidiQuantisationInheritedPhaseOffset(inherited);
	}

	// Quantisation overlay state is refreshed on the render thread, so keep this
	// path off the audio callback and only mutate the GUI model from the main/render path.
	RefreshQuantisationOverlayFromClock();
}

void Station::RefreshQuantisationOverlayFromClock()
{
	if (!_quantisationModel && !_quantisationDivisionModel)
		return;

	if (!_clock || !_clock->IsQuantisable() || (_quantisationOverlayAlpha <= 0.001f))
	{
		_pendingQuantisationParams = std::nullopt;
		_pendingQuantisationConfirm = false;
		if (_quantisationModel)
		{
			_quantisationModel->SetTiming(_HiddenSeedSamps);
			_quantisationModel->SetOverlayVisible(false, false);
		}
		if (_quantisationDivisionModel)
			_quantisationDivisionModel->SetOverlayVisible(false, false);
		return;
	}

	const auto visuals = LoopTake::QuantisationVisualsFor(GetLoopTakes());

	if (visuals.empty())
	{
		_pendingQuantisationParams = std::nullopt;
		_pendingQuantisationConfirm = false;
		if (_quantisationModel)
		{
			_quantisationModel->SetTiming(_HiddenSeedSamps);
			_quantisationModel->SetOverlayVisible(false, false);
		}
		if (_quantisationDivisionModel)
			_quantisationDivisionModel->SetOverlayVisible(false, false);
		return;
	}

	if (_pendingQuantisationParams.has_value())
	{
		const auto seedSamps = _pendingQuantisationParams->SeedSamps > 0u ?
			_pendingQuantisationParams->SeedSamps :
				_HiddenSeedSamps;

		if (_quantisationModel)
		{
			_quantisationModel->SetLoopTakeVisuals(seedSamps, visuals);
			_quantisationModel->SetOverlayVisible(true, _pendingQuantisationConfirm);
		}
		if (_quantisationDivisionModel)
		{
			_quantisationDivisionModel->SetLoopTakeVisuals(visuals);
			_quantisationDivisionModel->SetOverlayVisible(true, _pendingQuantisationConfirm);
		}
		_pendingQuantisationConfirm = false;
		return;
	}

	const auto seedSamps = _clock->QuantiseSamps();
	if (_quantisationModel)
	{
		_quantisationModel->SetLoopTakeVisuals(seedSamps, visuals);
		_quantisationModel->SetOverlayVisible(true, false);
	}
	if (_quantisationDivisionModel)
	{
		_quantisationDivisionModel->SetLoopTakeVisuals(visuals);
		_quantisationDivisionModel->SetOverlayVisible(true, false);
	}
}

void Station::SetupBuffers(unsigned int bufSize)
{
	_lastBufSize = bufSize;
	_blockSize = bufSize;

	auto& buffers = (_flipAudioBuffer && _HasUncommittedChanges()) ?
		_backAudioBuffers :
		_audioBuffers;

	for (auto& buf : buffers)
	{
		buf->SetSize(bufSize);
		buf->Reset();
	}

	for (auto& take : _loopTakes)
		take->SetupBuffers(bufSize);
	for (auto& take : _backLoopTakes)
		take->SetupBuffers(bufSize);
}

bool Station::ReconfigureVstFormat(float sampleRate, unsigned int blockSize)
{
	const auto chain = _vstChain.load(std::memory_order_acquire);
	if (chain && !chain->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
	if (_backVstChain && _backVstChain != chain && !_backVstChain->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
	for (const auto& take : _loopTakes)
		if (take && !take->ReconfigureVstFormat(sampleRate, blockSize)) return false;
	for (const auto& take : _backLoopTakes)
		if (take && !take->ReconfigureVstFormat(sampleRate, blockSize)) return false;
	return true;
}

void Station::SetSampleRate(float sampleRate)
{
    if (_sampleRate != sampleRate)
    {
        std::scoped_lock lock(_liveHeldMidiMutex);
        _recentPhysicalMidi.clear();
        _midiHistoryDroppedThroughSample.reset();
        _midiFormatEpochKnown.store(false, std::memory_order_release);
        _midiFormatEpochPending.store(true, std::memory_order_release);
    }
	_sampleRate = sampleRate;
	for (auto& take : _loopTakes)
		take->SetSampleRate(sampleRate);
	for (auto& take : _backLoopTakes)
		take->SetSampleRate(sampleRate);
}

void Station::SetLogging(const io::LoggingConfig& config) noexcept
{
	base::Jammable::SetLogging(config);

	for (auto& take : _loopTakes)
	{
		if (take)
			take->SetLogging(config);
	}

	for (auto& take : _backLoopTakes)
	{
		if (take)
			take->SetLogging(config);
	}
}

void Station::ForceUnloadAllVstPlugins()
{
	// Shutdown owns this thread after audio and job readers have stopped. There
	// will be no later audio acknowledgement to reclaim retired routing owners.
	_automationDispatch.store(nullptr);
	_automationDispatch.collect();
	const auto drainState = [](const std::shared_ptr<const AudioState>& state) {
		if (!state) return;
		_DrainVstChain(state->MidiChain);
		for (const auto& destination : state->MidiDestinations)
			if (destination && destination->Plugin)
				vst::QueueForUiThreadDestroy(destination->Plugin);
	};
	drainState(_audioState.exchange(nullptr, std::memory_order_acq_rel));
	for (const auto& retired : _retiredAudioStates)
		drainState(retired.State);
	_retiredAudioStates.clear();
	for (const auto& binding : _midiPlaybackBindings)
	{
		vst::QueueForUiThreadDestroy(binding->Destination->Plugin);
		// A removed take may still be pinned by a held playback route.
		binding->Take->ForceUnloadAllVstPlugins();
	}
	_syntheticLiveMidiIngress.Clear();
	_pendingLostPlaybackMidi.clear();
	_midiPlaybackBindings.clear();
	_midiDestinations.clear();
	auto chain = _vstChain.exchange(nullptr, std::memory_order_acq_rel);
	_DrainVstChain(std::move(chain));
	_DrainVstChain(std::move(_backVstChain));
	_flipVstChain.store(false, std::memory_order_release);
	_pendingVstLoads.clear();
	_pendingVstUnloads.clear();
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		_vstPluginPaths.clear();
		_vstSavedEntries.clear();
		_vstJobsPending.store(0u, std::memory_order_release);
	}
	for (auto& take : _loopTakes)
	{
		if (take)
			take->ForceUnloadAllVstPlugins();
	}
	for (auto& take : _backLoopTakes)
	{
		if (take)
			take->ForceUnloadAllVstPlugins();
	}
	_PublishAudioState();
}

void Station::SetNumBusChannels(unsigned int chans)
{
	_backAudioBuffers.clear();
	_backAudioMixers.clear();
	_guiRack->ClearRoutes();
	_guiRack->SetNumInputChannels(chans);

	for (unsigned int i = 0; i < chans; i++)
	{
		_backAudioBuffers.push_back(std::make_shared<audio::AudioBuffer>(_lastBufSize));

		MergeMixBehaviourParams mergeParams;
		if (i < _guiRack->NumOutputChannels())
			mergeParams.Channels.push_back(i);

		auto mixerParams = LoopTake::GetMixerParams(_guiParams.Size, mergeParams);
		auto mixer = std::make_shared<audio::AudioMixer>(mixerParams);
		mixer->SetUnmutedLevel(1.0);
		_backAudioMixers.push_back(mixer);

		if (i < _guiRack->NumOutputChannels())
			_guiRack->AddRoute(i, i);
	}
	_vstBlockScratch.resize(static_cast<size_t>(chans) * constants::MaxBlockSize);
	_vstBlockPtrs.resize(chans);
	for (unsigned int i = 0; i < chans; i++)
		_vstBlockPtrs[i] = _vstBlockScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);

	_flipAudioBuffer = true;
	_changesMade = true;

	for (auto& take : _loopTakes)
		take->SetNumBusChannels(chans);
}

void Station::SetNumAdcChannels(unsigned int chans)
{
}

void Station::SetNumDacChannels(unsigned int chans)
{
	auto& mixers = _flipAudioBuffer && _HasUncommittedChanges() ?
		_backAudioMixers :
		_audioMixers;

	for (auto& mixer : mixers)
	{
		mixer->SetMaxChannels(chans);
	}

	_masterMixer->SetMaxChannels(chans);

	bool initRoutes = (0 == _guiRack->NumOutputChannels()) && (chans > 0);
	
	_guiRack->SetNumOutputChannels(chans);

	if (initRoutes)
	{
		auto i = 0u;
		for (auto& mixer : mixers)
		{
			mixer->SetChannels({i});
			if (i < chans)
				_guiRack->AddRoute(i, i);

			i++;
		}
	}
}

unsigned int Station::NumBusChannels() const
{
	return (_HasUncommittedChanges() && _flipAudioBuffer) ?
		(unsigned int)_backAudioBuffers.size() :
		(unsigned int)_audioBuffers.size();
}

void Station::OnBounce(unsigned int numSamps,
	const io::UserConfig& config,
	const std::optional<audio::AudioStreamParams>& params)
{
	auto outLatency = params.has_value() ? params.value().OutputLatency : 0u;
	if (0u == outLatency)
		outLatency = config.Audio.LatencyOut;

	const auto sampleRate = params.has_value() ? params->SampleRate : config.Audio.SampleRate;
	const auto inputLatency = params.has_value() && params->InputLatency != 0u ? params->InputLatency : config.Audio.LatencyIn;
	const auto sourceAge = (std::max)(inputLatency, config.TriggerLoopAlignmentSamps(sampleRate));
	const auto sourceOffset = -static_cast<long>(sourceAge + outLatency);
	auto state = _BorrowAudioState();
	if (!state)
		return;

	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->ProcessActiveBounce(sourceOffset, numSamps);
}

bool Station::LayoutTakeRacks(const glm::mat4& viewProjection, utils::Size2d viewport)
{
	constexpr int padding = 8;
	std::optional<int> previousTop;
	bool changed = false;
	const auto stationPosition = PresentationModelPosition();
	const auto stationClip = viewProjection * glm::vec4(stationPosition.X, stationPosition.Y, stationPosition.Z, 1.0f);
	if (stationClip.w <= 0.0f) return false;
	const auto stationY = static_cast<int>((stationClip.y / stationClip.w + 1.0f) * 0.5f * viewport.Height);
	for (const auto& take : GetLoopTakes())
	{
		const auto rack = take->GetGuiRack();
		if (!rack || !take->IsVisible()) continue;
		const auto takePosition = take->ModelPosition();
		const auto scale = static_cast<float>(ModelScale());
		const auto clip = viewProjection * glm::vec4(
			stationPosition.X + takePosition.X * scale,
			stationPosition.Y + takePosition.Y * scale,
			stationPosition.Z + takePosition.Z * scale, 1.0f);
		if (clip.w <= 0.0f) continue;
		const auto centreY = static_cast<int>((clip.y / clip.w + 1.0f) * 0.5f * viewport.Height);
		const auto height = static_cast<int>(rack->GetSize().Height);
		// Anchor the first rack to its take. Later racks stop approaching their
		// predecessor at the padding limit, without switching layout modes.
		auto bottom = centreY - height / 2;
		if (previousTop) bottom = (std::max)(bottom, *previousTop + padding);
		previousTop = bottom + height;
		auto position = rack->Position();
		position.Y = bottom - stationY - take->Position().Y;
		if (position.Y != rack->Position().Y)
		{
			rack->SetPosition(position);
			changed = true;
		}
	}
	return changed;
}

void Station::SetRackVisibility(bool showStationRack, bool showLoopTakeRacks)
{
	// Set the visibility of the Station (master/channels/router) rack
	_guiRack->SetVisible(showStationRack);

	// Set the visibility of each LoopTake rack
	for (auto& take : _loopTakes)
	{
		take->SetRackVisibility(showLoopTakeRacks);
	}
}

bool Station::AcceptsLiveMidiChannel(std::uint8_t channel) const noexcept
{
	const auto mask = _allowedMidiChannelMask.load(std::memory_order_acquire);
	if (mask == 0u)
		return false;

	if (channel >= 16u)
		return false;

	const auto bit = static_cast<std::uint16_t>(1u << channel);
	return (mask & bit) != 0u;
}

void Station::SetAllowedMidiChannels(const std::vector<int>& channels)
{
	std::vector<int> filtered;
	filtered.reserve(channels.size());

	const auto oldMask = _allowedMidiChannelMask.load(std::memory_order_acquire);
	std::uint16_t mask = 0u;
	for (auto channel : channels)
	{
		if (channel < 1 || channel > 16)
			continue;

		const auto zeroBased = static_cast<std::uint8_t>(channel - 1);
		const auto bit = static_cast<std::uint16_t>(1u << zeroBased);
		if ((mask & bit) != 0u)
			continue;

		mask = static_cast<std::uint16_t>(mask | bit);
		filtered.push_back(channel);
	}

	const auto removedMask = static_cast<std::uint16_t>(oldMask & static_cast<std::uint16_t>(~mask));
	if (removedMask != 0u)
	{
		std::vector<MidiEvent> noteOffs;
		{
			std::scoped_lock lock(_liveHeldMidiMutex);
			for (std::uint8_t channel = 0; channel < 16; ++channel)
				if ((removedMask & (1u << channel)) != 0u)
					for (std::uint8_t note = 0; note < 128; ++note)
						_anonymousLiveHeldMidi.Clear(channel, note);
			for (auto& [deviceName, heldSnapshot] : _liveHeldMidi)
			{
				const bool queueNoteOffs = deviceName.empty();
				for (std::uint8_t ch = 0u; ch < 16u; ++ch)
				{
					const auto bit = static_cast<std::uint16_t>(1u << ch);
					if ((removedMask & bit) == 0u)
						continue;

					for (std::uint8_t note = 0u; note < 128u; ++note)
					{
						if (!heldSnapshot.Held.test(MidiNote::NoteSlot(ch, note)))
							continue;

						if (queueNoteOffs)
							noteOffs.push_back(MidiEvent::MakeNoteOff(0u, ch, note));
						heldSnapshot.Clear(ch, note);
					}
				}
			}
		}

		for (const auto& noteOff : noteOffs)
			TryEnqueueSyntheticLiveMidi(noteOff);
	}

	_allowedMidiChannels = std::move(filtered);
	_allowedMidiChannelMask.store(mask, std::memory_order_release);

	if (_guiRack)
		_guiRack->SetAllowedMidiChannels(_allowedMidiChannels, true);
}

bool Station::TryEnqueueImmediateLiveMidi(const MidiEvent& event) noexcept
{
	if (MembershipRemoving()) return false;
	return _TryEnqueueOrderedLiveMidi(_immediateLiveMidiIngress,
		_hasLastImmediateLiveMidiSample,
		_lastImmediateLiveMidiSample,
		event);
}

void Station::ResetLiveMidiWhileQuiescent() noexcept
{
	_immediateLiveMidiIngress.Clear();
	_hasLastImmediateLiveMidiSample = false;
	_lastImmediateLiveMidiSample = 0u;
	// Playback release records carry pinned binding acknowledgements. Retain
	// them exactly; discard monitoring events, including undelivered NoteOns.
	std::array<SyntheticMidi, 1024u> playbackReleases{};
	std::size_t count = 0u;
	SyntheticMidi entry{};
	while (_syntheticLiveMidiIngress.Pop(entry))
		if (entry.Release) playbackReleases[count++] = entry;
	_syntheticLiveMidiIngress.Clear();
	_hasLastSyntheticLiveMidiSample = false;
	_lastSyntheticLiveMidiSample = 0u;
	for (std::size_t i = 0; i < count; ++i)
	{
		playbackReleases[i].Event.sampleOffset = 0u;
		_TryEnqueueSyntheticMidi(playbackReleases[i]);
	}
	for (const auto& weak : _midiDestinations)
		if (const auto destination = weak.lock())
		{
			destination->PendingLiveReset |= destination->LiveHeld;
			destination->LiveHeld.reset();
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->LiveSustain.test(channel) && !destination->RecordedSustainOwners[channel])
					destination->PendingSustainRelease.set(channel);
			destination->LiveSustain.reset();
		}
}

bool Station::TryEnqueueSyntheticLiveMidi(const MidiEvent& event) noexcept
{
	if (MembershipRemoving()) return false;
	return _TryEnqueueSyntheticMidi({ event });
}

bool Station::_TryEnqueueSyntheticMidi(SyntheticMidi entry) noexcept
{
	auto& queued = entry.Event;
	if (_hasLastSyntheticLiveMidiSample &&
		midi::MidiSampleDelta(queued.sampleOffset, _lastSyntheticLiveMidiSample) < 0)
		queued.sampleOffset = _lastSyntheticLiveMidiSample;
	if (!_syntheticLiveMidiIngress.Push(entry)) return false;
	_lastSyntheticLiveMidiSample = queued.sampleOffset;
	_hasLastSyntheticLiveMidiSample = true;
	return true;
}

void Station::ObservePhysicalMidiForRecording(const MidiEvent& event, const std::string& deviceName)
{
    if (MembershipRemoving()) return;
    if (event.MessageType() == 0xb0u && event.data1 == 64u)
    {
        if (event.data2 >= 64u && !AcceptsLiveMidiChannel(event.Channel())) return;
        std::scoped_lock lock(_liveHeldMidiMutex);
        auto found = std::find_if(_liveSustainMidi.begin(), _liveSustainMidi.end(), [&](const auto& item) { return item.first == deviceName; });
        if (found == _liveSustainMidi.end())
        {
            _liveSustainMidi.push_back({deviceName, {}});
            found = std::prev(_liveSustainMidi.end());
        }
        found->second.set(event.Channel(), event.data2 >= 64u);
        return; // Preserve the existing note-only capture contract.
    }
    if (!event.IsNoteOn() && !event.IsNoteOff()) return;
    if (event.IsNoteOn() && !AcceptsLiveMidiChannel(event.Channel())) return;
    {
        const auto channel = event.Channel();
        const auto note = static_cast<std::uint8_t>(event.data1 & 0x7Fu);
        const auto slot = midi::MidiNote::NoteSlot(channel, note);
        std::scoped_lock lock(_liveHeldMidiMutex);
        auto upsert = [&](const std::string& key) {
            auto it = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
                [&](const auto& item) { return item.first == key; });
            if (it == _liveHeldMidi.end())
            {
                _liveHeldMidi.push_back({key, MidiNoteSnapshot{}});
                it = std::prev(_liveHeldMidi.end());
            }
            const auto prior = it->second.Velocity[slot];
            if (event.IsNoteOn()) it->second.Set(channel, note, event.data2);
            else it->second.Clear(channel, note);
            return prior;
        };
        const auto priorAny = upsert("");
        const auto priorDevice = deviceName.empty() ? priorAny : upsert(deviceName);
        if (deviceName.empty())
        {
            if (event.IsNoteOn()) _anonymousLiveHeldMidi.Set(channel, note, event.data2);
            else _anonymousLiveHeldMidi.Clear(channel, note);
        }
        _recentPhysicalMidi.push_back({event, deviceName, priorDevice, priorAny, ++_physicalMidiSequence});
        // Capacity plus one second of activation grace, at the active format.
        // Scan all entries: endpoints are drained separately, not in time order.
        const auto retained = static_cast<std::int64_t>(_sampleRate * 1.2f);
        auto drop = [&](const RecentPhysicalMidi& item) {
            if (!_midiHistoryDroppedThroughSample || midi::MidiSampleDelta(item.Event.sampleOffset,
                *_midiHistoryDroppedThroughSample) > 0)
                _midiHistoryDroppedThroughSample = item.Event.sampleOffset;
        };
        std::erase_if(_recentPhysicalMidi, [&](const auto& item) {
            if (midi::MidiSampleDelta(event.sampleOffset, item.Event.sampleOffset) <= retained) return false;
            drop(item);
            return true;
        });
        if (_recentPhysicalMidi.size() > 4096u)
        {
            drop(_recentPhysicalMidi.front());
            _recentPhysicalMidi.erase(_recentPhysicalMidi.begin());
        }
    }
    // Job targets cover START -> publication. This is the only ordinary recording
    // delivery; published callback topology is deliberately not the capture list.
    for (auto& target : _midiCaptureTargets)
        if (auto take = target.lock(); take && !take->IsCaptureCancelled())
            take->RecordMidiEvent(event, deviceName, event.sampleOffset);
}

void Station::CompleteMidiIngressPump(std::uint32_t deviceSample, bool overflow)
{
    std::erase_if(_midiCaptureTargets, [&](auto& target) {
        auto take = target.lock();
        if (!take || take->IsCaptureCancelled()) return true;
        if (overflow)
        {
            take->CancelCapture(true);
            return true;
        }
        take->CompleteMidiIngressPump(deviceSample);
        return !take->MidiTriggerCapturePending() && !take->IsArmed();
    });
}

void Station::FlushLiveHeldMidiNotes() noexcept
{
	midi::MidiNoteSnapshot heldSnapshot;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto liveHeld = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[](const auto& pair) { return pair.first.empty(); });
		if (liveHeld != _liveHeldMidi.end())
			heldSnapshot = liveHeld->second;
		_liveHeldMidi.clear();
		for (const auto& pedal : _liveSustainMidi) _pendingLostSustain |= pedal.second;
		_liveSustainMidi.clear();
        _recentPhysicalMidi.clear();
        _midiHistoryDroppedThroughSample.reset();
		_anonymousLiveHeldMidi = {};
	}

	FlushPendingLostLiveMidiNotes();
	if (heldSnapshot.Held.none()) return;

	for (std::uint8_t ch = 0u; ch < 16u; ++ch)
	{
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			if (heldSnapshot.Held.test(MidiNote::NoteSlot(ch, note)))
				TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOff(0u, ch, note));
		}
	}
}

void Station::FlushLiveHeldMidiNotesForDevice(const std::string& deviceName) noexcept
{
	if (deviceName.empty())
		return;
	midi::MidiNoteSnapshot released;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto lost = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[&deviceName](const auto& entry) { return entry.first == deviceName; });
		if (lost != _liveHeldMidi.end())
		{
			released = lost->second;
			_liveHeldMidi.erase(lost);
		}
		auto pedal = std::find_if(_liveSustainMidi.begin(), _liveSustainMidi.end(), [&](const auto& item) { return item.first == deviceName; });
		if (pedal != _liveSustainMidi.end())
		{
			auto releasedPedal = pedal->second;
			_liveSustainMidi.erase(pedal);
			for (const auto& healthy : _liveSustainMidi) releasedPedal &= ~healthy.second;
			_pendingLostSustain |= releasedPedal;
		}
		auto remaining = _anonymousLiveHeldMidi;
		for (const auto& entry : _liveHeldMidi)
			if (!entry.first.empty())
				for (size_t slot = 0; slot < midi::MidiNote::TotalNoteSlots; ++slot)
					if (entry.second.Held.test(slot)) {
						remaining.Held.set(slot);
						remaining.Velocity[slot] = entry.second.Velocity[slot];
					}
		released.Held &= ~remaining.Held;
		auto aggregate = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[](const auto& entry) { return entry.first.empty(); });
		if (aggregate != _liveHeldMidi.end()) aggregate->second = remaining;
		_pendingLostLiveMidiNoteOffs |= released.Held;
	}
	FlushPendingLostLiveMidiNotes();
}

void Station::FlushPendingLostLiveMidiNotes() noexcept
{
	_FlushPendingLostPlaybackMidi();
	// Job producer only. Retain release bits across a full synthetic queue;
	// a surviving endpoint taking ownership of the same note suppresses release.
	std::scoped_lock lock(_liveHeldMidiMutex);
	for (auto channel = 0u; channel < 16u; ++channel)
		if (_pendingLostSustain.test(channel))
		{
			const bool healthy = std::any_of(_liveSustainMidi.begin(), _liveSustainMidi.end(), [channel](const auto& item) { return item.second.test(channel); });
			if (!healthy && !_TryEnqueueSyntheticMidi({{0u, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, nullptr, true, _liveSustainRevisions[channel].load(std::memory_order_acquire)})) return;
			_pendingLostSustain.reset(channel);
		}
	if (_pendingLostLiveMidiNoteOffs.none()) return;
	const auto aggregate = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
		[](const auto& entry) { return entry.first.empty(); });
	if (aggregate != _liveHeldMidi.end())
		_pendingLostLiveMidiNoteOffs &= ~aggregate->second.Held;
	for (std::uint8_t channel = 0; channel < 16; ++channel)
		for (std::uint8_t note = 0; note < 128; ++note)
		{
			const auto slot = midi::MidiNote::NoteSlot(channel, note);
			if (_pendingLostLiveMidiNoteOffs.test(slot))
			{
				if (!_TryEnqueueSyntheticMidi({ MidiEvent::MakeNoteOff(0u, channel, note), nullptr, true,
					_liveNoteOnRevisions[slot].load(std::memory_order_acquire) }))
					return;
				_pendingLostLiveMidiNoteOffs.reset(slot);
			}
		}
}

void Station::SetMidiVstRoute(unsigned int midiOutputIndex, size_t vstIndex)
{
	const auto* current = _midiVstRoutes.load(std::memory_order_acquire);
	auto nextRoutes = current ?
		std::make_unique<MidiVstRoutingSnapshot>(*current) :
		std::make_unique<MidiVstRoutingSnapshot>();

	if (midiOutputIndex == LiveMidiOutputIndex)
	{
		nextRoutes->LivePlugin = vstIndex;
	}
	else
	{
		if (midiOutputIndex >= nextRoutes->PluginByMidiOutput.size())
			nextRoutes->PluginByMidiOutput.resize(static_cast<size_t>(midiOutputIndex) + 1u,
				MidiVstRoutingSnapshot::NoPlugin);
		nextRoutes->PluginByMidiOutput[midiOutputIndex] = vstIndex;
	}

	const auto* published = nextRoutes.get();
	_retainedMidiVstRoutes.push_back(std::move(nextRoutes));
	_midiVstRoutes.store(published, std::memory_order_release);
	_PublishAudioState();
}

void Station::ClearMidiVstRoutes()
{
	auto nextRoutes = std::make_unique<MidiVstRoutingSnapshot>();
	const auto* published = nextRoutes.get();
	_retainedMidiVstRoutes.push_back(std::move(nextRoutes));
	_midiVstRoutes.store(published, std::memory_order_release);
	_PublishAudioState();
}

Station::MidiVstRoutingSnapshot Station::SnapshotMidiVstRoutesForExport() const
{
	const auto* current = _midiVstRoutes.load(std::memory_order_acquire);
	return current ? *current : MidiVstRoutingSnapshot{};
}

bool Station::RestoreMidiVstRoutes(const MidiVstRoutingSnapshot& routes,
	size_t loadedPluginCount)
{
	if (routes.PluginByMidiOutput.size() > MaxMidiVstRouteOutputs)
		return false;

	auto isValidPlugin = [loadedPluginCount](size_t pluginIndex) noexcept {
		return pluginIndex == MidiVstRoutingSnapshot::NoPlugin || pluginIndex == MidiVstRoutingSnapshot::MissingPlugin || pluginIndex < loadedPluginCount;
	};
	if (!isValidPlugin(routes.LivePlugin))
		return false;
	for (const auto pluginIndex : routes.PluginByMidiOutput)
		if (!isValidPlugin(pluginIndex))
			return false;

	// Copy before publishing. Retained ownership prevents the callback from
	// observing freed route storage after the atomic pointer handoff.
	auto restored = std::make_unique<MidiVstRoutingSnapshot>(routes);
	const auto* published = restored.get();
	_retainedMidiVstRoutes.push_back(std::move(restored));
	_midiVstRoutes.store(published, std::memory_order_release);
	_PublishAudioState();
	return true;
}

unsigned int Station::_CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes)
{
	if (0 == numTakes)
		return 0;

	int minHeight = 5;
	int totalGap = (numTakes + 1) * _Gap.Width;
	int height = (((int)stationHeight) - totalGap) / (int)numTakes;

	if (height < minHeight)
		return minHeight;

	return height;
}

void Station::_InitReceivers()
{
	_guiRack->SetReceiver(ActionReceiver::shared_from_this());
}

void Station::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	if (_quantisationModel)
		_quantisationModel->InitResources(resourceLib, forceInit);
	if (_quantisationDivisionModel)
		_quantisationDivisionModel->InitResources(resourceLib, forceInit);

	if (_stationModel)
		_stationModel->InitResources(resourceLib, forceInit);

	GuiElement::_InitResources(resourceLib, forceInit);
}

void Station::_ReleaseResources()
{
	FinishPresentationMove();
	if (_quantisationModel)
		_quantisationModel->ReleaseResources();
	if (_quantisationDivisionModel)
		_quantisationDivisionModel->ReleaseResources();

	if (_stationModel)
		_stationModel->ReleaseResources();

	GuiElement::_ReleaseResources();
}

std::vector<JobAction> Station::_CommitChanges()
{
	if (const auto request = _firstMasterRecording.load(); request &&
		request->Timing.Version <= _firstMasterRecordingConsumed.load(std::memory_order_acquire))
	{
		const auto status = _firstMasterRecordingStatus.load(std::memory_order_relaxed);
		std::cout << "[midi-master] take=" << request->Take->Id()
			<< " status=" << (status == 1u ? "activated" : (status == 2u ? "playback-only" : "cancelled"))
			<< " origin=" << request->Timing.RecordOrigin
			<< " scene=" << _firstMasterRecordingScene.load(std::memory_order_relaxed)
			<< " logical=" << request->Timing.LengthSamps
			<< " phase=" << _firstMasterRecordingPhase.load(std::memory_order_relaxed)
			<< " version=" << request->Timing.Version << '\n';
		_firstMasterRecording.store(nullptr);
	}
	_firstMasterRecording.collect();
	// Plugin latency/controller notifications must be pumped with closed editors.
	if (const auto chain = _vstChain.load(std::memory_order_acquire))
		chain->PollPendingChanges();

	ReleaseRetiredAudioStates();
	bool audioStateChanged = false;
	if (_flipTakeBuffer)
	{
		_flipTakeBuffer = false;
		audioStateChanged = true;

		// Remove and add any children
		// (difference between back and front LoopTake buffer)
		std::vector<std::shared_ptr<LoopTake>> toAdd;
		std::vector<std::shared_ptr<LoopTake>> toRemove;
		std::copy_if(_backLoopTakes.begin(), _backLoopTakes.end(), std::back_inserter(toAdd), [&](const std::shared_ptr<LoopTake>& take) { return (std::find(_loopTakes.begin(), _loopTakes.end(), take) == _loopTakes.end()); });
		std::copy_if(_loopTakes.begin(), _loopTakes.end(), std::back_inserter(toRemove), [&](const std::shared_ptr<LoopTake>& take) { return (std::find(_backLoopTakes.begin(), _backLoopTakes.end(), take) == _backLoopTakes.end()); });

		for (auto& take : toAdd)
		{
			take->SetParent(GuiElement::shared_from_this());
			take->Init();
			_children.push_back(take);
		}

		for (auto& take : toRemove)
		{
			auto child = std::find(_children.begin(), _children.end(), take);
			if (_children.end() != child)
				_children.erase(child);
		}

		// Re-index children so _index values are consistent with TryGetChild lookups
		for (unsigned int i = 0; i < _children.size(); i++)
			_children[i]->SetIndex(i);

		_loopTakes = _backLoopTakes; // TODO: Undo?
	}

	if (_flipAudioBuffer)
	{
		_flipAudioBuffer = false;
		audioStateChanged = true;
		_audioBuffers = _backAudioBuffers;
		_audioMixers = _backAudioMixers;

		_guiRack->SetNumInputChannels((unsigned int)_audioBuffers.size());

		for (auto& take : _loopTakes)
		{
			take->SetNumBusChannels((unsigned int)_audioBuffers.size());
		}

		_WireVuSliders();
	}
	// Swap in the VST chain if the job thread has delivered a new one.
	if (_flipVstChain.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		_flipVstChain.store(false, std::memory_order_release);
		_vstChain.store(_backVstChain, std::memory_order_release);
		audioStateChanged = true;
	}
	if (audioStateChanged) _PublishAudioState();
	if (audioStateChanged || _automationDispatchDirty)
	{
		_automationDispatchDirty = false;
		RebuildAutomationDispatch();
	}

	// Detect pending VST load/unload requests and queue a job for them.
	std::vector<JobAction> jobs;

	// Drain pending unload indices — emit one JOB_UNLOADVST per entry so
	// rapid back-to-back UnloadVstPlugin() calls are not silently dropped.
	auto pendingUnloads = std::move(_pendingVstUnloads);
	auto pendingLoads = std::move(_pendingVstLoads);
	for (auto& job : pendingUnloads)
	{
		job.SourceId = Name();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}
	for (auto& job : pendingLoads)
	{
		job.SourceId = Name();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}

	GuiElement::_CommitChanges();

	return jobs;
}

const std::shared_ptr<AudioSink> Station::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	auto state = _BorrowAudioState();
	if (!state)
		return nullptr;

	switch (source)
	{
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		if (channel < state->AudioBuffers.size())
			return state->AudioBuffers[channel];
		break;
	}

	return nullptr;
}

bool Station::TryClaimPreparedMidiQuantisationAtAudioBoundary() noexcept
{
	std::uint8_t expected = 0u;
	if (!_promotionTopologyGate.compare_exchange_strong(expected, 1u,
		std::memory_order_seq_cst)) return false;
	_promotionAudioLease = _BorrowAudioState();
	if (!_promotionAudioLease.ReadSucceeded())
	{
		ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
		return false;
	}
	if (_promotionAudioLease)
		for (const auto& take : _promotionAudioLease->OwnedLoopTakes)
			if (take && !take->TryClaimPreparedMidiQuantisationAtAudioBoundary())
			{
				ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
				return false;
			}
	return true;
}

void Station::ReleasePreparedMidiQuantisationClaimAtAudioBoundary() noexcept
{
	if (_promotionAudioLease)
		for (const auto& take : _promotionAudioLease->OwnedLoopTakes)
			if (take) take->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
	_promotionAudioLease.reset();
	std::uint8_t expected = 1u;
	_promotionTopologyGate.compare_exchange_strong(expected, 0u,
		std::memory_order_seq_cst);
}

void Station::ActivatePreparedMidiQuantisationAtAudioBoundary() noexcept
{
	if (_promotionAudioLease)
		for (const auto& take : _promotionAudioLease->OwnedLoopTakes)
			if (take) take->ActivatePreparedMidiQuantisationAtAudioBoundary();
}

std::optional<std::uint32_t> Station::MasterPhaseAtAudioBoundary(
	const LoopTake* targetTake, const Loop* targetLoop,
	std::uint32_t masterLength) const noexcept
{
	const auto state = _BorrowAudioState();
	if (!state) return std::nullopt;
	for (const auto& take : state->OwnedLoopTakes)
		if (take.get() == targetTake)
			return take->MasterPhaseAtAudioBoundary(targetLoop, masterLength);
	return std::nullopt;
}

std::shared_ptr<const Station::AudioState> Station::_AudioStateSnapshot() const
{
	return _audioState.load(std::memory_order_acquire);
}

base::PublishedAudioSnapshot<const Station::AudioState>::Lease Station::_BorrowAudioState() const noexcept
{
	return _audioState.borrow();
}

std::shared_ptr<const Station::LoopTakeSnapshot> Station::_LoopTakeSnapshotState() const
{
	return _loopTakeSnapshot.load(std::memory_order_acquire);
}

void Station::_PublishLoopTakeSnapshot()
{
	auto state = std::make_shared<LoopTakeSnapshot>();
	const auto& takes = (_HasUncommittedChanges() && _flipTakeBuffer) ?
		_backLoopTakes :
		_loopTakes;
	state->reserve(takes.size());
	for (const auto& take : takes)
		state->push_back(take);
	_loopTakeSnapshot.store(std::move(state), std::memory_order_release);
}

void Station::_PublishAudioState()
{
	auto state = std::make_shared<AudioState>();
	state->Generation = _nextAudioStateGeneration++;
	state->OwnedLoopTakes = _loopTakes;
	state->LoopTakes.reserve(_loopTakes.size());
	for (const auto& take : _loopTakes)
		state->LoopTakes.push_back(take);
	state->AudioMixers = _audioMixers;
	state->AudioBuffers = _audioBuffers;
	_PrepareMidiDestinations(*state);
	state->VstRenderChannels = static_cast<unsigned int>(state->AudioBuffers.size());
	for (const auto& destination : state->MidiDestinations) state->VstRenderChannels = std::max(state->VstRenderChannels, destination->RenderChannels);
	for (const auto& destination : state->RetiredMidiDestinations) state->VstRenderChannels = std::max(state->VstRenderChannels, destination->RenderChannels);
	state->InstrumentScratch.resize(state->VstRenderChannels * constants::MaxBlockSize);
	state->InstrumentPtrs.resize(state->VstRenderChannels, nullptr);
	for (auto i = 0u; i < state->VstRenderChannels; ++i)
		state->InstrumentPtrs[i] = state->InstrumentScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);
	state->VstBlockScratch.resize(state->VstRenderChannels * constants::MaxBlockSize);
	state->VstBlockPtrs.resize(state->VstRenderChannels, nullptr);
	for (auto i = 0u; i < state->VstRenderChannels; i++)
		state->VstBlockPtrs[i] = state->VstBlockScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);
	std::uint8_t expected = 0u;
	while (!_promotionTopologyGate.compare_exchange_weak(expected, 2u,
		std::memory_order_seq_cst))
	{
		expected = 0u;
		std::this_thread::yield();
	}
	auto retired = _audioState.exchange(std::shared_ptr<const AudioState>(state), std::memory_order_acq_rel);
	_promotionTopologyGate.store(0u, std::memory_order_seq_cst);
	if (retired)
		_retiredAudioStates.push_back({ state->Generation, std::move(retired) });
}

void Station::AcknowledgeAudioBoundary() noexcept
{
	const auto state = _BorrowAudioState();
	if (state)
		_audioCompletedStateGeneration.store(state->Generation, std::memory_order_release);
}

bool Station::HasUnacknowledgedAudioState() const noexcept
{
	const auto state = _AudioStateSnapshot();
	return state && state->Generation >
		_audioCompletedStateGeneration.load(std::memory_order_acquire);
}

void Station::ReleaseRetiredAudioStates()
{
	_audioState.collect();
	_automationDispatch.collect();
	const auto completed = _audioCompletedStateGeneration.load(std::memory_order_acquire);
	auto keep = std::remove_if(_retiredAudioStates.begin(), _retiredAudioStates.end(),
		[completed](const RetiredAudioState& retired)
		{
			return retired.State && retired.ReleaseAfterGeneration <= completed;
		});
	_retiredAudioStates.erase(keep, _retiredAudioStates.end());
	// Registry pins also preserve notes delivered under an earlier route. A
	// binding with held notes survives route/index changes until cancellation.
	std::erase_if(_midiPlaybackBindings, [](const auto& binding) {
		return binding.use_count() == 1 && !binding->HasHeld.load(std::memory_order_acquire)
			&& binding->QueuedEvents.load(std::memory_order_acquire) == 0u;
	});
	const auto current = _AudioStateSnapshot();
	if (current && current->RetiredMidiQueueCancelled.load(std::memory_order_acquire))
		_PublishAudioState(); // Off callback: drop one-generation retirement pins.
}

void Station::_ArrangeChildren()
{
	auto numTakes = (unsigned int)_backLoopTakes.size();

	auto takeHeight = _CalcTakeHeight(_sizeParams.Size.Height, numTakes);
	utils::Size2d takeSize = { _sizeParams.Size.Width - (2 * _Gap.Width), static_cast<unsigned int>((std::max)(1, static_cast<int>(takeHeight) - 2 * static_cast<int>(_Gap.Height))) };

	auto takeCount = 0;
	for (auto& take : _backLoopTakes)
	{
		take->SetPosition({ 0, (int)(_Gap.Height + (takeCount * takeHeight)) });
		take->SetSize(takeSize);
		take->SetModelPosition({ 0.0f, (float)(takeCount * takeHeight), 0.0f });
		take->SetModelScale(1.0);

		takeCount++;
	}
}

GuiRackParams Station::_GetRackParams(utils::Size2d size)
{
	GuiRackParams rackParams;
	rackParams.EnableVstControl = true;
	rackParams.EnableStationLabel = true;
	rackParams.EnableVstRack = true;
	rackParams.EnableVstRackMidiRouting = true;
	rackParams.Position = { 0, 0 };
	rackParams.Size = size;
	rackParams.MinSize = rackParams.Size;
	rackParams.NumInputChannels = NumBusChannels();
	rackParams.NumOutputChannels = 2;
	rackParams.InitLevel = 1.0;
	rackParams.InitState = gui::GuiRackParams::RACK_MASTER;
	rackParams.AllowedMidiChannels = _allowedMidiChannels;
	rackParams.OnAllowedMidiChannelsChanged =
		[this](const std::vector<int>& channels)
		{
			SetAllowedMidiChannels(channels);
		};

	return rackParams;
}

std::optional<std::shared_ptr<LoopTake>> Station::_TryGetTake(std::string id)
{
	for (auto& take : _loopTakes)
	{
		if (take->Id() == id)
			return take;
	}
	for (auto& take : _backLoopTakes)
	{
		if (take->Id() == id)
			return take;
	}

	return std::nullopt;
}

void Station::_CollapseOtherTakeRouters()
{
	auto& takes = (_HasUncommittedChanges() && _flipTakeBuffer) ?
		_backLoopTakes :
		_loopTakes;

	for (auto& take : takes)
		take->CollapseRackToMaster();
}

void Station::_CollapseOtherTakeRoutersToChannels()
{
	auto& takes = (_HasUncommittedChanges() && _flipTakeBuffer) ?
		_backLoopTakes :
		_loopTakes;

	for (auto& take : takes)
		take->CollapseRouterToChannels();
}

void Station::_WireVuSliders()
{
	if (!_guiRack || !_masterMixer)
		return;

	auto masterSlider = _guiRack->GetMasterSlider();
	if (masterSlider)
		masterSlider->SetMixer(_masterMixer);

	for (auto i = 0u; i < _audioMixers.size(); i++)
	{
		auto slider = _guiRack->GetChannelSlider(i);
		if (slider)
			slider->SetMixer(_audioMixers[i]);
	}
}

void Station::_DitchLoopTake(std::shared_ptr<LoopTake>& take, bool flushLiveHeld) noexcept
{
	if (const auto request = _firstMasterRecording.load(); request && request->Take == take)
		request->Clock->CancelRecordingSeedOnOwner(request->Timing.Version);
	if (flushLiveHeld)
		FlushLiveHeldMidiNotes();
	else
	{
		// Publish cancellation before enqueueing releases. A callback already
		// rendering this take may finish, but its delivered ownership stays on audio.
		take->CancelCapture(true);
		_QueueLostPlaybackMidi(take);
	}

	// Recorded releases must retire their callback-owned binding first. A live
	// synthetic NoteOff can be suppressed while that binding still owns the
	// pitch; sampling loop-held notes here cannot replace ownership retirement.
	if (flushLiveHeld) for (const auto& midiLoop : take->GetMidiLoopSnapshot())
		if (midiLoop) midiLoop->RequestHeldFlush();
	// A retained callback snapshot may still hold this take. Cancel only its
	// logical capture; keep loop buffers and writers stable until job/UI retirement.
	take->CancelCapture();
	if (flushLiveHeld) _QueueLostPlaybackMidi(take);
	take->DetachGuiOwnershipForRetirement();
	const auto match = std::find(_backLoopTakes.begin(), _backLoopTakes.end(), take);
	if (match != _backLoopTakes.end())
	{
		_backLoopTakes.erase(match);
		_ArrangeChildren();
		_flipTakeBuffer = true;
		_changesMade = true;
		_PublishLoopTakeSnapshot();
		_loopTakeRevision.fetch_add(1u, std::memory_order_release);
	}
	RebuildAutomationDispatch();
}

void Station::LoadVstPlugin(std::wstring path, std::vector<std::uint8_t> initialState)
{
	const auto before = _pendingVstLoads.size();
	if (LoadManagedVstSlot(std::move(path)) && _pendingVstLoads.size() > before)
	{
		_pendingVstLoads.back().VstInitialState = initialState;
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		_vstSavedEntries.back().State = io::JamFile::VstEntry::EncodeState(initialState);
	}
}

vst::VstRackSlot Station::GetVstRackSlot() const
{
	vst::VstRackSlot slot;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	slot.Pending = _vstJobsPending.load(std::memory_order_acquire) != 0u
		|| _flipVstChain.load(std::memory_order_acquire);
	if (_vstSavedEntries.empty()) return slot;
	const auto& entry = _vstSavedEntries.front();
	slot.Path = utils::DecodeUtf8(entry.Path);
	slot.Identity = entry.Id;
	const auto chain = _vstChain.load(std::memory_order_acquire);
	slot.Plugin = chain ? chain->GetPlugin(0u) : nullptr;
	slot.Failed = !slot.Plugin;
	return slot;
}

bool Station::LoadManagedVstPlugin(std::wstring path)
{
	return LoadManagedVstSlot(std::move(path));
}

std::vector<vst::VstRackSlot> Station::GetVstRackSlots(std::size_t offset, std::size_t count) const
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	std::vector<vst::VstRackSlot> result;
	for (std::size_t index = std::min(offset, _vstSavedEntries.size()), end = index + std::min({count, MaxRackPlugins, _vstSavedEntries.size() - index}); index < end; ++index)
	{
		const auto& entry = _vstSavedEntries[index];
		vst::VstRackSlot slot;
		slot.Identity = entry.Id;
		slot.Path = utils::DecodeUtf8(entry.Path);
		slot.Plugin = chain ? chain->GetPlugin(index) : nullptr;
		slot.Pending = std::find(_pendingRackIds.begin(), _pendingRackIds.end(), entry.Id) != _pendingRackIds.end();
		slot.Failed = !slot.Plugin && !slot.Pending;
		result.push_back(std::move(slot));
	}
	return result;
}

bool Station::LoadManagedVstSlot(std::wstring path, const std::string& retryIdentity)
{
	if (path.empty() || _rackInactive) return false;
	std::string identity = retryIdentity;
	std::vector<std::uint8_t> state;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (std::find(_pendingRackIds.begin(), _pendingRackIds.end(), identity) != _pendingRackIds.end()) return false;
		const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
		if (!identity.empty())
		{
			const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == identity; });
			if (found == _vstSavedEntries.end()) return false;
			const auto index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
			if (chain && chain->GetPlugin(index)) return false;
			state = found->DecodeState();
		}
		else
		{
			if (_vstSavedEntries.size() >= MaxRackPlugins) { _rackDiagnostic = "Rack supports at most 32 plugins; remove a slot first."; return false; }
			const auto audio = _AudioStateSnapshot();
			if (audio && _vstSavedEntries.size() + audio->RetiredMidiDestinations.size() >= MaxRackDestinations) { _rackDiagnostic = "Waiting for removed plugin release recovery; retry shortly."; return false; }
			identity = utils::GetGuid();
			io::JamFile::VstEntry entry;
			entry.Id = identity;
			entry.Path = utils::EncodeUtf8(path);
			_vstSavedEntries.push_back(std::move(entry));
			_vstPluginPaths.push_back(path);
		}
		_pendingRackIds.push_back(identity);
		++_rackRevision;
	}
	JobAction job;
	job.JobActionType = JobAction::JOB_LOADVST;
	job.VstPath = std::move(path);
	job.VstInitialState = std::move(state);
	job.VstIdentity = identity;
	job.VstRequestId = utils::GetGuid();
	job.VstManaged = true;
	_pendingVstLoads.push_back(std::move(job));
	_vstJobsPending.fetch_add(1u, std::memory_order_release);
	_changesMade = true;
	return true;
}

bool Station::UnloadManagedVstPlugin(const std::string& identity)
{
	std::size_t index;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == identity; });
		if (identity.empty() || found == _vstSavedEntries.end()) return false;
		index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
		// A load completing after this gesture sees the identity gone, even
		// while its SDK call is in flight. Its processor is retired on the UI.
		std::erase(_pendingRackIds, identity);
	}
	UnloadVstPlugin(index);
	return true;
}

bool Station::SetLiveVstChannelRoute(unsigned int channel, midi::LiveVstChannelRoute route, std::uint64_t revision)
{
	if (channel >= 16u) return false;
	auto routes = _liveVstChannelRoutes;
	routes[channel] = std::move(route);
	return SetLiveVstChannelRoutes(std::move(routes), revision);
}

bool Station::SetLiveVstChannelRoutes(std::array<midi::LiveVstChannelRoute, 16> routes, std::uint64_t revision)
{
	if (revision != _rackRevision) { _rackDiagnostic = "Rack changed; retry the gesture."; return false; }
	const auto slots = GetVstRackSlots();
	for (auto channel = 0u; channel < 16u; ++channel)
	{
		auto& route = routes[channel];
		std::sort(route.PluginIds.begin(), route.PluginIds.end());
		route.PluginIds.erase(std::unique(route.PluginIds.begin(), route.PluginIds.end()), route.PluginIds.end());
		if (!route.Explicit) route.PluginIds.clear();
		if (route.PluginIds.size() > MaxRackPlugins) { _rackDiagnostic = "Each channel supports at most 32 destinations."; return false; }
		for (const auto& id : route.PluginIds)
		{
			if (id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(id)) { _rackDiagnostic = "Invalid plugin identity; remove or repair the cable."; return false; }
			const auto& oldIds = _liveVstChannelRoutes[channel].PluginIds;
			if (std::find(oldIds.begin(), oldIds.end(), id) != oldIds.end()) continue;
			const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) { return slot.Identity == id; });
			if (found == slots.end() || !found->Plugin) { _rackDiagnostic = "Destination is unavailable; retry its load first."; return false; }
			const auto capability = found->Plugin->MidiInput();
			if (!capability.Known || (!capability.Notes && !capability.Controllers)) { _rackDiagnostic = "This plugin has no supported MIDI input."; return false; }
		}
	}
	_liveVstChannelRoutes = std::move(routes);
	++_rackRevision;
	_PublishAudioState();
	_changesMade = true;
	if (!_rackInactive && !_rackLatencySuspended) _rackDiagnostic.clear();
	return true;
}

std::size_t Station::RackSlotCount() const
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	return _vstSavedEntries.size();
}

std::string Station::RackDiagnostic() const { return _rackDiagnostic; }

bool Station::_RackLatencyFeasible(const vst::VstChain& chain) const noexcept
{
	if (!chain.FitsCompensationCeiling()) return false;
	const auto stationDelay = static_cast<unsigned int>(std::max(0, chain.GetLatencySamples()));
	for (const auto& take : GetLoopTakes())
		if (take && static_cast<std::uint64_t>(stationDelay) + take->RecordedPlaybackLatencySamps() > 262144u) return false;
	return true;
}

void Station::RefreshRackLatency()
{
	if (_rackInactive) return;
	const auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain) return;
	const bool feasible = _RackLatencyFeasible(*chain);
	if (!feasible && !_rackLatencySuspended)
	{
		_rackLatencySuspended = true;
		_rackDiagnostic = "Rack paused: plugin latency exceeds 262144 samples. Reduce latency to resume; state and cables are preserved.";
		++_rackRevision;
		_PublishAudioState();
	}
	else if (feasible && _rackLatencySuspended)
	{
		const auto state = _AudioStateSnapshot();
		if (HasUnacknowledgedAudioState() || (state && !state->RetiredMidiDestinations.empty())) return;
		// A lost-source receipt may remain in synthetic ingress after destination
		// cleanup. Its final borrower must acknowledge before processor reuse.
		for (const auto& binding : _midiPlaybackBindings)
			if (!binding->RoutingActive.load(std::memory_order_acquire)
				&& !binding->ReleaseConsumed.load(std::memory_order_acquire)) return;
		_rackLatencySuspended = false;
		_rackDiagnostic.clear();
		++_rackRevision;
		_PublishAudioState();
	}
}

bool Station::RemoveInactiveRackSlot(std::size_t ordinal, std::uint64_t revision)
{
	if (!_rackInactive || revision != _rackRevision) return false;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	if (ordinal >= _vstSavedEntries.size()) return false;
	_vstSavedEntries.erase(_vstSavedEntries.begin() + ordinal);
	_vstPluginPaths.erase(_vstPluginPaths.begin() + ordinal);
	if (const auto* current = _midiVstRoutes.load(std::memory_order_acquire))
	{
		auto routes = std::make_unique<MidiVstRoutingSnapshot>(*current);
		auto compact = [ordinal](std::size_t& index) {
			if (index == ordinal) index = MidiVstRoutingSnapshot::MissingPlugin;
			else if (index != MidiVstRoutingSnapshot::NoPlugin && index != MidiVstRoutingSnapshot::MissingPlugin && index > ordinal) --index;
		};
		compact(routes->LivePlugin);
		for (auto& index : routes->PluginByMidiOutput) compact(index);
		const auto* published = routes.get();
		_retainedMidiVstRoutes.push_back(std::move(routes));
		_midiVstRoutes.store(published, std::memory_order_release);
	}
	++_rackRevision;
	_changesMade = true;
	return true;
}

bool Station::ClearInactiveRackRoutes(std::uint64_t revision)
{
	if (!_rackInactive || revision != _rackRevision) return false;
	_liveVstChannelRoutes = {};
	++_rackRevision;
	_changesMade = true;
	return true;
}

bool Station::RecoverRack(std::uint64_t revision)
{
	if (!_rackInactive || revision != _rackRevision || RackSlotCount() > MaxRackPlugins) return false;
	for (const auto& route : _liveVstChannelRoutes)
	{
		if (route.PluginIds.size() > MaxRackPlugins) return false;
		for (const auto& id : route.PluginIds)
			if (id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(id)) { _rackDiagnostic = "Remove malformed cable identities before resuming."; return false; }
	}
	std::vector<io::JamFile::VstEntry> entries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		std::vector<std::string> ambiguous;
		for (const auto& entry : _vstSavedEntries)
			if (std::count_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& other) { return entry.Id == other.Id; }) > 1) ambiguous.push_back(entry.Id);
		for (auto& entry : _vstSavedEntries)
			if (entry.Id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(entry.Id) || std::find(ambiguous.begin(), ambiguous.end(), entry.Id) != ambiguous.end()) entry.Id = utils::GetGuid();
		entries = _vstSavedEntries;
	}
	_rackInactive = false;
	_rackDiagnostic.clear();
	++_rackRevision;
	for (const auto& entry : entries) LoadManagedVstSlot(utils::DecodeUtf8(entry.Path), entry.Id);
	_changesMade = true;
	_PublishAudioState();
	return true;
}

bool Station::ConvertRackToAdditive(std::uint64_t revision)
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	if (revision != _rackRevision || _rackRenderPolicy == vst::VstChain::RenderPolicy::AdditiveInstruments) return false;
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	auto replacement = std::make_shared<vst::VstChain>();
	replacement->SetRenderPolicy(vst::VstChain::RenderPolicy::AdditiveInstruments);
	if (chain) for (std::size_t i = 0; i < chain->NumPlugins(); ++i)
	{
		const auto plugin = chain->GetPlugin(i);
		if (plugin && plugin->Role() == vst::RenderRole::Unknown) return false;
		replacement->AddSlot(plugin);
	}
	if (!_RackLatencyFeasible(*replacement)) return false;
	_rackRenderPolicy = vst::VstChain::RenderPolicy::AdditiveInstruments;
	_backVstChain = std::move(replacement);
	_flipVstChain.store(true, std::memory_order_release);
	++_rackRevision;
	_changesMade = true;
	return true;
}
bool Station::LoadVstPluginSynchronously(const std::wstring& path,
	const std::vector<std::uint8_t>& initialState,
	bool bypass, const std::string& stateId)
{
	// Startup construction completes before Scene::InitAudio publishes this station.
	if (_rackInactive || _vstSavedEntries.size() >= MaxRackPlugins || (!stateId.empty() && (!io::NativeMidiSidecar::ValidatePluginIdentity(stateId)
		|| std::any_of(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == stateId; }))))
	{
		_rackDiagnostic = "Rack limit or invalid identity; saved data requires explicit recovery.";
		return false;
	}
	auto plugin = vst::MakePluginForPath(path);
	auto hostChannels = NumBusChannels();
	if (hostChannels == 0u)
		hostChannels = 1u;
	bool loaded = plugin->PreInit(path)
		&& plugin->Load(path, _sampleRate, _blockSize, hostChannels, vst::HostedLayoutMode::Exact)
		&& (_rackRenderPolicy == vst::VstChain::RenderPolicy::LegacySerial
			|| plugin->Role() != vst::RenderRole::Unknown)
		&& plugin->GetLatencySamples() <= 262144;

	plugin->SetStateId(stateId);
	if (loaded && !initialState.empty())
		plugin->SetState(initialState);
	plugin->SetBypassed(bypass);
	const auto identity = plugin->StateId();
	if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }

	auto chain = _vstChain.load(std::memory_order_acquire);
	auto replacement = std::make_shared<vst::VstChain>();
	replacement->SetRenderPolicy(_rackRenderPolicy);
	if (chain)
	{
		for (std::size_t index = 0u; index < chain->NumPlugins(); ++index)
			replacement->AddSlot(chain->GetPlugin(index));
	}
	replacement->AddSlot(std::move(plugin));
	if (!_RackLatencyFeasible(*replacement))
	{
		loaded = false;
		vst::QueueForUiThreadDestroy(replacement->GetPlugin(replacement->NumPlugins() - 1u));
		replacement->ReplaceSlot(replacement->NumPlugins() - 1u, nullptr);
	}
	_vstChain.store(std::move(replacement), std::memory_order_release);
	_PublishAudioState();
	// Startup still exclusively owns this station.
	_vstPluginPaths.push_back(path);
	io::JamFile::VstEntry saved;
	saved.Path = utils::EncodeUtf8(path);
	saved.Id = identity;
	saved.Bypass = bypass;
	saved.State = io::JamFile::VstEntry::EncodeState(initialState);
	_vstSavedEntries.push_back(std::move(saved));
	return loaded;
}

void Station::UnloadVstPlugin(size_t index)
{
	JobAction job;
	job.JobActionType = JobAction::JOB_UNLOADVST;
	job.VstRequestId = utils::GetGuid();
	job.VstIndex = index;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (index < _vstSavedEntries.size()) job.VstIdentity = _vstSavedEntries[index].Id;
	}
	_pendingVstUnloads.push_back(std::move(job));
	_vstJobsPending.fetch_add(1u, std::memory_order_release);
	_changesMade = true;
}

std::shared_ptr<vst::IVstPlugin> Station::GetVstPlugin(size_t index) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain)
		return nullptr;

	return chain->GetPlugin(index);
}

std::vector<io::JamFile::VstEntry> Station::VstEntries() const
{
	std::vector<io::JamFile::VstEntry> entries;

	auto chain = _vstChain.load(std::memory_order_acquire);
	std::vector<std::wstring> paths;
	std::vector<io::JamFile::VstEntry> savedEntries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
		savedEntries = _vstSavedEntries;
		chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	}
	// Plugin state calls may pump native messages and reenter this owner.
	entries.reserve(paths.size());

	for (size_t i = 0; i < paths.size(); ++i)
	{
		io::JamFile::VstEntry entry = savedEntries[i];

		if (chain && i < chain->NumPlugins())
		{
			auto plugin = chain->GetPlugin(i);
			if (plugin)
			{
				entry.Id = plugin->StateId();
				entry.Bypass = plugin->IsBypassed();
				auto blob = plugin->GetState();
				if (!blob.empty())
					entry.State = io::JamFile::VstEntry::EncodeState(blob);
			}
		}

		entries.push_back(std::move(entry));
	}

	return entries;
}


bool Station::TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	std::vector<std::wstring> paths;
	std::vector<io::JamFile::VstEntry> savedEntries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
		savedEntries = _vstSavedEntries;
		chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	}
	// Plugin state calls may pump native messages and reenter this owner.
	std::vector<io::JamFile::VstEntry> entries;
	if (chain)
	{
		if (chain->NumPlugins() != paths.size()) return false;
		entries.reserve(chain->NumPlugins());
		for (size_t i = 0; i < chain->NumPlugins(); ++i)
		{
			auto plugin = chain->GetPlugin(i);
			if (!plugin) { entries.push_back(savedEntries[i]); continue; }
			std::vector<std::uint8_t> blob;
			if (!plugin->TryCaptureLiveState(blob)) return false;
			io::JamFile::VstEntry entry;
			entry.Id = plugin->StateId();
			entry.Path = utils::EncodeUtf8(paths[i]);
			entry.Bypass = plugin->IsBypassed();
			entry.State = io::JamFile::VstEntry::EncodeState(blob);
			entries.push_back(std::move(entry));
		}
	}
	output = std::move(entries);
	return true;
}

bool Station::TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry)
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain || entry.Id.empty()) return true;
	for (size_t i = 0; i < chain->NumPlugins(); ++i)
	{
		auto plugin = chain->GetPlugin(i);
		if (!plugin || plugin->StateId() != entry.Id) continue;
		const auto blob = entry.DecodeState();
		if (!entry.State.empty() && blob.empty()) return true;
		if (!plugin->TryRestoreLiveState(blob)) return false;
		plugin->SetBypassed(entry.Bypass);
		return true;
	}
	return true; // Deleted plugins never match a replacement's position.
}

ActionResult Station::OnAction(JobAction action)
{
	if (MembershipRemoving()) return ActionResult::NoAction();
	switch (action.JobActionType)
	{
	case JobAction::JOB_LOADVST:
	{
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto state = _AudioStateSnapshot();
			const bool overLimit = !action.VstManaged && (_vstSavedEntries.size() >= MaxRackPlugins || (state && _vstSavedEntries.size() + state->RetiredMidiDestinations.size() >= MaxRackDestinations)
				|| (!action.VstIdentity.empty() && std::any_of(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; })));
			const bool cancelled = action.VstManaged && std::find(_pendingRackIds.begin(), _pendingRackIds.end(), action.VstIdentity) == _pendingRackIds.end();
			if (_rackInactive || overLimit || cancelled || (!action.VstIdentity.empty() && !io::NativeMidiSidecar::ValidatePluginIdentity(action.VstIdentity)))
			{
				if (_vstJobsPending.load(std::memory_order_acquire)) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
				_rackDiagnostic = cancelled ? "" : "Rack capacity or identity rejected; remove a slot and retry.";
				return ActionResult::NoAction();
			}
		}
		auto plugin = action.PreInitPlugin ? action.PreInitPlugin : vst::MakePluginForPath(action.VstPath);
		auto hostChannels = NumBusChannels();
		if (hostChannels == 0u) hostChannels = 1u;
		bool loaded = plugin->Load(action.VstPath, _sampleRate, _blockSize, hostChannels, vst::HostedLayoutMode::Exact)
			&& (_rackRenderPolicy == vst::VstChain::RenderPolicy::LegacySerial
				|| plugin->Role() != vst::RenderRole::Unknown);
		if (action.VstIdentity.empty()) action.VstIdentity = plugin->StateId();
		plugin->SetStateId(action.VstIdentity);
		if (loaded && !action.VstInitialState.empty()) plugin->SetState(action.VstInitialState);
		if (action.VstManaged)
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; });
			if (found != _vstSavedEntries.end()) plugin->SetBypassed(found->Bypass);
		}
		loaded = loaded && plugin->GetLatencySamples() <= 262144;
		if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			// Multiple serialized jobs can complete before the UI publishes. Compose
			// against the latest delivered chain so no earlier edit is discarded.
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			auto replacement = std::make_shared<vst::VstChain>();
			replacement->SetRenderPolicy(_rackRenderPolicy);
			if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i) replacement->AddSlot(chain->GetPlugin(i));
			io::JamFile::VstEntry saved;
			saved.Id = action.VstIdentity;
			saved.Path = utils::EncodeUtf8(action.VstPath);
			saved.State = io::JamFile::VstEntry::EncodeState(action.VstInitialState);
			if (action.VstManaged)
			{
				const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; });
				const bool cancelled = std::find(_pendingRackIds.begin(), _pendingRackIds.end(), action.VstIdentity) == _pendingRackIds.end();
				if (found == _vstSavedEntries.end() || cancelled)
				{
					if (plugin) vst::QueueForUiThreadDestroy(std::move(plugin));
					if (_vstJobsPending.load(std::memory_order_acquire)) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
					return ActionResult::NoAction();
				}
				const auto index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
				while (replacement->NumPlugins() <= index) replacement->AddSlot(nullptr);
				replacement->ReplaceSlot(index, std::move(plugin));
				if (!_RackLatencyFeasible(*replacement))
				{
					loaded = false;
					vst::QueueForUiThreadDestroy(replacement->GetPlugin(index));
					replacement->ReplaceSlot(index, nullptr);
				}
				_vstPluginPaths[index] = action.VstPath;
				saved.Bypass = found->Bypass;
				// A failed retry must keep the original opaque state for recovery.
				if (!loaded) saved.State = found->State;
				*found = std::move(saved);
				std::erase(_pendingRackIds, action.VstIdentity);
			}
			else
			{
				replacement->AddSlot(std::move(plugin));
				_vstPluginPaths.push_back(action.VstPath);
				_vstSavedEntries.push_back(std::move(saved));
				if (!_RackLatencyFeasible(*replacement))
				{
					loaded = false;
					vst::QueueForUiThreadDestroy(replacement->GetPlugin(replacement->NumPlugins() - 1u));
					replacement->ReplaceSlot(replacement->NumPlugins() - 1u, nullptr);
				}
			}
			if (!loaded) _rackDiagnostic = "Plugin could not be activated: check supported role, layout and latency (262144 samples maximum). Saved state is retained.";
			_backVstChain = std::move(replacement);
			++_rackRevision;
			_flipVstChain.store(true, std::memory_order_release);
		}
		if (_vstJobsPending.load(std::memory_order_acquire) != 0u) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
		_changesMade = true;
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	case JobAction::JOB_UNLOADVST:
	{
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			// Identity is captured when requested. An index alone can drift while queued.
			auto removeIndex = _vstSavedEntries.size();
			for (size_t i = 0u; i < _vstSavedEntries.size(); ++i)
				if (!action.VstIdentity.empty() && _vstSavedEntries[i].Id == action.VstIdentity) { removeIndex = i; break; }
			if (removeIndex < _vstSavedEntries.size())
			{
				auto replacement = std::make_shared<vst::VstChain>();
	replacement->SetRenderPolicy(_rackRenderPolicy);
				if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i)
				{
					auto existing = chain->GetPlugin(i);
					if (i == removeIndex) { if (existing) vst::QueueForUiThreadDestroy(std::move(existing)); }
					else replacement->AddSlot(std::move(existing));
				}
				_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				_vstSavedEntries.erase(_vstSavedEntries.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				if (const auto* current = _midiVstRoutes.load(std::memory_order_acquire))
				{
					auto routes = std::make_unique<MidiVstRoutingSnapshot>(*current);
					auto compact = [removeIndex](std::size_t& index) {
						if (index == removeIndex) index = MidiVstRoutingSnapshot::MissingPlugin;
						else if (index != MidiVstRoutingSnapshot::NoPlugin && index != MidiVstRoutingSnapshot::MissingPlugin && index > removeIndex) --index;
					};
					compact(routes->LivePlugin);
					for (auto& index : routes->PluginByMidiOutput) compact(index);
					const auto* published = routes.get();
					_retainedMidiVstRoutes.push_back(std::move(routes));
					_midiVstRoutes.store(published, std::memory_order_release);
				}
				++_rackRevision;
				_backVstChain = std::move(replacement);
				_flipVstChain.store(true, std::memory_order_release);
			}
		}
		if (_vstJobsPending.load(std::memory_order_acquire) != 0u) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
		_changesMade = true;
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	default:
		break;
	}

	return ActionResult::NoAction();
}

void engine::Station::BeginMembershipRemoval()
{
	SetMembershipRemoving(true);
	if (const auto request = _firstMasterRecording.load())
		request->Clock->CancelRecordingSeedOnOwner(request->Timing.Version);
	for (const auto& take : GetLoopTakeSnapshot()) take->CancelCapture();
}
