#include "Station.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include "../midi/MidiRouter.h"

using namespace engine;
using namespace timing;
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
	std::optional<audio::AudioStreamParams> params)
{
	if (!clock)
		return;

	auto policyCfg = cfg.value_or(io::UserConfig());
	const auto sampleRate = _ResolveSampleRate(cfg, params);
	if (auto timing = policyCfg.DeduceLoopTiming(loopLengthSamps, sampleRate); timing.has_value())
	{
		const auto quantisation = policyCfg.Loop.SeedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE;
		clock->SetQuantisation(timing->GrainSamps, quantisation);
		clock->SetSeedSourceLength(loopLengthSamps);
		std::cout << "Seeded clock from first loop: grain=" << timing->GrainSamps
			<< " mode=" << (policyCfg.Loop.SeedUsesPowers ? "power" : "multiple")
			<< " loopGrains=" << timing->LoopGrains
			<< " bpm=" << timing->Bpm
			<< " bpi=" << timing->Bpi << std::endl;
	}
}

Station::Station(StationParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	_flipTakeBuffer(false),
	_flipAudioBuffer(false),
	_name(params.Name),
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
	_triggers(),
	_backLoopTakes(),
	_audioMixers(),
	_backAudioMixers(),
	_audioBuffers(),
	_backAudioBuffers(),
	_audioState(nullptr),
	_vstChain(nullptr),
	_backVstChain(nullptr),
	_flipVstChain(false),
	_pendingVstLoads(),
	_pendingVstUnloads(),
	_vstPathsMutex(),
	_vstPluginPaths(),
	_liveMidiIngress(),
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
	stationParams.Name = stationStruct.Name;
	auto station = std::make_shared<Station>(stationParams, mixerParams);
	station->SetStationPhaseOffsetSamps(stationStruct.StationPhaseOffsetSamps);

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
		auto take = LoopTake::FromFile(takeParams, takeStruct, dir);
		
		if (take.has_value())
			station->AddTake(take.value());

		takeCount++;
	}

	// Queue load jobs for any VST plugins serialised in the station's chain.
	for (const auto& vstEntry : stationStruct.VstChain)
		station->LoadVstPlugin(utils::DecodeUtf8(vstEntry.Path), vstEntry.DecodeState());

	return station;
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

void Station::Draw3d(base::DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (!_isVisible)
		return;

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	auto pos = ModelPosition();
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
		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(0.0f, _StationModelYOffset, 0.01f)));
		const auto stationPeak = _masterMixer ? _masterMixer->VuPeakLevel() : 0.0f;
		_stationModel->SetStationState(GlobalId(), IsSelected(), _isPicking3d, stationPeak);
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
	auto state = _AudioStateSnapshot();
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

	for (const auto& weakTake : state->LoopTakes)
		if (auto take = weakTake.lock()) take->Zero(numSamps, source);
}

