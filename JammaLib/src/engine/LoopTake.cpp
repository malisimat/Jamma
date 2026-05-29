#include "LoopTake.h"

#include <algorithm>
#include <cstdint>

#include "MidiModel.h"

namespace
{
	constexpr int MidiQuantisationDragPixelsPerStep = 32;

	bool HasMidiQuantisationGestureModifiers(base::Action::Modifiers modifiers) noexcept
	{
		return (base::Action::MODIFIER_CTRL & modifiers)
			&& (base::Action::MODIFIER_SHIFT & modifiers);
	}

	int MidiQuantisationFractionIndex(engine::MidiQuantisationFraction fraction) noexcept
	{
		return static_cast<int>(fraction);
	}

	engine::MidiQuantisationFraction ClampMidiQuantisationFractionIndex(int index) noexcept
	{
		if (index < 0)
			index = 0;
		else if (index >= static_cast<int>(engine::MidiQuantisationFractionCount))
			index = static_cast<int>(engine::MidiQuantisationFractionCount) - 1;

		return static_cast<engine::MidiQuantisationFraction>(index);
	}

	int MidiQuantisationDragSteps(int deltaY) noexcept
	{
		const auto absDelta = deltaY < 0 ? -deltaY : deltaY;
		const auto steps = (absDelta + (MidiQuantisationDragPixelsPerStep / 2)) /
			MidiQuantisationDragPixelsPerStep;
		return deltaY < 0 ? steps : -steps;
	}

	const char* MidiQuantisationFractionLabel(engine::MidiQuantisationFraction fraction) noexcept
	{
		switch (fraction)
		{
		case engine::MidiQuantisationFraction::Whole: return "1";
		case engine::MidiQuantisationFraction::Half: return "1/2";
		case engine::MidiQuantisationFraction::Quarter: return "1/4";
		case engine::MidiQuantisationFraction::Eighth: return "1/8";
		case engine::MidiQuantisationFraction::Sixteenth: return "1/16";
		case engine::MidiQuantisationFraction::ThirtySecond: return "1/32";
		default: return "?";
		}
	}

	std::uint64_t PackMidiQuantisationSettings(const engine::MidiQuantisationSettings& settings) noexcept
	{
		const auto fraction = static_cast<std::uint64_t>(settings.Fraction);
		return (settings.Enabled ? 1ull : 0ull)
			| (fraction << 8u)
			| (static_cast<std::uint64_t>(settings.GrainSamps) << 16u);
	}

	engine::MidiQuantisationSettings UnpackMidiQuantisationSettings(std::uint64_t packed) noexcept
	{
		engine::MidiQuantisationSettings settings;
		settings.Enabled = (packed & 1ull) != 0ull;

		auto fraction = static_cast<int>((packed >> 8u) & 0xffull);
		if (fraction < 0)
			fraction = 0;
		else if (fraction >= static_cast<int>(engine::MidiQuantisationFractionCount))
			fraction = static_cast<int>(engine::MidiQuantisationFractionCount) - 1;
		settings.Fraction = static_cast<engine::MidiQuantisationFraction>(fraction);
		settings.GrainSamps = static_cast<std::uint32_t>((packed >> 16u) & 0xffffffffull);
		return settings;
	}

	void DrainVstChain(std::shared_ptr<vst::VstChain> chain)
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
}

using namespace engine;
using base::Action;
using base::AudioSink;
using base::AudioSource;
using base::DrawContext;
using base::Drawable;
using base::DrawableParams;
using engine::Trigger;
using resources::ResourceLib;
using actions::ActionResult;
using actions::TriggerAction;
using actions::GuiAction;
using actions::JobAction;
using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::MergeMixBehaviourParams;
using audio::WireMixBehaviourParams;
using gui::GuiRackParams;
using utils::Size2d;

const Size2d LoopTake::_Gap = { 6, 6 };
const Size2d LoopTake::_ToggleSize = { 64, 32 };
const Size2d LoopTake::_ToggleGap = { 6, 6 };

LoopTake::LoopTake(LoopTakeParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	MultiAudioSink(),
	_flipLoopBuffer(false),
	_loopsNeedUpdating(false),
	_endRecordingCompleted(false),
	_state(STATE_INACTIVE),
	_id(params.Id),
	_sourceId(""),
	_lastBufSize(constants::MaxBlockSize),
	_fadeSamps(params.FadeSamps),
	_sourceType(SOURCE_LOOPTAKE),
	_recordedSampCount(0),
	_endRecordSampCount(0),
	_endRecordSamps(0),
	_midiVisualPlayIndex(0ul),
	_midiVisualLoopLength(0ul),
	_isPunchInActive(false),
	_guiRack(nullptr),
	_masterMixer(nullptr),
	_loops(),
	_backLoops(),
	_midiQuantisationPacked(PackMidiQuantisationSettings(MidiQuantisationSettings())),
	_midiQuantisationUpdatePending(false),
	_midiQuantisationGestureActive(false),
	_midiQuantisationGestureMoved(false),
	_midiQuantisationGestureStartPosition({ 0, 0 }),
	_midiQuantisationGestureStartFraction(MidiQuantisationFraction::Whole),
	_audioMixers(),
	_backAudioMixers(),
	_audioBuffers(),
	_backAudioBuffers(),
	_vstChain(nullptr),
	_vstPluginPaths(),
	_vstBlockScratch(),
	_vstBlockPtrs()
{
	_masterMixer = std::make_shared<AudioMixer>(mixerParams);
	_guiRack = std::make_shared<gui::GuiRack>(_GetRackParams(params.Size));

	_children.push_back(_guiRack);

	_WireVuSliders();
	_PublishAudioState();
}

LoopTake::~LoopTake()
{
	DrainVstChain(_vstChain.load(std::memory_order_acquire));
	DrainVstChain(_backVstChain);
}

std::optional<std::shared_ptr<LoopTake>> LoopTake::FromFile(LoopTakeParams takeParams, io::JamFile::LoopTake takeStruct, std::wstring dir)
{
	auto mixerParams = GetMixerParams({ 100,100 }, audio::WireMixBehaviourParams());
	auto take = std::make_shared<LoopTake>(takeParams, mixerParams);

	LoopParams loopParams;
	loopParams.Wav = "hh";

	for (auto loopStruct : takeStruct.Loops)
	{
		auto loop = Loop::FromFile(loopParams, loopStruct, dir);
		
		if (loop.has_value())
		{
			for (const auto& vstEntry : loopStruct.VstChain)
				loop.value()->LoadVstPlugin(utils::DecodeUtf8(vstEntry.Path));

			take->AddLoop(loop.value());
		}
	}

	for (const auto& vstEntry : takeStruct.VstChain)
		take->LoadVstPlugin(utils::DecodeUtf8(vstEntry.Path));

	return take;
}

AudioMixerParams LoopTake::GetMixerParams(utils::Size2d loopSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, loopSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

void LoopTake::SetSize(utils::Size2d size)
{
	auto mixerParams = GetMixerParams(size,
		WireMixBehaviourParams());

	for (auto& mixer : _audioMixers)
	{
		mixer->SetSize(mixerParams.Size);
	}

	if (_guiRack)
		_guiRack->SetSize(size);

	GuiElement::SetSize(size);

	_ArrangeChildren();
}

void LoopTake::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	_UpdateMidiModelRotation();
	base::GuiElement::Draw3d(ctx, numInstances, pass);
}

unsigned int LoopTake::NumInputChannels(Audible::AudioSourceType source) const
{
	return (_changesMade && _flipLoopBuffer) ?
		(unsigned int)_backLoops.size() :
		(unsigned int)_loops.size();
}

unsigned int LoopTake::NumOutputChannels(Audible::AudioSourceType source) const
{
	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		return (_changesMade && _flipLoopBuffer) ?
			(unsigned int)_backLoops.size() :
			(unsigned int)_loops.size();
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		return NumBusChannels();
	}

	return 0u;
}

