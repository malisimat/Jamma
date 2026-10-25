#include "Station.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include "../midi/MidiBlockTiming.h"
#include "../midi/MidiRouter.h"
#include "../io/NativeMidiSidecar.h"
#include "../io/IoSessionLoader.h"
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
		if (_loggingConfig.Event == "verbose")
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
	_allowedMidiChannels(),
	_allowedMidiChannelMask(0u),
	_midiVstRoutes(nullptr),
	_retainedMidiVstRoutes(),
	_sampleRate(44100.0f),
	_blockSize(512u),
	_vstBlockScratch(),
	_vstBlockPtrs()
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
	_rackState.RetireChainsOnUiThread();
}

std::optional<std::shared_ptr<Station>> Station::FromFile(StationParams stationParams,
	AudioMixerParams mixerParams, io::JamFile::Station stationStruct, std::wstring dir)
{
	return io::IoSessionLoader::ReconstructStation(std::move(stationParams), std::move(mixerParams),
		std::move(stationStruct), std::move(dir));
}

void Station::RestoreSavedRack(const std::vector<io::JamFile::VstEntry>& entries,
	std::array<midi::LiveVstChannelRoute, 16> routes, bool inactive)
{
	_rackState.RestoreSaved(entries, std::move(routes), inactive);
}