// Only called when outputting to DAC
void Station::WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps,
	std::uint32_t blockStartSample)
{
	auto ptr = Sharable::shared_from_this();
	auto state = _AudioStateSnapshot();
	if (!state)
		return;
	auto stationSink = std::dynamic_pointer_cast<MultiAudioSink>(ptr);
	for (const auto& weakTake : state->LoopTakes)
	{
		auto take = weakTake.lock();
		if (!take)
			continue;

		take->WriteBlock(stationSink, trigger, indexOffset, numSamps);
	}

	auto sampsToRead = (numSamps <= constants::MaxBlockSize) ? numSamps : constants::MaxBlockSize;
	auto masterLevel = static_cast<float>(_masterMixer->Level());
	auto masterPeak = 0.0f;
	const auto channelCount = (state->AudioBuffers.size() < state->AudioMixers.size()) ? state->AudioBuffers.size() : state->AudioMixers.size();

	_PrepareVstScratch(*state, sampsToRead);

	auto chain = _vstChain.load(std::memory_order_acquire);
	const auto* routes = _midiVstRoutes.load(std::memory_order_acquire);
	const bool vstActive = (channelCount > 0u) && chain && chain->IsActive() && (state->VstBlockPtrs.size() >= channelCount);

	_RunVstBlock(chain.get(), routes, *state, vstActive,
		static_cast<unsigned int>(channelCount), sampsToRead, blockStartSample);

	// Drive any wired parameter automation. Runs independently of vstActive so a
	// bypassed/idle chain still receives recorded parameter motion. Flat loop over
	// the pre-baked dispatch list — no weak_ptr locks, no shared_ptr chasing.
	_RunAutomationDispatch(blockStartSample);

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

void Station::_PrepareVstScratch(const AudioState& state, unsigned int sampsToRead) noexcept
{
	const auto channelCount = std::min(state.AudioBuffers.size(), state.AudioMixers.size());
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
	const MidiVstRoutingSnapshot* routes,
	const AudioState& state,
	bool vstActive,
	unsigned int channelCount,
	unsigned int sampsToRead,
	std::uint32_t blockStartSample) noexcept
{
	if (vstActive)
	{
		vst::HostTimeState hostTime;
		hostTime.sampleRate = static_cast<double>(_sampleRate);
		hostTime.isPlaying  = true;
		hostTime.samplePos  = _clock
			? static_cast<double>(_clock->AbsoluteSamplePos(blockStartSample))
			: static_cast<double>(blockStartSample);
		const auto seedSamps   = _clock ? _clock->QuantiseSamps() : 0u;
		const auto masterSamps = _clock ? _clock->SeedSourceLength() : 0ul;
		if (seedSamps > 0u && _sampleRate > 0.0f)
			if (const auto timing = TimingQuantiser::TimingFromSeedAndMaster(
					seedSamps, masterSamps, static_cast<unsigned int>(_sampleRate)))
			{
				hostTime.tempo = static_cast<double>(timing->Bpm);
				hostTime.bpi   = static_cast<int32_t>(timing->Bpi);
			}
		chain->UpdateHostTime(hostTime);
		chain->BeginMidiBlock(blockStartSample, sampsToRead);
	}

	// Always drain live MIDI to avoid backlogging stale events when no instrument is active.
	MidiEvent liveMidi{};
	while (_liveMidiIngress.Pop(liveMidi))
	{
		if (vstActive)
			midi::SendMidiToVstChain(chain, routes, liveMidi, true, LiveMidiOutputIndex);
	}

	if (!vstActive)
		return;

	auto midiOutputIndex = 0u;
	midi::MidiVstOutputSink midiSink(chain, routes);
	for (const auto& weakTake : state.LoopTakes)
	{
		auto take = weakTake.lock();
		if (!take)
			continue;
		midiOutputIndex += take->ReadMidiBlock(blockStartSample, sampsToRead, midiSink, midiOutputIndex);
	}

	chain->ProcessBlockMulti(state.VstBlockPtrs.data(), static_cast<int>(channelCount), sampsToRead);
}

void Station::RebuildAutomationDispatch()
{
	// Non-audio thread only. Walk every take and MIDI loop, resolve all raw
	// pointers and lane metadata into a compact flat list, then publish it.
	const std::uint8_t back = _automationDispatchBack;
	auto* buf = _automationDispatchBuf[back];
	std::uint8_t count = 0u;

	const auto& takes = GetLoopTakes();
	for (const auto& take : takes)
	{
		if (!take)
			continue;

		for (const auto& midiLoop : take->GetMidiLoops())
		{
			if (!midiLoop)
				continue;

			for (std::size_t laneIdx = 0u; laneIdx < midi::MidiLoop::MaxAutomationLanes; ++laneIdx)
			{
				if (count >= MaxAutomationDispatches)
					break;

				const auto& lane = midiLoop->GetLane(laneIdx);
				if (!lane.Mapping.IsActive() || !lane.Mapping.TargetPlugin)
					continue;

				auto& entry = buf[count];
				entry.plugin = lane.Mapping.TargetPlugin;
				entry.paramIdx = lane.Mapping.TargetParameterIndex;
				entry.loop = midiLoop.get();
				entry.laneIdx = static_cast<std::uint8_t>(laneIdx);
				entry.loopLengthSamps = midiLoop->LoopLengthSamps();
				entry.cursorIdx = 0u;
				entry.lastValue = -2.0f; // force first write after a rebuild
				++count;
			}
		}
	}

	_automationDispatchCount[back] = count;
	// Release pairs with the audio thread's acquire load: makes all MIDI-thread
	// writes to lane Points visible before the new list is consumed.
	_automationDispatch.store(buf, std::memory_order_release);
	_automationDispatchBack ^= 1u;
}

void Station::_RunAutomationDispatch(std::uint32_t blockStartSample) noexcept
{
	auto* dispatches = _automationDispatch.load(std::memory_order_acquire);
	if (!dispatches)
		return;

	// acquire above pairs with the release store in RebuildAutomationDispatch.
	const bool recordHeld = midi::AutomationRecordHeld.load(std::memory_order_acquire);
	const auto* recordTarget = midi::RecordTargetLoop.load(std::memory_order_relaxed);
	const std::uint8_t frontIdx = (dispatches == _automationDispatchBuf[0]) ? 0u : 1u;
	const auto count = _automationDispatchCount[frontIdx];

	for (std::uint8_t i = 0u; i < count; ++i)
	{
		auto& entry = dispatches[i];
		if (!entry.plugin || !entry.loop)
			continue;

		// Suppress playback only for lanes on the loop actively being recorded;
		// other loops' lanes keep playing back unaffected.
		if (recordHeld && entry.loop == recordTarget)
			continue;

		const double frac = (entry.loopLengthSamps > 0u)
			? std::fmod(static_cast<double>(blockStartSample), static_cast<double>(entry.loopLengthSamps))
				/ static_cast<double>(entry.loopLengthSamps)
			: 0.0;

		const float val = entry.loop->GetAutomationValueAtCursor(entry.laneIdx, frac, entry.cursorIdx);

		if (std::abs(val - entry.lastValue) > AutomationEpsilon)
		{
			entry.plugin->SetParameter(entry.paramIdx, val);
			entry.lastValue = val;
		}
	}
}

void Station::EndMultiPlay(unsigned int numSamps)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (const auto& weakTake : state->LoopTakes)
		if (auto take = weakTake.lock()) take->EndMultiPlay(numSamps);

	for (auto& buffer : state->AudioBuffers)
	{
		buffer->EndWrite(numSamps, true);
		buffer->EndPlay(numSamps);
	}
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
			auto state = _AudioStateSnapshot();
			if (!state)
				break;

			for (const auto& weakTake : state->LoopTakes)
				if (auto take = weakTake.lock()) take->OnBlockWriteChannel(channel, request, writeOffset);
		}
		break;
	}
}