unsigned int LoopTake::NumBusChannels() const
{
	if (nullptr != _guiRack)
		return _guiRack->NumOutputChannels();

	return 0u;
}

void LoopTake::Zero(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	const auto channelCount = ((Audible::AUDIOSOURCE_ADC == source)
		|| (Audible::AUDIOSOURCE_MONITOR == source)
		|| (Audible::AUDIOSOURCE_BOUNCE == source)) ?
		static_cast<unsigned int>(state->Loops.size()) :
		static_cast<unsigned int>(state->AudioBuffers.size());
	for (auto chan = 0u; chan < channelCount; chan++)
	{
		auto channel = _InputChannel(chan, source);
		if (channel)
			channel->Zero(numSamps);
	}

	for (auto& loop : state->Loops)
		loop->Zero(numSamps);
}

// Only called when outputting to DAC
void LoopTake::WriteBlock(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps)
{
	if (nullptr == dest)
		return;
	
	if (!dest->IsArmed())
		return;

	auto ptr = Sharable::shared_from_this();
	auto loopDest = trigger == nullptr ?
		std::dynamic_pointer_cast<MultiAudioSink>(ptr) :
		dest;
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (const auto& loop : state->Loops)
		loop->WriteBlock(loopDest,
			trigger,
			indexOffset,
			numSamps);

	if (nullptr != trigger)
		return;

	auto sampsToRead = (numSamps <= constants::MaxBlockSize) ? numSamps : constants::MaxBlockSize;
	auto masterLevel = static_cast<float>(_masterMixer->Level());
	auto masterPeak = 0.0f;
	const auto channelCount = (state->AudioBuffers.size() < state->AudioMixers.size()) ? state->AudioBuffers.size() : state->AudioMixers.size();
	if (channelCount == 0u)
	{
		_masterMixer->UpdateVu(0.0f, sampsToRead);
		_masterMixer->Offset(sampsToRead);
		return;
	}

	for (auto i = 0u; i < channelCount; i++)
	{
		auto* scratch = (i < state->VstBlockPtrs.size()) ? state->VstBlockPtrs[i] : nullptr;
		if (!scratch)
			continue;

		auto& buf = state->AudioBuffers[i];
		buf->Delay(sampsToRead);
		auto srcPtr = buf->PlaybackRead(scratch, sampsToRead);

		// When PlaybackRead returns a direct pointer into the ring buffer
		// (no wrap-around), we must copy into scratch before processing.
		if (srcPtr != scratch)
			std::copy(srcPtr, srcPtr + sampsToRead, scratch);

		for (auto samp = 0u; samp < sampsToRead; samp++)
			scratch[samp] *= masterLevel;
	}

	auto chain = _vstChain.load(std::memory_order_acquire);
	if (chain && chain->IsActive() && (state->VstBlockPtrs.size() >= channelCount))
		chain->ProcessBlockMulti(state->VstBlockPtrs.data(), static_cast<int>(channelCount), sampsToRead);

	for (auto i = 0u; i < channelCount; i++)
	{
		auto* scratch = state->VstBlockPtrs[i];
		if (!scratch)
			continue;

		// Track max peak across all channels for the master VU.
		for (auto samp = 0u; samp < sampsToRead; samp++)
		{
			auto absSamp = std::abs(scratch[samp]);
			if (absSamp > masterPeak)
				masterPeak = absSamp;
		}

		state->AudioMixers[i]->WriteBlock(dest, scratch, sampsToRead);
	}

	_masterMixer->UpdateVu(masterPeak, sampsToRead);
	_masterMixer->Offset(sampsToRead);
}

void LoopTake::EndMultiPlay(unsigned int numSamps)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (auto& loop : state->Loops)
		loop->EndMultiPlay(numSamps);

	for (auto& buffer : state->AudioBuffers)
	{
		buffer->EndWrite(numSamps, true);
		buffer->EndPlay(numSamps);
	}

	if (_midiVisualLoopLength > 0ul)
	{
		_midiVisualPlayIndex += numSamps;
		while (_midiVisualPlayIndex >= _midiVisualLoopLength)
			_midiVisualPlayIndex -= _midiVisualLoopLength;
	}
}