void Station::RestoreLiveVstRoutesBeforePublication(std::array<midi::LiveVstChannelRoute, 16> routes)
{
	_rackState.RestoreLiveChannelRoutes(std::move(routes));
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

	if (_stationModel && pass == base::PASS_SCENE)
	{
		RefreshQuantisationOverlayFromClock();
		_stationModel->DrawQuantisationOverlay(ctx, numInstances, pass);
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
	position.X += _stationModel ? _stationModel->PresentationOffsetX() : 0.0f;
	return position;
}

void Station::BeginPresentationMove(float startX)
{
	if (_stationModel) _stationModel->BeginPresentationMove(startX - ModelPosition().X);
}

bool Station::AdvancePresentationMove(float elapsedSeconds)
{
	return _stationModel && _stationModel->AdvancePresentationMove(elapsedSeconds);
}

void Station::FinishPresentationMove() noexcept
{
	if (_stationModel) _stationModel->FinishPresentationMove();
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

	const auto& chain = state->Midi.MidiChain;
	const bool vstActive = (channelCount > 0u) && chain && chain->NumPlugins() > 0u && (state->VstBlockPtrs.size() >= channelCount);

	_RunVstBlock(chain.get(), *state, vstActive,
		static_cast<unsigned int>(channelCount), sampsToRead, blockStartSample);

	// Drive any wired parameter automation. Runs independently of vstActive so a
	// bypassed/idle chain still receives recorded parameter motion. Flat loop over
	// the pre-baked dispatch list — no weak_ptr locks, no shared_ptr chasing.
	_automationPlayback.ProcessBlock(blockStartSample, sampsToRead);

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
	const auto chain = _rackState.ActiveChain();
	return chain && !_rackState.Inactive() && !_rackState.LatencySuspended() ? chain->GetLatencySamples() : 0;
}

void Station::PollPlaybackPluginChanges() const noexcept
{
	const auto state = _AudioStateSnapshot();
	if (!state) return;
	if (state->Midi.MidiChain) state->Midi.MidiChain->PollPendingChanges();
	for (const auto& take : state->OwnedLoopTakes)
		if (take) take->PollPlaybackPluginChanges();
}

unsigned int Station::RecordedPlaybackLatencySamps() const noexcept
{
	const auto stationLatency = static_cast<unsigned int>(std::max(0, CurrentVstLatencySamps()));
	const auto state = _AudioStateSnapshot();
	if (!state) return stationLatency;
	auto longest = static_cast<std::uint64_t>(stationLatency);
	if (state->Midi.MidiChain)
		for (std::size_t index = 0; index < state->Midi.MidiDestinations.size(); ++index)
			longest = std::max(longest, static_cast<std::uint64_t>(std::max(0, state->Midi.MidiChain->GetLatencySamplesFrom(index))));
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
	if (!state->Midi.MidiChain) return true;
	for (auto index = std::size_t{ 0u }; index < state->Midi.MidiDestinations.size(); ++index)
	{
		const auto remaining = static_cast<unsigned int>(std::max(0, state->Midi.MidiChain->GetLatencySamplesFrom(index)));
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
	const auto state = _AudioStateSnapshot();
	return !state || midi::MidiDelivery::ConfigureCompensation(state->Midi, referenceLatencySamps);
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
	midi::MidiDelivery::BeginRetiredBlock(state.Midi, blockStartSample, sampsToRead);
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
	}
	_midiDelivery.ProcessBlock(state.Midi, state.OwnedLoopTakes, state.InstrumentPtrs,
		vstActive, _membershipRemoving, _allowedMidiChannelMask, sampsToRead, blockStartSample);
	if (!vstActive) return;
	chain->ProcessRack(state.VstBlockPtrs.data(), state.InstrumentPtrs.data(), static_cast<int>(state.VstRenderChannels), sampsToRead);
	midi::MidiDelivery::RecoverSkippedBlocks(state.Midi);
}

void Station::RebuildAutomationDispatch()
{
	_automationPlayback.Rebuild(GetLoopTakeSnapshot(), _rackState.ActiveChain());
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
	auto chain = _rackState.ActiveChain();
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
	if (!_isEnabled || !_isVisible || res.IsEaten) return res;

	if (action.ElementType == GuiAction::ACTIONELEMENT_MIDIQUANTISATION)
	{
		if (_receiver) _receiver->OnAction(action);
	}
	else if (action.ElementType == GuiAction::ACTIONELEMENT_TOGGLE)
	{
		// Legacy direct toggle input; GuiRack handles its own toggles internally.
		if (const auto* toggle = std::get_if<GuiAction::GuiInt>(&action.Data); toggle && _router && action.Index == 2)
			_router->SetVisible(toggle->Value == static_cast<int>(GuiToggleParams::TOGGLE_ON));
	}

	const auto intent = gui::GuiRack::DecodeControlAction(action);
	if (const auto* panel = std::get_if<gui::GuiRack::PanelOpened>(&intent))
	{
		if (panel->State == GuiRackParams::RACK_ROUTER) _CollapseOtherTakeRouters();
		else if (panel->State == GuiRackParams::RACK_CHANNELS) _CollapseOtherTakeRoutersToChannels();
	}
	else if (const auto* count = std::get_if<gui::GuiRack::InputCountChange>(&intent))
		SetNumBusChannels(count->Count);
	else if (const auto* routes = std::get_if<gui::GuiRack::RouteChange>(&intent))
	{
		for (auto channel = 0u; channel < _audioMixers.size(); ++channel)
			_audioMixers[channel]->SetChannels(routes->OutputsFor(channel));
	}
	else if (const auto* level = std::get_if<gui::GuiRack::LevelChange>(&intent))
	{
		GuiAction mixerAction = action;
		mixerAction.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
		mixerAction.Data = GuiAction::GuiDouble{level->Value};
		if (level->Index == 0u) _masterMixer->OnAction(mixerAction);
		else if (level->Index - 1u < _audioMixers.size()) _audioMixers[level->Index - 1u]->OnAction(mixerAction);
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
        midi::CaptureHistory::Replay replay;
        if (!midiInputChannels.empty())
        {
            auto history = _captureHistory.PrepareReplay(earliest);
            if (!history)
            {
                std::cout << "[MIDI] Capture rejected: physical history overflow\n";
                return ActionResult::NoAction();
            }
            replay = std::move(*history);
        }
        auto newLoopTake = AddTake();
        newLoopTake->ConfigureTriggerCapture(timing);
        newLoopTake->Record(action.InputChannels, Name(), midiInputChannels,
            action.MidiInputDevices, std::move(replay.Held), transportStartSamps, timing.TriggerSample);
        for (const auto& entry : replay.Events)
            newLoopTake->RecordMidiEvent(entry.Event, entry.Device, *timing.TriggerSample);
        if (!midiInputChannels.empty()) _captureHistory.TrackTarget(newLoopTake);
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
        if (!midiInputChannels.empty()) _captureHistory.TrackTarget(newLoopTake);

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
	if (res.IsEaten && _loggingConfig.Event == "verbose")
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
		_midiDelivery.QueueLostPlaybackMidi(take, false);
		take->DetachGuiOwnershipForRetirement();
	}
	Jammable::Reset();
	_captureHistory.Reset();

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

void Station::SetQuantisationParams(std::optional<engine::QuantisationParams> params, bool confirm)
{
	if (_stationModel) _stationModel->SetQuantisationParams(params, confirm);
}

void Station::ClearQuantisationParams()
{
	SetQuantisationParams(std::nullopt);
}

void Station::SetQuantisationOverlayAlpha(float alpha) noexcept
{
	if (_stationModel) _stationModel->SetQuantisationOverlayAlpha(alpha);
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
	if (!_stationModel) return;
	const bool quantisable = _clock && _clock->IsQuantisable() && _stationModel->QuantisationOverlayEnabled();
	_stationModel->RefreshQuantisationOverlay(quantisable, quantisable ? _clock->QuantiseSamps() : 0u,
		quantisable ? LoopTake::QuantisationVisualsFor(GetLoopTakes()) : std::vector<engine::QuantisationLoopTakeVisual>{});
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
	const auto chain = _rackState.ActiveChain();
	if (chain && !chain->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
	if (_rackState.StagedChain() && _rackState.StagedChain() != chain && !_rackState.StagedChain()->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
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
        _captureHistory.ClearRecent();
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
	_automationPlayback.ClearAfterReadersStopped();
	const auto drainState = [](const std::shared_ptr<const AudioState>& state) {
		if (!state) return;
		vst::RackState::RetireChain(state->Midi.MidiChain);
		for (const auto& destination : state->Midi.MidiDestinations)
			if (destination && destination->Plugin)
				vst::QueueForUiThreadDestroy(destination->Plugin);
	};
	drainState(_audioState.exchange(nullptr, std::memory_order_acq_rel));
	for (const auto& retired : _retiredAudioStates)
		drainState(retired.State);
	_retiredAudioStates.clear();
	_midiDelivery.ClearAfterReadersStopped();
	_rackState.ClearAfterReadersStopped();
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
		for (const auto& noteOff : _captureHistory.RemoveChannels(removedMask))
			TryEnqueueSyntheticLiveMidi(noteOff);
	}

	_allowedMidiChannels = std::move(filtered);
	_allowedMidiChannelMask.store(mask, std::memory_order_release);
	if (_rackState.ClearChannelRoutes(removedMask))
	{
		_PublishAudioState();
		_changesMade = true;
	}

	if (_guiRack)
		_guiRack->SetAllowedMidiChannels(_allowedMidiChannels, true);
}

bool Station::TryEnqueueImmediateLiveMidi(const MidiEvent& event) noexcept
{
	if (MembershipRemoving()) return false;
	return _midiDelivery.TryEnqueueImmediateLiveMidi(event);
}

void Station::ResetLiveMidiWhileQuiescent() noexcept
{
	_midiDelivery.ResetLiveMidiWhileQuiescent();
}

bool Station::TryEnqueueSyntheticLiveMidi(const MidiEvent& event) noexcept
{
	if (MembershipRemoving()) return false;
	return _midiDelivery.TryEnqueueSyntheticLiveMidi(event);
}

void Station::ObservePhysicalMidiForRecording(const MidiEvent& event, const std::string& deviceName)
{
	if (MembershipRemoving()) return;
	_captureHistory.Observe(event, deviceName, _sampleRate, AcceptsLiveMidiChannel(event.Channel()));
}

void Station::CompleteMidiIngressPump(std::uint32_t deviceSample, bool overflow)
{
	_captureHistory.CompletePump(deviceSample, overflow);
}

void Station::FlushLiveHeldMidiNotes() noexcept
{
	const auto heldSnapshot = _captureHistory.ClearSources();
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
	if (deviceName.empty()) return;
	_captureHistory.LoseDevice(deviceName);
	FlushPendingLostLiveMidiNotes();
}

void Station::FlushPendingLostLiveMidiNotes() noexcept
{
	_captureHistory.FlushPendingReleases(_midiDelivery);
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
	if (_stationModel) _stationModel->InitPresentationResources(resourceLib, forceInit);
	GuiElement::_InitResources(resourceLib, forceInit);
}

void Station::_ReleaseResources()
{
	if (_stationModel) _stationModel->ReleasePresentationResources();
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
	if (const auto chain = _rackState.ActiveChain())
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
	// The rack commits metadata/chain together before Station publishes destinations.
	if (_rackState.CommitChain()) audioStateChanged = true;
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
	auto [pendingUnloads, pendingLoads] = _rackState.TakePendingJobs();
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
	const auto previous = _AudioStateSnapshot();
	_midiDelivery.Prepare(state->Midi, previous ? &previous->Midi : nullptr,
		(_rackState.Inactive() || _rackState.LatencySuspended()) ? nullptr : _rackState.ActiveChain(),
		state->OwnedLoopTakes, _midiVstRoutes.load(std::memory_order_acquire), _rackState.LiveChannelRoutes(),
		static_cast<unsigned int>(state->AudioBuffers.size()));
	state->VstRenderChannels = static_cast<unsigned int>(state->AudioBuffers.size());
	for (const auto& destination : state->Midi.MidiDestinations) state->VstRenderChannels = std::max(state->VstRenderChannels, destination->RenderChannels);
	for (const auto& destination : state->Midi.RetiredMidiDestinations) state->VstRenderChannels = std::max(state->VstRenderChannels, destination->RenderChannels);
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
	_automationPlayback.Collect();
	const auto completed = _audioCompletedStateGeneration.load(std::memory_order_acquire);
	auto keep = std::remove_if(_retiredAudioStates.begin(), _retiredAudioStates.end(),
		[completed](const RetiredAudioState& retired)
		{
			return retired.State && retired.ReleaseAfterGeneration <= completed;
		});
	_retiredAudioStates.erase(keep, _retiredAudioStates.end());
	// Registry pins also preserve notes delivered under an earlier route. A
	// binding with held notes survives route/index changes until cancellation.
	_midiDelivery.Collect();
	const auto current = _AudioStateSnapshot();
	if (current && current->Midi.RetiredMidiQueueCancelled.load(std::memory_order_acquire))
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
		_midiDelivery.QueueLostPlaybackMidi(take);
	}

	// Recorded releases must retire their callback-owned binding first. A live
	// synthetic NoteOff can be suppressed while that binding still owns the
	// pitch; sampling loop-held notes here cannot replace ownership retirement.
	if (flushLiveHeld) for (const auto& midiLoop : take->GetMidiLoopSnapshot())
		if (midiLoop) midiLoop->RequestHeldFlush();
	// A retained callback snapshot may still hold this take. Cancel only its
	// logical capture; keep loop buffers and writers stable until job/UI retirement.
	take->CancelCapture();
	if (flushLiveHeld) _midiDelivery.QueueLostPlaybackMidi(take);
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
	const auto state = _AudioStateSnapshot();
	if (_rackState.LoadVstPlugin(std::move(path), std::move(initialState), state ? state->Midi.RetiredMidiDestinations.size() : 0u)) _changesMade = true;
}

vst::VstRackSlot Station::GetVstRackSlot() const
{
	return _rackState.GetVstRackSlot();
}

bool Station::LoadManagedVstPlugin(std::wstring path)
{
	return LoadManagedVstSlot(std::move(path));
}

std::vector<vst::VstRackSlot> Station::GetVstRackSlots(std::size_t offset, std::size_t count) const
{
	return _rackState.GetVstRackSlots(offset, count);
}

bool Station::LoadManagedVstSlot(std::wstring path, const std::string& retryIdentity, bool discardOnFailure)
{
	const auto state = _AudioStateSnapshot();
	const bool accepted = _rackState.LoadManagedVstSlot(std::move(path), retryIdentity, discardOnFailure,
		state ? state->Midi.RetiredMidiDestinations.size() : 0u);
	if (accepted) _changesMade = true;
	return accepted;
}

std::optional<std::wstring> Station::TakeFailedInteractiveVstLoad(const std::string& identity)
{
	return _rackState.TakeFailedInteractiveVstLoad(identity);
}

bool Station::UnloadManagedVstPlugin(const std::string& identity)
{
	const bool accepted = _rackState.UnloadManagedVstPlugin(identity);
	if (accepted) _changesMade = true;
	return accepted;
}

bool Station::SetLiveVstChannelRoute(unsigned int channel, midi::LiveVstChannelRoute route, std::uint64_t revision)
{
	if (channel >= 16u) return false;
	auto routes = _rackState.LiveChannelRoutes();
	routes[channel] = std::move(route);
	return SetLiveVstChannelRoutes(std::move(routes), revision);
}

bool Station::SetLiveVstChannelRoutes(std::array<midi::LiveVstChannelRoute, 16> routes, std::uint64_t revision)
{
	auto allowed = _rackState.SetLiveVstChannelRoutes(std::move(routes), revision, _allowedMidiChannels);
	if (!allowed) return false;
	if (allowed->size() != _allowedMidiChannels.size()) SetAllowedMidiChannels(*allowed);
	_PublishAudioState();
	_changesMade = true;
	return true;
}

std::size_t Station::RackSlotCount() const
{
	return _rackState.RackSlotCount();
}

std::string Station::RackDiagnostic() const
{
	return _rackState.RackDiagnostic();
}

vst::RackState::Format Station::_RackFormat() const
{
	std::uint64_t maxChildLatency = 0u;
	for (const auto& take : GetLoopTakes())
		if (take) maxChildLatency = std::max(maxChildLatency, static_cast<std::uint64_t>(take->RecordedPlaybackLatencySamps()));
	return {_sampleRate, _blockSize, NumBusChannels(), maxChildLatency};
}

bool Station::_RackLatencyFeasible(const vst::VstChain& chain) const noexcept
{
	return vst::RackState::LatencyFeasible(chain, _RackFormat().MaxChildLatency);
}

void Station::_CompactMidiVstRoutesAfterRemoval(std::size_t removedIndex)
{
	if (const auto* current = _midiVstRoutes.load(std::memory_order_acquire))
	{
		auto routes = std::make_unique<MidiVstRoutingSnapshot>(*current);
		auto compact = [removedIndex](std::size_t& index) {
			if (index == removedIndex) index = MidiVstRoutingSnapshot::MissingPlugin;
			else if (index != MidiVstRoutingSnapshot::NoPlugin && index != MidiVstRoutingSnapshot::MissingPlugin && index > removedIndex) --index;
		};
		compact(routes->LivePlugin);
		for (auto& index : routes->PluginByMidiOutput) compact(index);
		const auto* published = routes.get();
		_retainedMidiVstRoutes.push_back(std::move(routes));
		_midiVstRoutes.store(published, std::memory_order_release);
	}
}

void Station::RefreshRackLatency()
{
	if (_rackState.Inactive()) return;
	const auto chain = _rackState.ActiveChain();
	if (!chain) return;
	const bool feasible = _RackLatencyFeasible(*chain);
	bool canResume = true;
	if (feasible && _rackState.LatencySuspended())
	{
		const auto state = _AudioStateSnapshot();
		canResume = !HasUnacknowledgedAudioState() && (!state || state->Midi.RetiredMidiDestinations.empty())
			&& !_midiDelivery.HasUnconsumedRemovedBinding();
	}
	if (_rackState.UpdateLatencyFeasibility(feasible, canResume)) _PublishAudioState();
}

bool Station::RemoveInactiveRackSlot(std::size_t ordinal, std::uint64_t revision)
{
	const bool accepted = _rackState.RemoveInactiveRackSlot(ordinal, revision,
		[this](std::size_t index) { _CompactMidiVstRoutesAfterRemoval(index); });
	if (accepted) _changesMade = true;
	return accepted;
}

bool Station::ClearInactiveRackRoutes(std::uint64_t revision)
{
	const bool accepted = _rackState.ClearInactiveRackRoutes(revision);
	if (accepted) _changesMade = true;
	return accepted;
}

bool Station::RecoverRack(std::uint64_t revision)
{
	const auto state = _AudioStateSnapshot();
	const bool accepted = _rackState.RecoverRack(revision, state ? state->Midi.RetiredMidiDestinations.size() : 0u);
	if (accepted) { _changesMade = true; _PublishAudioState(); }
	return accepted;
}

bool Station::LoadVstPluginSynchronously(const std::wstring& path,
	const std::vector<std::uint8_t>& initialState,
	bool bypass, const std::string& stateId)
{
	return _rackState.LoadVstPluginSynchronously(path, initialState, bypass, stateId, _RackFormat(), Name(),
		[this] { _PublishAudioState(); });
}

void Station::UnloadVstPlugin(size_t index)
{
	_rackState.UnloadVstPlugin(index);
	_changesMade = true;
}

std::shared_ptr<vst::IVstPlugin> Station::GetVstPlugin(size_t index) const
{
	return _rackState.GetVstPlugin(index);
}

std::vector<io::JamFile::VstEntry> Station::VstEntries() const
{
	return _rackState.VstEntries();
}

bool Station::TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const
{
	return _rackState.TryGroupVstEntries(output);
}

bool Station::TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry)
{
	return _rackState.TryRestoreGroupVstEntry(entry);
}

ActionResult Station::OnAction(JobAction action)
{
	if (MembershipRemoving()) return ActionResult::NoAction();
	const auto state = _AudioStateSnapshot();
	auto result = _rackState.OnAction(std::move(action), _RackFormat(), Name(),
		state ? state->Midi.RetiredMidiDestinations.size() : 0u,
		[this](std::size_t index) { _CompactMidiVstRoutesAfterRemoval(index); });
	if (result.IsEaten) _changesMade = true;
	return result;
}

void engine::Station::BeginMembershipRemoval()
{
	SetMembershipRemoving(true);
	if (const auto request = _firstMasterRecording.load())
		request->Clock->CancelRecordingSeedOnOwner(request->Timing.Version);
	for (const auto& take : GetLoopTakeSnapshot()) take->CancelCapture();
}