void Station::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (const auto& weakTake : state->LoopTakes)
		if (auto take = weakTake.lock()) take->EndMultiWrite(numSamps, updateIndex, source);
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
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	auto state = action.KeyActionType == KeyAction::KEY_DOWN ? 1u : 0u;
	return OnTriggerEvent(TriggerSource::TRIGGER_KEY, action.KeyChar, state, action);
}

ActionResult Station::OnTriggerEvent(TriggerSource source,
	unsigned int value,
	unsigned int state,
	const base::Action& action,
	const std::string& device)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	auto result = ActionResult::NoAction();
	for (auto& trig : _triggers)
	{
		auto trigResult = trig->OnEvent(source, value, state, action, device);
		if (!trigResult.IsEaten)
			continue;

		if (!result.IsEaten || (trigResult.ResultType != actions::ACTIONRESULT_DEFAULT))
			result = trigResult;
	}

	return result;
}

ActionResult Station::OnAction(GuiAction action)
{
	const bool isRackAction =
		(action.ElementType == GuiAction::ACTIONELEMENT_RACK) ||
		(action.ElementType == GuiAction::ACTIONELEMENT_ROUTER) ||
		(action.ElementType == GuiAction::ACTIONELEMENT_TOGGLE);

	auto res = isRackAction ? ActionResult::NoAction() : GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
		return res;

	switch (action.ElementType)
	{
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
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	ActionResult res;
	res.IsEaten = false;

	auto loopTake = _TryGetTake(action.TargetId);

	switch (action.ActionType)
	{
	case TriggerAction::TRIGGER_REC_START:
	{
		std::vector<std::pair<std::string, MidiNoteSnapshot>> heldSnapshot;
		if (!action.MidiInputChannels.empty())
		{
			std::scoped_lock lock(_liveHeldMidiMutex);
			heldSnapshot = _liveHeldMidi;
		}
		auto newLoopTake = AddTake();
		const auto transportStartSamps = _clock ? _clock->AbsoluteSamplePos() : 0ul;
		newLoopTake->Record(action.InputChannels,
			Name(),
			action.MidiInputChannels,
			action.MidiInputDevices,
			std::move(heldSnapshot),
			static_cast<std::uint64_t>(transportStartSamps));

		res.SourceId = "";
		res.TargetId = newLoopTake->Id();
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		break;
	}
	case TriggerAction::TRIGGER_REC_END:
	{
		auto loopLength = action.SampleCount;

		if (0 == loopLength)
		{
			if (loopTake.has_value())
				_DitchLoopTake(loopTake.value());

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
				if (_clock->IsQuantisable())
				{
					auto [quantisedLength, err] = _clock->QuantiseLength(action.SampleCount);
					loopLength = quantisedLength;
					errorSamps = err;
					std::cout << "Quantised loop to " << loopLength << " with error " << errorSamps << std::endl;
				}
				else
				{
						_TrySeedClockFromFirstLoop(_clock, action.SampleCount, cfg, streamParams);
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
				cfg.value().OverdubPlayPos(errorSamps, loopLength) :
				0;
			auto endRecordSamps = cfg.has_value() ?
				cfg.value().EndRecordingSamps(errorSamps) :
				0;

			std::cout << "Playing loop from " << playPos << " with loop length " << loopLength << " (out latency = " << outLatency << ")" << std::endl;

			if (loopTake.has_value())
				loopTake.value()->Play(playPos, loopLength, endRecordSamps, errorSamps);

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		}
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_START:
	{
		auto sourceLoopTake = _loopTakes.empty() ? std::shared_ptr<LoopTake>() : _loopTakes.back();
		auto sourceId = sourceLoopTake ? sourceLoopTake->Id() : "";

		auto newLoopTake = AddTake();
		const auto transportStartSamps = _clock ? _clock->AbsoluteSamplePos() : 0ul;
		newLoopTake->Overdub(action.InputChannels,
			Name(),
			action.MidiInputChannels,
			action.MidiInputDevices,
			sourceLoopTake,
			static_cast<std::uint64_t>(transportStartSamps));

		res.SourceId = sourceId;
		res.TargetId = newLoopTake->Id();
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_END:
	{
		auto loopLength = action.SampleCount;

		if (0 == loopLength)
		{
			if (loopTake.has_value())
				_DitchLoopTake(loopTake.value());

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
				if (_clock->IsQuantisable())
				{
					auto [quantisedLength, err] = _clock->QuantiseLength(action.SampleCount);
					loopLength = quantisedLength;
					errorSamps = err;
					std::cout << "Quantised loop to " << loopLength << " with error " << errorSamps << std::endl;
				}
				else
				{
					_TrySeedClockFromFirstLoop(_clock, action.SampleCount, cfg, streamParams);
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
				cfg.value().LoopPlayPos(errorSamps, loopLength, outLatency) :
				0;
			auto endRecordSamps = cfg.has_value() ?
				cfg.value().EndRecordingSamps(errorSamps) :
				0;

			std::cout << "Playing loop from " << playPos << " with loop length " << loopLength << " (out latency = " << outLatency << ")" << std::endl;

			if (loopTake.has_value())
				loopTake.value()->Play(playPos, loopLength, endRecordSamps, errorSamps);

			auto sourceLoopTake = _TryGetTake(action.SourceId);
			if (sourceLoopTake.has_value())
				sourceLoopTake.value()->Mute();

			res.IsEaten = true;
			res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		}
		break;
	}
	case TriggerAction::TRIGGER_PUNCHIN_START:
		if (action.ApplyToTargetTake && action.ApplyToTargetMidi && loopTake.has_value())
		{
			for (const auto& event : loopTake.value()->BuildMidiPunchInLiveTransitionEvents(static_cast<std::uint32_t>(action.SampleCount)))
				EnqueueLiveMidiEvent(event);
		}

		if (action.ApplyToTargetTake && loopTake.has_value())
			loopTake.value()->PunchIn(action.ApplyToTargetAudio, action.ApplyToTargetMidi);

		if (action.ApplyToSourceTake)
		{
			if (auto sourceLoopTake = _TryGetTake(action.SourceId); sourceLoopTake.has_value())
				sourceLoopTake.value()->Mute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		break;
	case TriggerAction::TRIGGER_PUNCHIN_END:
		if (action.ApplyToTargetTake && action.ApplyToTargetMidi && loopTake.has_value())
		{
			for (const auto& event : loopTake.value()->BuildMidiPunchOutLiveTransitionEvents(static_cast<std::uint32_t>(action.SampleCount)))
				EnqueueLiveMidiEvent(event);
		}

		if (action.ApplyToTargetTake && loopTake.has_value())
			loopTake.value()->PunchOut(action.ApplyToTargetAudio, action.ApplyToTargetMidi);

		if (action.ApplyToSourceTake)
		{
			if (auto sourceLoopTake = _TryGetTake(action.SourceId); sourceLoopTake.has_value())
				sourceLoopTake.value()->UnMute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		break;
	case TriggerAction::TRIGGER_DITCH:
		if (loopTake.has_value())
		{
			_DitchLoopTake(loopTake.value());

			auto id = loopTake.value()->Id();
			auto match = std::find_if(_backLoopTakes.begin(),
				_backLoopTakes.end(),
				[&id](const std::shared_ptr<LoopTake>& arg) { return arg->Id() == id; });

			if (match != _backLoopTakes.end())
			{
				_backLoopTakes.erase(match);
				_ArrangeChildren();
				_flipTakeBuffer = true;
				_changesMade = true;
			}
		}

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

	return res;
}

void Station::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	for (auto& trig : _triggers)
	{
		trig->OnTick(curTime, samps, cfg, params);
	}
}

void Station::Reset()
{
	Jammable::Reset();
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		_liveHeldMidi.clear();
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

	for (auto& trigger : _triggers)
	{
		auto child = std::find(_children.begin(), _children.end(), trigger);
		if (_children.end() != child)
			_children.erase(child);
	}
	_triggers.clear();
}

std::shared_ptr<LoopTake> Station::AddTake()
{
	LoopTakeParams takeParams;
	takeParams.Id = _name + "-TK-" + utils::GetGuid();
	takeParams.Size = { 100,100 };
	takeParams.FadeSamps = _fadeSamps;

	MergeMixBehaviourParams mergeParams;
	auto mixerParams = LoopTake::GetMixerParams(takeParams.Size, mergeParams);
	auto take = std::make_shared<LoopTake>(takeParams, mixerParams);
	AddTake(take);

	return take;
}

void Station::AddTake(std::shared_ptr<LoopTake> take)
{
	take->SetupBuffers(_lastBufSize);
	take->SetNumBusChannels(NumBusChannels());
	take->SetSelectDepth(CurrentSelectDepth());
	take->SetLogging(_loggingConfig);
	take->SetReceiver(ActionReceiver::shared_from_this());
	_backLoopTakes.push_back(take);
	_ApplyMidiQuantisationPhaseOffset();
	_ArrangeChildren();
	_flipTakeBuffer = true;
	_changesMade = true;
}

void Station::AddTrigger(std::shared_ptr<Trigger> trigger)
{
	trigger->SetReceiver(ActionReceiver::shared_from_this());

	_triggers.push_back(trigger);
	_children.push_back(trigger);
}

unsigned int Station::NumTakes() const
{
	return _changesMade ?
		(unsigned int)_backLoopTakes.size() :
		(unsigned int)_loopTakes.size();
}

std::string Station::Name() const
{
	return _name;
}

void Station::SetName(std::string name)
{
	_name = name;
}

void Station::SetClock(std::shared_ptr<utils::Timer> clock)
{
	_clock = clock;
}

void Station::SetQuantisationParams(std::optional<timing::QuantisationParams> params,
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

	_pendingQuantisationParams = timing::QuantisationParams{
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

	auto& buffers = (_flipAudioBuffer && _changesMade) ?
		_backAudioBuffers :
		_audioBuffers;

	for (auto& buf : buffers)
	{
		buf->SetSize(bufSize);
	}

	for (auto& take : _loopTakes)
		take->SetupBuffers(bufSize);
	for (auto& take : _backLoopTakes)
		take->SetupBuffers(bufSize);
}

void Station::SetSampleRate(float sampleRate)
{
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
	auto& mixers = _flipAudioBuffer && _changesMade ?
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
	return (_changesMade && _flipAudioBuffer) ?
		(unsigned int)_backAudioBuffers.size() :
		(unsigned int)_audioBuffers.size();
}

void Station::OnBounce(unsigned int numSamps,
	io::UserConfig config,
	std::optional<audio::AudioStreamParams> params)
{
	auto outLatency = params.has_value() ? params.value().OutputLatency : 0u;
	if (0u == outLatency)
		outLatency = config.Audio.LatencyOut;

	auto sourceOffset = config.OverdubSourceReadOffset(outLatency);
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (auto& trigger : _triggers)
	{
		auto takes = trigger->GetTakes();

		for (auto& take : takes)
		{
			std::string sourceId = take.SourceTakeId;
			std::string targetId = take.TargetTakeId;
			auto sourceMatch = std::find_if(state->LoopTakes.begin(),
				state->LoopTakes.end(),
				[&sourceId](const std::weak_ptr<LoopTake>& arg) { auto t = arg.lock(); return t && t->Id() == sourceId; });
			auto targetMatch = std::find_if(state->LoopTakes.begin(),
				state->LoopTakes.end(),
				[&targetId](const std::weak_ptr<LoopTake>& arg) { auto t = arg.lock(); return t && t->Id() == targetId; });

			if ((state->LoopTakes.end() != sourceMatch) && (state->LoopTakes.end() != targetMatch))
			{
				auto sourceTake = sourceMatch->lock();
				auto targetTake = targetMatch->lock();
				if (sourceTake && targetTake)
					sourceTake->WriteBlock(targetTake, trigger, sourceOffset, numSamps);
			}
		}
	}
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

bool Station::AcceptsLiveMidiFromDevice(const std::string& deviceName) const noexcept
{
	for (const auto& trigger : _triggers)
	{
		if (!trigger)
			continue;
		const auto& devices = trigger->MidiInputDevices();
		if (devices.empty())
			return true;
		for (const auto& d : devices)
		{
			if (d == deviceName)
				return true;
		}
	}
	// No trigger has a device restriction — allow all.
	return _triggers.empty();
}

void Station::EnqueueLiveMidiEvent(const MidiEvent& event)
{
	// Synthetic events, like punch-in transitions, do not have a device name.
	EnqueueLiveMidiEvent(event, "");
}

void Station::EnqueueLiveMidiEvent(const MidiEvent& event, const std::string& deviceName)
{
	if (event.IsNoteOn() || event.IsNoteOff())
	{
		const auto channel = event.Channel();
		const auto note = static_cast<std::uint8_t>(event.data1 & 0x7F);
		std::scoped_lock lock(_liveHeldMidiMutex);
		// Keep one snapshot for "any device" and one snapshot for the actual device.
		// The empty-name snapshot is for device-agnostic recording; the named one is
		// for real MIDI input from a specific device.
		auto upsert = [&](const std::string& key) {
			auto it = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
				[&key](const auto& p) { return p.first == key; });
			if (it == _liveHeldMidi.end())
			{
				_liveHeldMidi.push_back({ key, MidiNoteSnapshot{} });
				it = std::prev(_liveHeldMidi.end());
			}
			if (event.IsNoteOn()) it->second.Set(channel, note, event.data2);
			else it->second.Clear(channel, note);
		};
		upsert("");
		if (!deviceName.empty())
			upsert(deviceName);
	}
	_liveMidiIngress.Push(event);
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
}

void Station::ClearMidiVstRoutes()
{
	auto nextRoutes = std::make_unique<MidiVstRoutingSnapshot>();
	const auto* published = nextRoutes.get();
	_retainedMidiVstRoutes.push_back(std::move(nextRoutes));
	_midiVstRoutes.store(published, std::memory_order_release);
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
	if (audioStateChanged)
	{
		_PublishAudioState();
	}

	// Swap in the VST chain if the job thread has delivered a new one.
	if (_flipVstChain.exchange(false, std::memory_order_acquire))
	{
		_vstChain.store(_backVstChain, std::memory_order_release);
	}

	// Detect pending VST load/unload requests and queue a job for them.
	std::vector<JobAction> jobs;

	// Drain pending unload indices — emit one JOB_UNLOADVST per entry so
	// rapid back-to-back UnloadVstPlugin() calls are not silently dropped.
	auto pendingUnloads = std::move(_pendingVstUnloads);
	for (auto idx : pendingUnloads)
	{
		// Queue removal to the job thread so DLL teardown does not happen
		// under the audio mutex.
		JobAction job;
		job.JobActionType = JobAction::JOB_UNLOADVST;
		job.SourceId = _name;
		job.VstIndex = idx;
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	// Drain pending load paths into jobs, then clear.  Move the vector out
	// first so the for-loop iterates a local copy (avoids moving from a
	// reference to a live vector element).
	auto pendingLoads = std::move(_pendingVstLoads);
	for (auto& [path, initialState] : pendingLoads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.SourceId = _name;
		job.VstPath = std::move(path);
		job.VstInitialState = std::move(initialState);
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}

	GuiElement::_CommitChanges();

	return jobs;
}

const std::shared_ptr<AudioSink> Station::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	auto state = _AudioStateSnapshot();
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

std::shared_ptr<const Station::AudioState> Station::_AudioStateSnapshot() const
{
	return _audioState.load(std::memory_order_acquire);
}

void Station::_PublishAudioState()
{
	auto state = std::make_shared<AudioState>();
	state->LoopTakes.reserve(_loopTakes.size());
	for (const auto& take : _loopTakes)
		state->LoopTakes.push_back(take);
	state->AudioMixers = _audioMixers;
	state->AudioBuffers = _audioBuffers;
	state->VstBlockScratch.resize(state->AudioBuffers.size() * constants::MaxBlockSize);
	state->VstBlockPtrs.resize(state->AudioBuffers.size(), nullptr);
	for (auto i = 0u; i < state->AudioBuffers.size(); i++)
		state->VstBlockPtrs[i] = state->VstBlockScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);
	_audioState.store(state, std::memory_order_release);
}

void Station::_ArrangeChildren()
{
	auto numTakes = (unsigned int)_backLoopTakes.size();

	auto takeHeight = _CalcTakeHeight(_sizeParams.Size.Height, numTakes);
	utils::Size2d takeSize = { _sizeParams.Size.Width - (2 * _Gap.Width), takeHeight - (2 * _Gap.Height) };

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
	rackParams.Position = { 0, 0 };
	rackParams.Size = size;
	rackParams.MinSize = rackParams.Size;
	rackParams.NumInputChannels = NumBusChannels();
	rackParams.NumOutputChannels = 2;
	rackParams.InitLevel = 1.0;
	rackParams.InitState = gui::GuiRackParams::RACK_MASTER;

	return rackParams;
}

std::optional<std::shared_ptr<LoopTake>> Station::_TryGetTake(std::string id)
{
	for (auto& take : _loopTakes)
	{
		if (take->Id() == id)
			return take;
	}

	return std::nullopt;
}

void Station::_CollapseOtherTakeRouters()
{
	auto& takes = (_changesMade && _flipTakeBuffer) ?
		_backLoopTakes :
		_loopTakes;

	for (auto& take : takes)
		take->CollapseRackToMaster();
}

void Station::_CollapseOtherTakeRoutersToChannels()
{
	auto& takes = (_changesMade && _flipTakeBuffer) ?
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

void Station::_DitchLoopTake(std::shared_ptr<LoopTake>& take) noexcept
{
	// Flush any held MIDI notes so the VST instrument doesn't get stuck notes.
	// Events are injected via EnqueueLiveMidiEvent (thread-safe live queue) and
	// drained by the audio thread on the next WriteBlock call.
	for (const auto& midiLoop : take->GetMidiLoops())
	{
		if (!midiLoop)
			continue;
		const auto& held = midiLoop->HeldNotes();
		if (held.none())
			continue;
		for (std::uint8_t ch = 0; ch < 16; ++ch)
		{
			for (std::uint8_t note = 0; note < 128; ++note)
			{
				if (held.test(MidiLoop::NoteSlot(ch, note)))
					EnqueueLiveMidiEvent(MidiEvent::MakeNoteOff(0u, ch, note));
			}
		}
	}
	take->Ditch();
}

void Station::LoadVstPlugin(std::wstring path,
	std::vector<std::uint8_t> initialState)
{
	_pendingVstLoads.push_back({ std::move(path), std::move(initialState) });
	_changesMade = true;
}

void Station::UnloadVstPlugin(size_t index)
{
	_pendingVstUnloads.push_back(index);
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
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	entries.reserve(_vstPluginPaths.size());

	for (size_t i = 0; i < _vstPluginPaths.size(); ++i)
	{
		io::JamFile::VstEntry entry;
		entry.Path = utils::EncodeUtf8(_vstPluginPaths[i]);
		entry.Bypass = false;

		if (chain && i < chain->NumPlugins())
		{
			auto plugin = chain->GetPlugin(i);
			if (plugin)
			{
				auto blob = plugin->GetState();
				if (!blob.empty())
					entry.State = io::JamFile::VstEntry::EncodeState(blob);
			}
		}

		entries.push_back(std::move(entry));
	}

	return entries;
}

ActionResult Station::OnAction(JobAction action)
{
	switch (action.JobActionType)
	{
	case JobAction::JOB_LOADVST:
	{
		// Running on the job thread — safe to do heavy work here.
		// Always build a brand-new VstChain so _backVstChain never aliases
		// the live _vstChain (which the audio callback reads concurrently).
		// Take a snapshot of the current live chain under _vstChainMutex so
		// we don't race with _CommitChanges() swapping it on the main thread.
		std::shared_ptr<vst::VstChain> chainSnapshot;
		chainSnapshot = _vstChain.load(std::memory_order_acquire);

		auto newChain = std::make_shared<vst::VstChain>();
		if (chainSnapshot)
		{
			for (size_t i = 0; i < chainSnapshot->NumPlugins(); ++i)
			{
				auto existing = chainSnapshot->GetPlugin(i);
				if (existing)
					newChain->AddPlugin(existing);
			}
		}

		// Use the pre-initialised plugin instance when Scene::CommitChanges()
		// prepared one on the UI thread; otherwise create a fresh plugin.
		auto plugin = action.PreInitPlugin
			? action.PreInitPlugin
			: vst::MakePluginForPath(action.VstPath);
		auto hostChannels = NumBusChannels();
		if (hostChannels == 0u)
			hostChannels = 1u;
		if (plugin->Load(action.VstPath, _sampleRate, _blockSize, hostChannels, vst::HostedLayoutMode::Exact))
		{
			// Restore saved state before adding to the chain so the plugin's
			// parameters are correct when audio processing resumes.
			if (!action.VstInitialState.empty())
				plugin->SetState(action.VstInitialState);

			newChain->AddPlugin(plugin);
			{
				std::lock_guard<std::mutex> lock(_vstPathsMutex);
				_vstPluginPaths.push_back(action.VstPath);
			}
			// Publish the new chain and signal _CommitChanges() to swap it in.
			_backVstChain = std::move(newChain);
			_flipVstChain.store(true, std::memory_order_release);
			_changesMade = true;
		}
		else if (action.PreInitPlugin)
		{
			// Load failed but plugin was pre-initialised on the UI thread;
			// destroying it here (job thread) would violate VST3 threading.
			vst::QueueForUiThreadDestroy(std::move(plugin));
		}

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	case JobAction::JOB_UNLOADVST:
	{
		// Build a new chain omitting the plugin at the requested index.
		// This runs on the job thread so DLL teardown is not under the audio mutex.
		// Take a snapshot of the current live chain under _vstChainMutex.
		std::shared_ptr<vst::VstChain> chainSnapshot;
		chainSnapshot = _vstChain.load(std::memory_order_acquire);

		auto newChain = std::make_shared<vst::VstChain>();
		const auto removeIndex = action.VstIndex;
		if (chainSnapshot)
		{
			for (size_t i = 0; i < chainSnapshot->NumPlugins(); ++i)
			{
				if (i == removeIndex)
					continue;
				auto existing = chainSnapshot->GetPlugin(i);
				if (existing)
					newChain->AddPlugin(existing);
			}
		}

		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			if (removeIndex < _vstPluginPaths.size())
				_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(removeIndex));
		}

		_backVstChain = std::move(newChain);
		_flipVstChain.store(true, std::memory_order_release);
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