bool LoopTake::IsArmed() const
{
	auto state = _state.load(std::memory_order_acquire);
	return (STATE_RECORDING == state) ||
		(STATE_PLAYINGRECORDING == state) ||
		(STATE_OVERDUBBING == state) ||
		(STATE_PUNCHEDIN == state) ||
		(STATE_OVERDUBBINGRECORDING == state) ||
		_isPunchInActive.load(std::memory_order_acquire);
}

void LoopTake::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	auto audioState = _AudioStateSnapshot();
	if (!audioState)
		return;

	for (auto& loop : audioState->Loops)
		 loop->EndWrite(numSamps, updateIndex);

	auto isRecording = IsArmed();
	auto takeState = _state.load(std::memory_order_acquire);
	auto isEndRecording = (STATE_PLAYINGRECORDING == takeState) ||
		(STATE_OVERDUBBINGRECORDING == takeState);

	if (isEndRecording)
	{
		_endRecordSampCount += numSamps;
		if ((_endRecordSampCount >= _endRecordSamps) && !_isPunchInActive.load(std::memory_order_acquire))
			_endRecordingCompleted = true;
	}

	if (isRecording)
	{
		_recordedSampCount.fetch_add(numSamps, std::memory_order_relaxed);
		_loopsNeedUpdating = true;
		_changesMade = true;
	}
}

void LoopTake::SetSelectDepth(base::SelectDepth depth)
{
	Jammable::SetSelectDepth(depth);
	if (_guiRack)
		_guiRack->SetVisible(depth == base::SelectDepth::DEPTH_LOOPTAKE);
}

ActionResult LoopTake::OnAction(actions::TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if (_midiQuantisationGestureActive)
	{
		if (actions::TouchAction::TOUCH_UP == action.State)
		{
			const auto quantisation = MidiQuantisation();
			if (!_midiQuantisationGestureMoved)
			{
				const auto enabled = !quantisation.Enabled;
				std::cout << "MIDI quantisation gesture click: take=" << _id
					<< " enabled " << quantisation.Enabled << " -> " << enabled
					<< " fraction=" << MidiQuantisationFractionLabel(quantisation.Fraction)
					<< " grain=" << quantisation.GrainSamps << std::endl;
				_ApplyMidiQuantisationGesture(quantisation.Fraction, enabled, "click-toggle");
			}
			else
			{
				std::cout << "MIDI quantisation gesture end: take=" << _id
					<< " moved=true enabled=" << quantisation.Enabled
					<< " fraction=" << MidiQuantisationFractionLabel(quantisation.Fraction)
					<< " grain=" << quantisation.GrainSamps << std::endl;
			}

			_midiQuantisationGestureActive = false;
			_midiQuantisationGestureMoved = false;
			return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
		}

		return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	}

	if ((actions::TouchAction::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& HasMidiQuantisationGestureModifiers(action.Modifiers)
		&& HitTest(action.Position))
		return BeginMidiQuantisationGesture(action);

	return Jammable::OnAction(action);
}

ActionResult LoopTake::BeginMidiQuantisationGesture(actions::TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if ((actions::TouchAction::TOUCH_DOWN != action.State)
		|| (0 != action.Index)
		|| !HasMidiQuantisationGestureModifiers(action.Modifiers))
		return ActionResult::NoAction();

	action = GlobalToLocal(action);

	_midiQuantisationGestureActive = true;
	_midiQuantisationGestureMoved = false;
	_midiQuantisationGestureStartPosition = action.Position;
	const auto quantisation = MidiQuantisation();
	_midiQuantisationGestureStartFraction = quantisation.Fraction;

	std::cout << "MIDI quantisation gesture start: take=" << _id
		<< " enabled=" << quantisation.Enabled
		<< " fraction=" << MidiQuantisationFractionLabel(quantisation.Fraction)
		<< " grain=" << quantisation.GrainSamps
		<< " position=(" << action.Position.X << "," << action.Position.Y << ")"
		<< " modifiers=" << action.Modifiers << std::endl;

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = actions::ACTIONRESULT_ACTIVEELEMENT;
	res.ActiveElement = GuiElement::shared_from_this();
	return res;
}

ActionResult LoopTake::OnAction(actions::TouchMoveAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if (!_midiQuantisationGestureActive)
		return Jammable::OnAction(action);

	_midiQuantisationGestureMoved = true;

	action = GlobalToLocal(action);
	const auto delta = action.Position - _midiQuantisationGestureStartPosition;
	const auto startIndex = MidiQuantisationFractionIndex(_midiQuantisationGestureStartFraction);
	const auto fraction = ClampMidiQuantisationFractionIndex(startIndex + MidiQuantisationDragSteps(delta.Y));

	_ApplyMidiQuantisationGesture(fraction, true, "drag-fraction");
	return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
}

ActionResult LoopTake::OnAction(GuiAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
		return res;

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
	else if (auto d = std::get_if<GuiAction::GuiDouble>(&action.Data))
	{
		if (0 == action.Index)
		{
			GuiAction mixerAction = action;
			mixerAction.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
			_masterMixer->OnAction(mixerAction);
		}
		else if ((action.Index > 0) && ((action.Index - 1) < _loops.size()))
		{
			_loops[action.Index - 1]->SetMixerLevel(d->Value);
		}
	}
	else if (std::get_if<GuiAction::GuiInt>(&action.Data))
	{
		// Rack state pre-notification from _guiRack — forward to parent receiver (Station).
		if (_receiver)
			_receiver->OnAction(action);
	}
	else if (auto arr = std::get_if<GuiAction::GuiIntArray>(&action.Data))
	{
		if (GuiAction::ACTIONELEMENT_MIDIQUANTISATION == action.ElementType)
		{
			// Payload layout: [enabled, fractionIndex, optional grainSamps].
			// grainSamps == 0 (or absent) preserves the current grain so the
			// caller can toggle on/off without re-supplying timing each gesture.
			MidiQuantisationSettings updated = MidiQuantisation();
			if (arr->Values.size() >= 1)
				updated.Enabled = (0 != arr->Values[0]);
			if (arr->Values.size() >= 2)
			{
				const auto raw = arr->Values[1];
				const auto clamped = raw < 0 ? 0 :
					(raw >= static_cast<int>(MidiQuantisationFractionCount) ?
						static_cast<int>(MidiQuantisationFractionCount) - 1 :
						raw);
				updated.Fraction = static_cast<MidiQuantisationFraction>(clamped);
			}
			if (arr->Values.size() >= 3 && arr->Values[2] > 0)
				updated.GrainSamps = static_cast<std::uint32_t>(arr->Values[2]);
			std::cout << "MIDI quantisation GuiAction: take=" << _id
				<< " enabled=" << updated.Enabled
				<< " fraction=" << MidiQuantisationFractionLabel(updated.Fraction)
				<< " grain=" << updated.GrainSamps << std::endl;
			SetMidiQuantisation(updated);
		}
	}

	return res;
}

ActionResult LoopTake::OnAction(JobAction action)
{
	switch (action.JobActionType)
	{
	case JobAction::JOB_LOADVST:
	{
		// Take a snapshot of the current live chain under _vstChainMutex so we
		// don't race with _CommitChanges() swapping it on the main thread.
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

		auto plugin = action.PreInitPlugin
			? action.PreInitPlugin
			: vst::MakePluginForPath(action.VstPath);
		auto hostChannels = NumInputChannels(Audible::AUDIOSOURCE_LOOPS);
		if (hostChannels == 0u)
			hostChannels = 1u;
		if (plugin->Load(action.VstPath, _sampleRate, _lastBufSize, hostChannels, vst::HostedLayoutMode::Exact))
		{
			newChain->AddPlugin(plugin);
			{
				std::lock_guard<std::mutex> lock(_vstPathsMutex);
				_vstPluginPaths.push_back(action.VstPath);
			}
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
	case JobAction::JOB_UPDATELOOPS:
	{
		_UpdateLoops();

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}
	break;
	case JobAction::JOB_UPDATEMIDIQUANTISATION:
	{
		const auto settings = MidiQuantisation();
		for (auto& midiLoop : action.MidiLoops)
		{
			if (midiLoop)
				midiLoop->SetQuantisation(settings);
		}

		const auto displayLength = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
		auto queuedModels = 0u;
		for (auto& midiLoop : action.MidiLoops)
		{
			if (midiLoop && midiLoop->QueueModelUpdateFromEvents(displayLength, true))
				++queuedModels;
		}

		std::cout << "MIDI quantisation update job: take=" << _id
			<< " enabled=" << settings.Enabled
			<< " fraction=" << MidiQuantisationFractionLabel(settings.Fraction)
			<< " grain=" << settings.GrainSamps
			<< " displayLength=" << displayLength
			<< " midiLoops=" << action.MidiLoops.size()
			<< " queuedModels=" << queuedModels << std::endl;

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	break;
	case JobAction::JOB_ENDRECORDING:
	{
		EndRecording();
		_UpdateLoops();
		_UpdateMidiModels(true);
		std::cout << "Ended recording" << std::endl;

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}
	break;
	}

	return ActionResult::NoAction();
}

std::string LoopTake::Id() const
{
	return _id;
}

std::string LoopTake::SourceId() const
{
	return _sourceId;
}

LoopTake::LoopTakeSource LoopTake::TakeSourceType() const
{
	return _sourceType;
}

LoopTake::LoopTakeState LoopTake::TakeState() const
{
	return _state.load(std::memory_order_relaxed);
}

unsigned long LoopTake::NumRecordedSamps() const
{
	return _recordedSampCount.load(std::memory_order_relaxed);
}

std::shared_ptr<Loop> LoopTake::AddLoop(unsigned int chan, std::string stationName)
{
	auto newNumLoops = (unsigned int)_loops.size() + 1;
	const auto loopSlot = static_cast<unsigned int>(_backLoops.size());
	(void)chan;

	auto loopHeight = _CalcLoopHeight(_sizeParams.Size.Height, newNumLoops);

	audio::WireMixBehaviourParams wire;
	wire.Channels = { loopSlot };
	auto mixerParams = Loop::GetMixerParams({ 110, loopHeight }, wire);
	
	LoopParams loopParams;
	loopParams.Wav = stationName;
	loopParams.Id = stationName + "-" + utils::GetGuid();
	loopParams.TakeId = _id;
	loopParams.Channel = loopSlot;
	loopParams.FadeSamps = _fadeSamps;
	auto loop = std::make_shared<Loop>(loopParams, mixerParams);
	AddLoop(loop);

	return loop;
}

void LoopTake::AddLoop(std::shared_ptr<Loop> loop)
{
	_backLoops.push_back(loop);
	_backAudioBuffers.push_back(std::make_shared<audio::AudioBuffer>(_lastBufSize));
	
	MergeMixBehaviourParams mergeParams;
	if (_backAudioBuffers.size() <= NumBusChannels())
		mergeParams.Channels.push_back((unsigned int)(_backAudioBuffers.size() - 1));

	auto mixerParams = LoopTake::GetMixerParams(_guiParams.Size, mergeParams);
	auto mixer = std::make_shared<audio::AudioMixer>(mixerParams);
	mixer->SetUnmutedLevel(1.0);
	_backAudioMixers.push_back(mixer);

	_children.push_back(loop);

	Init();

	_ArrangeChildren();

	_flipLoopBuffer = true;
	_changesMade = true;

	_guiRack->SetNumInputChannels((unsigned int)_backLoops.size());
	_guiRack->AddRoute((unsigned int)_backLoops.size() - 1, (unsigned int)_backLoops.size() - 1);
}

void LoopTake::SetMixerLevel(unsigned int chan, double level)
{
	if (chan < _audioMixers.size())
		_audioMixers[chan]->SetUnmutedLevel(level);
}

void LoopTake::SetupBuffers(unsigned int bufSize)
{
	_lastBufSize = bufSize;

	for (auto& loop : _loops)
		loop->SetBlockSize(bufSize);
	for (auto& loop : _backLoops)
		loop->SetBlockSize(bufSize);

	auto& buffers = (_flipLoopBuffer && _changesMade) ?
		_backAudioBuffers :
		_audioBuffers;

	for (auto& buf : buffers)
	{
		buf->SetSize(bufSize);
	}
}

void LoopTake::SetNumBusChannels(unsigned int chans)
{
	for (auto& mixer : _audioMixers)
	{
		mixer->SetMaxChannels(chans);
	}

	_masterMixer->SetMaxChannels(chans);
	_guiRack->SetNumOutputChannels(chans);
}

void LoopTake::Record(std::vector<unsigned int> channels,
	std::string stationName,
	std::vector<unsigned int> midiChannels,
	std::vector<std::string> midiDevices)
{
	if (STATE_INACTIVE != _state.load(std::memory_order_relaxed))
		return;

	_state.store(STATE_RECORDING, std::memory_order_release);

	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_midiVisualPlayIndex = 0ul;
	_midiVisualLoopLength = 0ul;
	_isPunchInActive.store(false, std::memory_order_relaxed);
	_backLoops.clear();
	_RemoveMidiModelChildren();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan, stationName);
		loop->Record();
	}

	_midiLoops.clear();
	_midiLoopChannels.clear();
	_midiLoopDevices.clear();
	if (midiDevices.empty() && !midiChannels.empty())
		midiDevices.push_back("");

	for (const auto& midiDevice : midiDevices)
	{
		for (auto midiChan : midiChannels)
		{
			auto midiLoop = std::make_shared<MidiLoop>();
			MidiModelParams modelParams;
			modelParams.ModelScale = 1.0f;
			auto midiModel = std::make_shared<MidiModel>(modelParams);
			midiLoop->AttachModel(midiModel);
			midiLoop->StartRecord();
			midiLoop->SetQuantisation(MidiQuantisation());
			_midiLoops.push_back(midiLoop);
			_midiLoopChannels.push_back(midiChan);
			_midiLoopDevices.push_back(midiDevice);
			_children.push_back(midiModel);
		}
	}

	if (!midiChannels.empty())
	{
		Init();
		_ArrangeChildren();
	}

	//_flipLoopBuffer = true;
	_loopsNeedUpdating = true;
	_changesMade = true;
}

bool LoopTake::RecordMidiEvent(const MidiEvent& ev, std::uint32_t globalSampleNow) noexcept
{
	return RecordMidiEvent(ev, "", globalSampleNow);
}

bool LoopTake::RecordMidiEvent(const MidiEvent& ev,
	const std::string& device,
	std::uint32_t globalSampleNow) noexcept
{
	if (_midiLoops.empty())
		return false;

	const auto midiChan = ev.Channel();
	bool recorded = false;
	const auto recordedNow = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));

	for (auto i = 0u; i < _midiLoops.size(); ++i)
	{
		if (_midiLoopChannels[i] != midiChan)
			continue;

		if (!_midiLoopDevices[i].empty() && (_midiLoopDevices[i] != device))
			continue;

		if (_midiLoops[i]->State() != MidiLoopState::Recording)
			continue;

		MidiEvent stamped = ev;
		stamped.sampleOffset = ResolveMidiRecordSample(ev.sampleOffset, globalSampleNow, recordedNow);
		_midiLoops[i]->RecordEvent(stamped);
		recorded = true;
	}

	return recorded;
}

unsigned int LoopTake::ReadMidiBlock(std::uint32_t globalSample,
	std::uint32_t numSamples,
	IMidiOutputSink& sink,
	unsigned int firstOutputIndex) noexcept
{
	class IndexedMidiSink final : public IMidiSink
	{
	public:
		IndexedMidiSink(IMidiOutputSink& outputSink, unsigned int outputIndex) noexcept :
			_outputSink(outputSink),
			_outputIndex(outputIndex)
		{
		}

		void OnEvent(const MidiEvent& ev) noexcept override
		{
			_outputSink.OnEvent(_outputIndex, ev);
		}

	private:
		IMidiOutputSink& _outputSink;
		unsigned int _outputIndex;
	};

	for (auto i = 0u; i < _midiLoops.size(); ++i)
	{
		if (!_midiLoops[i])
			continue;

		IndexedMidiSink indexedSink(sink, firstOutputIndex + i);
		_midiLoops[i]->ReadBlock(globalSample, numSamples, indexedSink);
	}

	return static_cast<unsigned int>(_midiLoops.size());
}

std::uint32_t LoopTake::ResolveMidiRecordSample(std::uint32_t eventGlobalSample,
	std::uint32_t globalSampleNow,
	std::uint32_t recordedSampleCount) noexcept
{
	if (0u == recordedSampleCount)
		return 0u;

	const auto samplesAgo = static_cast<std::int32_t>(globalSampleNow - eventGlobalSample);

	if (samplesAgo <= 0)
		return recordedSampleCount;

	const auto delta = static_cast<std::uint32_t>(samplesAgo);
	if (delta >= recordedSampleCount)
		return 0u;

	return recordedSampleCount - delta;
}

void LoopTake::Play(unsigned long index,
	unsigned long loopLength,
	unsigned int endRecordSamps)
{
	auto state = _state.load(std::memory_order_relaxed);
	if ((STATE_RECORDING != state) &&
		(STATE_OVERDUBBING != state) &&
		(STATE_PUNCHEDIN != state))
		return;

	_endRecordSampCount = 0;
	_endRecordSamps = endRecordSamps;
	_midiVisualLoopLength = loopLength;
	_midiVisualPlayIndex = index >= constants::MaxLoopFadeSamps ?
		index - constants::MaxLoopFadeSamps :
		index;
	while (_midiVisualLoopLength > 0ul && _midiVisualPlayIndex >= _midiVisualLoopLength)
		_midiVisualPlayIndex -= _midiVisualLoopLength;
	auto continueCapture = (endRecordSamps > 0) || _isPunchInActive.load(std::memory_order_relaxed);

	for (auto& loop : _loops)
	{
		loop->Play(index, loopLength, continueCapture);
	}

	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop->State() == MidiLoopState::Recording)
		{
			midiLoop->EndRecord(static_cast<std::uint32_t>(loopLength));
			midiLoop->UpdateModelFromEvents(static_cast<std::uint32_t>(loopLength), true);
		}
	}

	auto isOverdubbing = (STATE_OVERDUBBING == state) || (STATE_PUNCHEDIN == state);
	if (_isPunchInActive.load(std::memory_order_relaxed))
		isOverdubbing = true;
	auto recordState = isOverdubbing ? STATE_OVERDUBBINGRECORDING : STATE_PLAYINGRECORDING;
	auto playState = continueCapture ? recordState : STATE_PLAYING;
	_state.store(loopLength > 0 ? playState : STATE_INACTIVE, std::memory_order_release);
}

bool LoopTake::Select()
{
	auto isNewState = GuiElement::Select();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->Select();
		}
	}

	return isNewState;
}

bool LoopTake::DeSelect()
{
	auto isNewState = GuiElement::DeSelect();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->DeSelect();
		}
	}

	return isNewState;
}

bool LoopTake::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->Mute();
		}
	}

	return isNewState;
}

bool LoopTake::UnMute()
{
	auto isNewState = Tweakable::UnMute();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->UnMute();
		}
	}

	return isNewState;
}

void LoopTake::SetPickingFromState(EditMode mode, bool flipState)
{
	switch (mode)
	{
	case EDIT_SELECT:
		_isPicking3d = flipState ?
			!_isSelected :
			_isSelected;
		break;
	case EDIT_MUTE:
	{
		auto tweakState = GetTweakState();
		_isPicking3d = flipState ? 
			!(TWEAKSTATE_MUTED & tweakState)
			: (TWEAKSTATE_MUTED & tweakState);
		break;
	}
	}

	GuiElement::SetPicking3d(_isPicking3d);
}

void LoopTake::SetStateFromPicking(EditMode mode, bool flipState)
{
	bool isPicking = flipState ?
		!_isPicking3d :
		_isPicking3d;

	switch (mode)
	{
	case EDIT_SELECT:
		if (isPicking)
			Select();
		else
			DeSelect();
		break;
	case EDIT_MUTE:
		if (isPicking)
			Mute();
		else
			UnMute();
		break;
	}
}

void LoopTake::EndRecording()
{
	auto state = _state.load(std::memory_order_relaxed);
	if ((STATE_PLAYINGRECORDING != state) &&
		(STATE_OVERDUBBINGRECORDING != state))
		return;

	if (_isPunchInActive.load(std::memory_order_relaxed))
		return;

	_state.store(STATE_PLAYING, std::memory_order_release);

	for (auto& loop : _loops)
	{
		loop->EndRecording();
	}
}

void LoopTake::Ditch()
{
	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_midiVisualPlayIndex = 0ul;
	_midiVisualLoopLength = 0ul;
	_isPunchInActive.store(false, std::memory_order_relaxed);

	for (auto& loop : _loops)
	{
		loop->Ditch();
	}

	Zero(_lastBufSize, Audible::AUDIOSOURCE_LOOPS);

	_loops.clear();

	for (auto& midiLoop : _midiLoops)
		midiLoop->Reset();
	_RemoveMidiModelChildren();
	_midiLoops.clear();
	_midiLoopChannels.clear();
	_midiLoopDevices.clear();
}

void LoopTake::Overdub(std::vector<unsigned int> channels, std::string stationName)
{
	if (STATE_INACTIVE != _state.load(std::memory_order_relaxed))
		return;

	_state.store(STATE_OVERDUBBING, std::memory_order_release);

	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_midiVisualPlayIndex = 0ul;
	_midiVisualLoopLength = 0ul;
	_isPunchInActive.store(false, std::memory_order_relaxed);
	_backLoops.clear();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan, stationName);
		loop->Overdub();
	}

	_loopsNeedUpdating = true;
	_changesMade = true;
}

void LoopTake::PunchIn()
{
	auto state = _state.load(std::memory_order_relaxed);
	if ((STATE_OVERDUBBING != state) &&
		(STATE_OVERDUBBINGRECORDING != state) &&
		(STATE_PLAYING != state))
		return;

	_isPunchInActive.store(true, std::memory_order_release);
	if (STATE_OVERDUBBING == state)
		_state.store(STATE_PUNCHEDIN, std::memory_order_release);

	for (auto& loop : _loops)
	{
		loop->PunchIn();
	}
}

void LoopTake::PunchOut()
{
	auto state = _state.load(std::memory_order_relaxed);
	if (!_isPunchInActive.load(std::memory_order_relaxed) && (STATE_PUNCHEDIN != state))
		return;

	_isPunchInActive.store(false, std::memory_order_release);
	if (STATE_PUNCHEDIN == state)
		_state.store(STATE_OVERDUBBING, std::memory_order_release);

	for (auto& loop : _loops)
	{
		loop->PunchOut();
	}

	auto nextState = _state.load(std::memory_order_relaxed);
	auto canFinishRecording = ((STATE_PLAYINGRECORDING == nextState) ||
		(STATE_OVERDUBBINGRECORDING == nextState)) &&
		(_endRecordSampCount >= _endRecordSamps);
	if (canFinishRecording)
		_endRecordingCompleted = true;
}

void LoopTake::SetRackVisibility(bool visible)
{
	// Set the visibility of the LoopTake rack
	_guiRack->SetVisible(visible);
}

gui::GuiRackParams::RackState LoopTake::GetRackState() const
{
	return _guiRack ? _guiRack->GetRackState() : gui::GuiRackParams::RACK_MASTER;
}

void LoopTake::CollapseRackToMaster()
{
	if (_guiRack && _guiRack->GetRackState() != gui::GuiRackParams::RACK_MASTER)
		_guiRack->SetRackState(gui::GuiRackParams::RACK_MASTER, true);
}

void LoopTake::CollapseRouterToChannels()
{
	if (_guiRack && _guiRack->GetRackState() == gui::GuiRackParams::RACK_ROUTER)
		_guiRack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
}

unsigned int LoopTake::_CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops)
{
	if (0 == numLoops)
		return 0;

	auto minHeight = 5;
	int totalGap = (numLoops + 1) * (int)_Gap.Width;
	int height = (((int)takeHeight) - totalGap) / (int)numLoops;

	if (height < minHeight)
		return minHeight;

	return height;
}

void LoopTake::_InitReceivers()
{
	_guiRack->SetReceiver(ActionReceiver::shared_from_this());
}

void LoopTake::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	
	_ArrangeChildren();
}

std::vector<JobAction> LoopTake::_CommitChanges()
{
	bool audioStateChanged = false;
	if (_flipLoopBuffer)
	{
		_flipLoopBuffer = false;
		audioStateChanged = true;

		// Remove and add any children
		// (difference between back and front LoopTake buffer)
		std::vector<std::shared_ptr<Loop>> toAdd;
		std::vector<std::shared_ptr<Loop>> toRemove;
		std::copy_if(_backLoops.begin(), _backLoops.end(), std::back_inserter(toAdd), [&](const std::shared_ptr<Loop>& loop) { return (std::find(_loops.begin(), _loops.end(), loop) == _loops.end()); });
		std::copy_if(_loops.begin(), _loops.end(), std::back_inserter(toRemove), [&](const std::shared_ptr<Loop>& loop) { return (std::find(_backLoops.begin(), _backLoops.end(), loop) == _backLoops.end()); });

		for (auto& loop : toAdd)
		{
			loop->SetParent(GuiElement::shared_from_this());
			loop->Init();
			_children.push_back(loop);
		}

		for (auto& loop : toRemove)
		{
			auto child = std::find(_children.begin(), _children.end(), loop);
			if (_children.end() != child)
				_children.erase(child);
		}

		// Re-index children so _index values are consistent with TryGetChild lookups
		for (unsigned int i = 0; i < _children.size(); i++)
			_children[i]->SetIndex(i);

		_loops = _backLoops; // TODO: Undo?
		_audioBuffers = _backAudioBuffers;
		_audioMixers = _backAudioMixers;
		_ResizeVstScratch((unsigned int)_audioBuffers.size());

		_guiRack->SetNumInputChannels((unsigned int)_loops.size());

		_WireVuSliders();
	}
	if (audioStateChanged)
		_PublishAudioState();

	// Swap in the VST chain when the job thread has delivered a new one.
	if (_flipVstChain.exchange(false, std::memory_order_acquire))
	{
		_vstChain.store(_backVstChain, std::memory_order_release);
	}

	std::vector<JobAction> jobs;

	if (_loopsNeedUpdating)
	{
		_loopsNeedUpdating = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_UPDATELOOPS;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	if (_endRecordingCompleted)
	{
		_endRecordingCompleted = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_ENDRECORDING;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	if (_midiQuantisationUpdatePending)
	{
		_midiQuantisationUpdatePending = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_UPDATEMIDIQUANTISATION;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		job.MidiLoops = _midiLoops;
		jobs.push_back(job);
	}

	// Drain pending unload indices — emit one JOB_UNLOADVST per entry so
	// rapid back-to-back UnloadVstPlugin() calls are not silently dropped.
	auto pendingUnloads = std::move(_pendingVstUnloads);
	for (auto idx : pendingUnloads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_UNLOADVST;
		job.SourceId = Id();
		job.VstIndex = idx;
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	auto pendingLoads = std::move(_pendingVstLoads);
	for (auto& path : pendingLoads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.SourceId = Id();
		job.VstPath = std::move(path);
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}

	GuiElement::_CommitChanges();

	return jobs;
}

const std::shared_ptr<AudioSink> LoopTake::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return nullptr;

	const auto& loops = state->Loops;
	const auto& audioBuffers = state->AudioBuffers;

	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		if (channel < loops.size())
			return loops[channel];

		break;
	case Audible::AUDIOSOURCE_LOOPS:
		if (channel < audioBuffers.size())
			return audioBuffers[channel];

		break;
	case Audible::AUDIOSOURCE_MIXER:
		if (channel < audioBuffers.size())
			return audioBuffers[channel];

		break;
	}

	return nullptr;
}

std::shared_ptr<const LoopTake::AudioState> LoopTake::_AudioStateSnapshot() const
{
	return _audioState.load(std::memory_order_acquire);
}

void LoopTake::_PublishAudioState()
{
	auto state = std::make_shared<AudioState>();
	state->Loops = _loops;
	state->AudioMixers = _audioMixers;
	state->AudioBuffers = _audioBuffers;
	state->VstBlockScratch.resize(state->AudioBuffers.size() * constants::MaxBlockSize);
	state->VstBlockPtrs.resize(state->AudioBuffers.size(), nullptr);
	for (auto i = 0u; i < state->AudioBuffers.size(); i++)
		state->VstBlockPtrs[i] = state->VstBlockScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);
	_audioState.store(state, std::memory_order_release);
}

void LoopTake::_ArrangeChildren()
{
	auto numLoops = (unsigned int)_backLoops.size();
	auto numMidiLoops = 0u;
	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop && midiLoop->Model())
			numMidiLoops++;
	}
	auto numVisualRings = numLoops + numMidiLoops;

	if (0 == numVisualRings)
		return;

	utils::Size2d loopSize = { _sizeParams.Size.Width - (2 * _Gap.Width), _sizeParams.Size.Height - (2 * _Gap.Height) };

	auto loopCount = 0u;
	auto dScale = 0.1;
	auto dTotalScale = 0.4 / ((double)numVisualRings);

	for (auto& loop : _backLoops)
	{
		loop->SetPosition({ (int)_Gap.Width + ((int)loopSize.Width * (int)loop->LoopChannel()), (int)_Gap.Height });
		loop->SetSize(loopSize);
		loop->SetModelPosition({ 0.0f, 0.0f, 0.0f });
		loop->SetModelScale(1.0 + (loopCount * dScale) - (dTotalScale * 0.5));

		loopCount++;
	}

	for (auto midiLoopIndex = 0u; midiLoopIndex < _midiLoops.size(); ++midiLoopIndex)
	{
		auto& midiLoop = _midiLoops[midiLoopIndex];
		if (!midiLoop)
			continue;

		auto midiModel = midiLoop->Model();
		if (!midiModel)
			continue;

		midiModel->SetModelPosition({ 0.0f, 0.0f, 0.0f });
		midiModel->SetModelScale(1.0 + (loopCount * dScale) - (dTotalScale * 0.5));

		loopCount++;
	}
}

GuiRackParams LoopTake::_GetRackParams(utils::Size2d size)
{
	GuiRackParams rackParams;
	rackParams.Position = { 0, 0 };
	rackParams.Size = size;
	rackParams.MinSize = rackParams.Size;
	rackParams.NumInputChannels = NumInputChannels(Audible::AUDIOSOURCE_LOOPS);
	rackParams.NumOutputChannels = NumOutputChannels(Audible::AUDIOSOURCE_LOOPS);
	rackParams.InitLevel = 1.0;
	rackParams.InitState = gui::GuiRackParams::RACK_MASTER;

	return rackParams;
}

void LoopTake::_UpdateLoops()
{
	for (auto& loop : _loops)
	{
		loop->UpdateCapacity();
		loop->RefreshVisualModel();
	}

	_UpdateMidiModels(false);
}

void LoopTake::_UpdateMidiModels(bool force)
{
	const auto displayLength = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop)
			midiLoop->UpdateModelFromEvents(displayLength, force);
	}
}

void LoopTake::_UpdateMidiModelRotation()
{
	double loopIndexFrac = 0.0;
	const auto state = _state.load(std::memory_order_relaxed);
	const auto isRecording = (STATE_RECORDING == state) ||
		(STATE_OVERDUBBING == state) ||
		(STATE_PUNCHEDIN == state);

	if (isRecording)
	{
		loopIndexFrac = 0.0;
	}
	else if (!_loops.empty())
	{
		loopIndexFrac = _loops.front()->LoopIndexFrac();
	}
	else if (_midiVisualLoopLength > 0ul)
	{
		loopIndexFrac = 1.0 - std::max(0.0, std::min(1.0,
			((double)(_midiVisualPlayIndex % _midiVisualLoopLength)) / ((double)_midiVisualLoopLength)));
	}

	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop && midiLoop->Model())
			midiLoop->Model()->SetLoopIndexFrac(loopIndexFrac);
	}
}

void LoopTake::SetMidiQuantisation(const MidiQuantisationSettings& settings) noexcept
{
	const auto previous = MidiQuantisation();
	_midiQuantisationPacked.store(PackMidiQuantisationSettings(settings), std::memory_order_release);

	if (previous != settings)
	{
		_midiQuantisationUpdatePending = true;
		_changesMade = true;
	}
}

MidiQuantisationSettings LoopTake::MidiQuantisation() const noexcept
{
	return UnpackMidiQuantisationSettings(_midiQuantisationPacked.load(std::memory_order_acquire));
}

std::uint32_t LoopTake::_ResolveMidiQuantisationGestureGrain() const noexcept
{
	for (const auto& midiLoop : _midiLoops)
	{
		if (!midiLoop)
			continue;

		const auto loopLength = midiLoop->LoopLengthSamps();
		if (loopLength > 0u)
			return loopLength;
	}

	for (const auto& loop : _loops)
	{
		if (!loop)
			continue;

		const auto loopLength = static_cast<std::uint32_t>(loop->LoopLength());
		if (loopLength > 0u)
			return loopLength;
	}

	if (_midiVisualLoopLength > 0ul)
		return static_cast<std::uint32_t>(_midiVisualLoopLength);

	return static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
}

void LoopTake::_ApplyMidiQuantisationGesture(MidiQuantisationFraction fraction, bool enabled, [[maybe_unused]] const char* source) noexcept
{
	const auto previous = MidiQuantisation();
	MidiQuantisationSettings updated = previous;
	updated.Enabled = enabled;
	updated.Fraction = fraction;

	const auto resolvedGrain = (enabled && 0u == updated.GrainSamps) ?
		_ResolveMidiQuantisationGestureGrain() :
		updated.GrainSamps;
	if (enabled && resolvedGrain > 0u)
		updated.GrainSamps = resolvedGrain;

	SetMidiQuantisation(updated);
}

void LoopTake::_RemoveMidiModelChildren()
{
	for (auto& midiLoop : _midiLoops)
	{
		if (!midiLoop || !midiLoop->Model())
			continue;

		auto midiModel = midiLoop->Model();
		auto child = std::find(_children.begin(), _children.end(), midiModel);
		if (_children.end() != child)
			_children.erase(child);
	}

	for (auto i = 0u; i < _children.size(); i++)
		_children[i]->SetIndex(i);
}

void LoopTake::_WireVuSliders()
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

void LoopTake::_ResizeVstScratch(unsigned int channelCount)
{
	_vstBlockScratch.resize(static_cast<size_t>(channelCount) * constants::MaxBlockSize);
	_vstBlockPtrs.resize(channelCount);
	for (unsigned int i = 0; i < channelCount; i++)
		_vstBlockPtrs[i] = _vstBlockScratch.data() + (static_cast<size_t>(i) * constants::MaxBlockSize);
}

void LoopTake::SetSampleRate(float sampleRate)
{
	_sampleRate = sampleRate;
	for (auto& loop : _loops)
		loop->SetSampleRate(sampleRate);
	for (auto& loop : _backLoops)
		loop->SetSampleRate(sampleRate);
}

void LoopTake::LoadVstPlugin(std::wstring path)
{
	_pendingVstLoads.push_back(std::move(path));
	_changesMade = true;
}

void LoopTake::UnloadVstPlugin(size_t index)
{
	_pendingVstUnloads.push_back(index);
	_changesMade = true;
}

std::shared_ptr<vst::IVstPlugin> LoopTake::GetVstPlugin(size_t index) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain)
		return nullptr;

	return chain->GetPlugin(index);
}

std::vector<io::JamFile::VstEntry> LoopTake::VstEntries() const
{
	std::vector<io::JamFile::VstEntry> entries;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	entries.reserve(_vstPluginPaths.size());

	for (const auto& path : _vstPluginPaths)
	{
		io::JamFile::VstEntry entry;
		entry.Path = utils::EncodeUtf8(path);
		entry.Bypass = false;
		entries.push_back(std::move(entry));
	}

	return entries;
}
