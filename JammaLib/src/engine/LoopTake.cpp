#include "LoopTake.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "../graphics/MidiModel.h"
#include "../midi/MidiNote.h"
#include "../midi/MidiIndexedOutputSink.h"

namespace
{
	bool HasMidiQuantisationGestureModifiers(base::Action::Modifiers modifiers) noexcept
	{
		return (base::Action::MODIFIER_CTRL & modifiers);
	}

	unsigned long NormalizeLoopIndex(long long index, unsigned long loopLength) noexcept
	{
		if (0ul == loopLength)
			return 0ul;

		const auto length = static_cast<long long>(loopLength);
		auto normalized = index % length;
		if (normalized < 0)
			normalized += length;
		return static_cast<unsigned long>(normalized);
	}

	std::uint32_t NormalizeMidiLoopOffset(std::uint32_t offset,
		std::uint32_t loopLength) noexcept
	{
		if (0u == loopLength)
			return 0u;

		return offset % loopLength;
	}

	unsigned long InitialMidiPlayIndex(unsigned long loopLength,
		int midiQuantisationErrorSamps) noexcept
	{
		return NormalizeLoopIndex(static_cast<long long>(midiQuantisationErrorSamps), loopLength);
	}

	bool AppendMidiEvent(const midi::MidiEvent& event,
		midi::MidiEvent* outEvents,
		std::size_t outCapacity,
		std::size_t& outCount) noexcept
	{
		if (!outEvents || outCount >= outCapacity)
			return false;

		outEvents[outCount++] = event;
		return true;
	}

	std::size_t BuildRebasedMidiOverdubSourceEvents(const midi::MidiOverdubLoopState& state,
		midi::MidiEvent* outEvents,
		std::size_t outCapacity) noexcept
	{
		if (!outEvents || 0u == outCapacity || 0u == state.SourceLoopLengthSamps)
			return 0u;

		const auto sourceLoopLength = state.SourceLoopLengthSamps;
		const auto sourceStart = NormalizeMidiLoopOffset(state.SourceStartSample, sourceLoopLength);
		const auto rebaseOffset = (sourceLoopLength - sourceStart) % sourceLoopLength;
		std::size_t outCount = 0u;

		for (std::size_t i = 0u; i < state.SourceEventCount; ++i)
		{
			const auto& sourceEvent = state.SourceEvents[i];
			if (sourceEvent.sampleOffset >= sourceLoopLength)
				continue;
			if (sourceEvent.IsNoteOn() || sourceEvent.IsNoteOff())
				continue;

			auto mapped = sourceEvent;
			mapped.sampleOffset = NormalizeMidiLoopOffset(sourceEvent.sampleOffset + rebaseOffset, sourceLoopLength);
			if (!AppendMidiEvent(mapped, outEvents, outCapacity, outCount))
				return outCount;
		}

		const auto noteSpans = midi::MidiNote::ExtractSpans(
			state.SourceEvents.data(),
			state.SourceEventCount,
			sourceLoopLength);

		for (const auto& span : noteSpans)
		{
			const auto spanStart = span.StartSample;
			auto spanEnd = span.StartSample + span.DurationSamples;
			if (spanEnd > sourceLoopLength)
				spanEnd = sourceLoopLength;
			if (spanEnd <= spanStart)
				continue;

			const auto mappedStart = NormalizeMidiLoopOffset(spanStart + rebaseOffset, sourceLoopLength);
			const auto mappedEnd = NormalizeMidiLoopOffset(spanEnd + rebaseOffset, sourceLoopLength);

			if (mappedStart < mappedEnd)
			{
				if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOn(mappedStart, span.Channel, span.Note, span.Velocity), outEvents, outCapacity, outCount))
					return outCount;
				if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOff(mappedEnd, span.Channel, span.Note), outEvents, outCapacity, outCount))
					return outCount;
				continue;
			}

			if (mappedStart > mappedEnd)
			{
				if (mappedEnd > 0u)
				{
					if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOn(0u, span.Channel, span.Note, span.Velocity), outEvents, outCapacity, outCount))
						return outCount;
					if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOff(mappedEnd, span.Channel, span.Note), outEvents, outCapacity, outCount))
						return outCount;
				}

				if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOn(mappedStart, span.Channel, span.Note, span.Velocity), outEvents, outCapacity, outCount))
					return outCount;
				continue;
			}

			if (!AppendMidiEvent(midi::MidiEvent::MakeNoteOn(0u, span.Channel, span.Note, span.Velocity), outEvents, outCapacity, outCount))
				return outCount;
		}

		midi::MidiNote::SortMidiEvents(outEvents, outCount);
		return outCount;
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
	_isMidiPunchInActive(false),
	_guiRack(nullptr),
	_masterMixer(nullptr),
	_loops(),
	_backLoops(),
	_midiRecordHeld(),
	_midiQuantisationPacked(midi::MidiQuantisationSettings().Pack()),
	_midiTakePhaseOffsetSamps(0),
	_midiInheritedPhaseOffsetSamps(0),
	_midiTransportStartSamps(0u),
	_midiQuantisationUpdatePending(false),
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
	midi::MidiQuantisationSettings quantisation = take->MidiQuantisation();
	quantisation.PhaseOffsetSamps = takeStruct.TakePhaseOffsetSamps;
	take->SetMidiQuantisation(quantisation);

	LoopParams loopParams;
	loopParams.Wav = "hh";

	for (auto loopStruct : takeStruct.Loops)
	{
		auto loop = Loop::FromFile(loopParams, loopStruct, dir);
		
		if (loop.has_value())
		{
			for (const auto& vstEntry : loopStruct.VstChain)
				loop.value()->LoadVstPlugin(utils::DecodeUtf8(vstEntry.Path), vstEntry.DecodeState());

			take->AddLoop(loop.value());
		}
	}

	for (const auto& vstEntry : takeStruct.VstChain)
		take->LoadVstPlugin(utils::DecodeUtf8(vstEntry.Path), vstEntry.DecodeState());

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
	const auto takeVisualScale = static_cast<float>(_masterMixer ? _masterMixer->UnmutedLevel() : 1.0);

	for (size_t i = 0; i < _loops.size(); ++i)
	{
		const auto channelVisualScale = (i < _audioMixers.size() && _audioMixers[i]) ?
			static_cast<float>(_audioMixers[i]->UnmutedLevel()) :
			1.0f;
		_loops[i]->SetMasterVisualScale(takeVisualScale * channelVisualScale * _parentVisualScale);
	}

	for (size_t i = 0; i < _backLoops.size(); ++i)
	{
		const auto channelVisualScale = (i < _backAudioMixers.size() && _backAudioMixers[i]) ?
			static_cast<float>(_backAudioMixers[i]->UnmutedLevel()) :
			1.0f;
		_backLoops[i]->SetMasterVisualScale(takeVisualScale * channelVisualScale * _parentVisualScale);
	}

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

	for (const auto& weakLoop : state->Loops)
		if (auto loop = weakLoop.lock()) loop->Zero(numSamps);
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

	for (const auto& weakLoop : state->Loops)
		if (auto loop = weakLoop.lock())
			loop->WriteBlock(loopDest, trigger, indexOffset, numSamps);

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

void LoopTake::EndMultiPlay(unsigned int numSamps)
{
	auto state = _AudioStateSnapshot();
	if (!state)
		return;

	for (const auto& weakLoop : state->Loops)
		if (auto loop = weakLoop.lock()) loop->EndMultiPlay(numSamps);

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

	for (const auto& weakLoop : audioState->Loops)
		if (auto loop = weakLoop.lock()) loop->EndWrite(numSamps, updateIndex);

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

	if (_IsGestureActive(GestureKind::MidiQuantisation))
	{
		if (actions::TouchAction::TOUCH_UP == action.State)
		{
			const auto quantisation = MidiQuantisation();
			if (!_GestureState().Moved)
				_ApplyMidiQuantisationGesture(midi::MidiQuantisationGesture::Toggle, quantisation.Fraction, "click-toggle");

			_EndGesture();
			return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
		}

		return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	}

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

	const auto quantisation = MidiQuantisation();
	_BeginGesture(GestureKind::MidiQuantisation,
		action.Position,
		midi::MidiQuantisation::FractionIndex(quantisation.Fraction));

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

	if (!_IsGestureActive(GestureKind::MidiQuantisation))
		return Jammable::OnAction(action);

	_MarkGestureMoved();

	action = GlobalToLocal(action);
	const auto delta = action.Position - _GestureState().StartPosition;
	const auto startFraction = midi::MidiQuantisation::ClampFractionIndex(_GestureState().StartValue);
	const auto fraction = midi::MidiQuantisation::ResolveDragFraction(startFraction, delta.Y);

	_ApplyMidiQuantisationGesture(midi::MidiQuantisationGesture::DragFraction, fraction, "drag-fraction");
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
			const auto previous = MidiQuantisation();
			const auto updated = midi::MidiQuantisation::ApplyGuiPayload(previous,
				arr->Values.data(),
				arr->Values.size());
			if (_loggingConfig.Ui == "verbose" && previous.Fraction != updated.Fraction)
				_LogMidiQuantisationFractionChange(previous.Fraction, updated.Fraction, "gui-action");
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
			// Restore saved state before adding to the chain.
			if (!action.VstInitialState.empty())
				plugin->SetState(action.VstInitialState);

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
		const auto settings = ResolvedMidiQuantisation();
		for (auto& midiLoop : action.MidiLoops)
		{
			if (midiLoop)
				midiLoop->SetQuantisation(settings);
		}

		const auto displayLength = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
		for (auto& midiLoop : action.MidiLoops)
		{
			if (midiLoop)
				midiLoop->QueueModelUpdateFromEvents(displayLength, true);
		}

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

unsigned long LoopTake::VisualLoopLengthSamps() const noexcept
{
	auto length = 0ul;
	for (const auto& loop : _loops)
	{
		if (loop)
			length = std::max(length, loop->LoopLength());
	}

	if (length > 0ul)
		return length;

	return _midiVisualLoopLength;
}

double LoopTake::LoopIndexFrac() const noexcept
{
	const auto state = _state.load(std::memory_order_relaxed);
	const auto isRecording = (STATE_RECORDING == state) ||
		(STATE_OVERDUBBING == state) ||
		(STATE_PUNCHEDIN == state);

	if (isRecording)
		return 0.0;

	std::shared_ptr<Loop> representativeLoop;
	for (const auto& loop : _loops)
	{
		if (!loop)
			continue;

		if (!representativeLoop || (loop->LoopLength() > representativeLoop->LoopLength()))
			representativeLoop = loop;
	}

	if (representativeLoop)
		return representativeLoop->LoopIndexFrac();

	if (_midiVisualLoopLength > 0ul)
	{
		return 1.0 - std::max(0.0, std::min(1.0,
			((double)(_midiVisualPlayIndex % _midiVisualLoopLength)) / ((double)_midiVisualLoopLength)));
	}

	return 0.0;
}

float LoopTake::VisualRadius() const noexcept
{
	return static_cast<float>(Loop::CalcDrawRadius(VisualLoopLengthSamps()));
}

std::optional<QuantisationLoopTakeVisual> LoopTake::QuantisationVisual() const noexcept
{
	const auto loopLengthSamps = VisualLoopLengthSamps();
	if (loopLengthSamps == 0ul)
		return std::nullopt;

	const auto resolvedQuantisation = ResolvedMidiQuantisation();
	const auto grainSamps = resolvedQuantisation.GrainSamps;
	const auto loopGrains = (grainSamps > 0u && (loopLengthSamps % grainSamps) == 0ul) ?
		static_cast<std::uint32_t>(loopLengthSamps / grainSamps) :
		0u;

	const auto takeSize = GetSize();
	const auto takePos = ModelPosition();
	return QuantisationLoopTakeVisual{
		loopLengthSamps,
		grainSamps,
		loopGrains,
		LoopIndexFrac(),
		takePos.Y,
		std::max(8.0f, static_cast<float>(takeSize.Height) * 0.45f),
		VisualRadius(),
		MidiQuantisation().Fraction,
		resolvedQuantisation.PhaseOffsetSamps
	};
}

std::vector<QuantisationLoopTakeVisual> LoopTake::QuantisationVisualsFor(
	const std::vector<std::shared_ptr<LoopTake>>& takes)
{
	std::vector<QuantisationLoopTakeVisual> visuals;
	visuals.reserve(takes.size());

	for (const auto& take : takes)
	{
		if (!take)
			continue;

		if (auto visual = take->QuantisationVisual(); visual.has_value())
			visuals.push_back(visual.value());
	}

	return visuals;
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
	std::vector<std::string> midiDevices,
	std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> heldAtStart,
	std::uint64_t transportStartSamps)
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
	_isMidiPunchInActive.store(false, std::memory_order_relaxed);
	_midiRecordHeld.clear();
	_midiTransportStartSamps.store(transportStartSamps, std::memory_order_release);
	_backLoops.clear();
	_RemoveMidiModelChildren();
	_ResetMidiOverdubSession();

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
			auto midiLoop = std::make_shared<midi::MidiLoop>();
			graphics::MidiModelParams modelParams;
			modelParams.ModelScale = 1.0f;
			auto midiModel = std::make_shared<graphics::MidiModel>(modelParams);
			midiLoop->AttachModel(midiModel);
			midiLoop->StartRecord();
			midiLoop->SetQuantisation(ResolvedMidiQuantisation());
			_midiLoops.push_back(midiLoop);
			_midiLoopChannels.push_back(midiChan);
			_midiLoopDevices.push_back(midiDevice);
			_children.push_back(midiModel);
		}
	}
	_midiRecordHeld.assign(_midiLoops.size(), midi::MidiNoteSnapshot{});

	for (std::size_t i = 0u; i < _midiLoops.size(); ++i)
	{
		const auto midiChan = static_cast<std::uint8_t>(_midiLoopChannels[i] & midi::MidiEvent::ChannelMask);
		const auto& dev = _midiLoopDevices[i];
		auto it = std::find_if(heldAtStart.begin(), heldAtStart.end(),
			[&dev](const auto& p) { return p.first == dev; });
		if (it == heldAtStart.end())
			continue;
		const auto& snap = it->second;
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			const auto slot = midi::MidiNote::NoteSlot(midiChan, note);
			if (snap.Held.test(slot) && snap.Velocity[slot] > 0u)
				RecordMidiEvent(midi::MidiEvent::MakeNoteOn(0u, midiChan, note, snap.Velocity[slot]), dev, 0u);
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

bool LoopTake::RecordMidiEvent(const midi::MidiEvent& ev, std::uint32_t globalSampleNow) noexcept
{
	return RecordMidiEvent(ev, "", globalSampleNow);
}

bool LoopTake::RecordMidiEvent(const midi::MidiEvent& ev,
	const std::string& device,
	std::uint32_t globalSampleNow) noexcept
{
	if (_midiLoops.empty())
		return false;

	const auto midiChan = ev.Channel();
	const auto noteSlot = midi::MidiLoop::NoteSlot(midiChan, ev.data1);
	bool recorded = false;
	const auto recordedNow = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
	const auto isOverdubCapture = _midiOverdubSession.Active;
	const auto punchActive = _isMidiPunchInActive.load(std::memory_order_relaxed);

	for (auto i = 0u; i < _midiLoops.size(); ++i)
	{
		if (_midiLoopChannels[i] != midiChan)
			continue;

		if (!_midiLoopDevices[i].empty() && (_midiLoopDevices[i] != device))
			continue;

		if (_midiLoops[i]->State() != midi::MidiLoopState::Recording)
			continue;

		if (isOverdubCapture)
		{
			if (i < _midiOverdubSession.Loops.size())
			{
				auto& overdub = _midiOverdubSession.Loops[i];
				if (ev.IsNoteOn())
					overdub.LiveHeld.Velocity[noteSlot] = ev.data2;
				else if (ev.IsNoteOff())
					overdub.LiveHeld.Velocity[noteSlot] = 0u;

				if (ev.IsNoteOn())
					overdub.LiveHeld.Held.set(noteSlot);
				else if (ev.IsNoteOff())
					overdub.LiveHeld.Held.reset(noteSlot);

				if (!punchActive)
					continue;

				midi::MidiEvent stamped = ev;
				stamped.sampleOffset = ResolveMidiRecordSample(ev.sampleOffset, globalSampleNow, recordedNow);
				if (stamped.sampleOffset < overdub.ActivePunchStart)
					stamped.sampleOffset = overdub.ActivePunchStart;

				recorded = _AppendMidiOverdubLiveEvent(i, stamped) || recorded;
			}

			continue;
		}

		midi::MidiEvent stamped = ev;
		stamped.sampleOffset = ResolveMidiRecordSample(ev.sampleOffset, globalSampleNow, recordedNow);
		if (i < _midiRecordHeld.size())
		{
			auto& held = _midiRecordHeld[i];
			if (ev.IsNoteOn())
				held.Set(midiChan, ev.data1, ev.data2);
			else if (ev.IsNoteOff())
				held.Clear(midiChan, ev.data1);
		}
		if (!_midiLoops[i]->RecordEvent(stamped))
			continue;

		// Drive visual updates directly from MIDI ingress so note rendering does
		// not depend on audio-loop update cadence.
		const auto displayLength = (stamped.sampleOffset < std::numeric_limits<std::uint32_t>::max())
			? std::max(recordedNow, stamped.sampleOffset + 1u)
			: std::max(recordedNow, stamped.sampleOffset);
		_midiLoops[i]->QueueModelUpdateFromEvents(displayLength, false);
		recorded = true;
	}

	return recorded;
}

std::vector<midi::MidiEvent> LoopTake::BuildMidiPunchInLiveTransitionEvents(std::uint32_t punchSample) const
{
	return _BuildMidiLiveTransitionEvents(punchSample, true);
}

std::vector<midi::MidiEvent> LoopTake::BuildMidiPunchOutLiveTransitionEvents(std::uint32_t punchSample) const
{
	return _BuildMidiLiveTransitionEvents(punchSample, false);
}

std::vector<midi::MidiEvent> LoopTake::_BuildMidiLiveTransitionEvents(std::uint32_t punchSample,
	bool isPunchInTransition) const
{
	midi::MidiNoteSnapshot sourceSnapshot;
	midi::MidiNoteSnapshot liveSnapshot;
	_SnapshotPunchBoundaryMidi(punchSample, sourceSnapshot, liveSnapshot);

	std::vector<midi::MidiEvent> events;
	for (std::uint8_t channel = 0u; channel < 16u; ++channel)
	{
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			const auto slot = midi::MidiNote::NoteSlot(channel, note);
			if (isPunchInTransition)
			{
				if (sourceSnapshot.Held.test(slot) && !liveSnapshot.Held.test(slot))
					events.push_back(midi::MidiEvent::MakeNoteOff(punchSample, channel, note));
				continue;
			}

			if (liveSnapshot.Held.test(slot))
				events.push_back(midi::MidiEvent::MakeNoteOff(punchSample, channel, note));
		}
	}

	if (isPunchInTransition)
		return events;

	for (std::uint8_t channel = 0u; channel < 16u; ++channel)
	{
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			const auto slot = midi::MidiNote::NoteSlot(channel, note);
			if (sourceSnapshot.Held.test(slot))
				events.push_back(midi::MidiEvent::MakeNoteOn(punchSample, channel, note, sourceSnapshot.Velocity[slot]));
		}
	}

	return events;
}

unsigned int LoopTake::ReadMidiBlock(std::uint32_t globalSample,
	std::uint32_t numSamples,
	midi::IMidiOutputSink& sink,
	unsigned int firstOutputIndex) noexcept
{
	if (IsMuted())
		return static_cast<unsigned int>(_midiLoops.size());

	const auto midiBlockStart = static_cast<std::uint32_t>(_midiVisualPlayIndex);

	for (auto i = 0u; i < _midiLoops.size(); ++i)
	{
		if (!_midiLoops[i])
			continue;

		midi::MidiIndexedOutputSink indexedSink(sink, firstOutputIndex + i, midiBlockStart, globalSample);
		_midiLoops[i]->ReadBlock(midiBlockStart, numSamples, indexedSink);
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
	unsigned int endRecordSamps,
	int midiQuantisationErrorSamps)
{
	auto state = _state.load(std::memory_order_relaxed);
	if ((STATE_RECORDING != state) &&
		(STATE_OVERDUBBING != state) &&
		(STATE_PUNCHEDIN != state))
		return;

	_endRecordSampCount = 0;
	_endRecordSamps = endRecordSamps;
	_midiVisualLoopLength = loopLength;

	_midiVisualPlayIndex = InitialMidiPlayIndex(loopLength, midiQuantisationErrorSamps);
	if (_loggingConfig.Ui == "verbose")
	{
		const char* triggerType = (STATE_RECORDING == state) ? "record-end" : "overdub-end";
		std::cout << "MIDI first-play timing: take=" << _id
			<< " trigger=" << triggerType
			<< " loopLength=" << loopLength
			<< " errorSamps=" << midiQuantisationErrorSamps
			<< " audioPlayPos=" << index
			<< " audioEndRecordSamps=" << endRecordSamps
			<< " midiStart=" << _midiVisualPlayIndex
			<< '\n';
	}

	auto continueCapture = (endRecordSamps > 0) || _isPunchInActive.load(std::memory_order_relaxed);

	for (auto& loop : _loops)
	{
		loop->Play(index, loopLength, continueCapture);
	}

	const auto midiLoopLength = static_cast<std::uint32_t>(loopLength);
	if (_midiOverdubSession.Active)
	{
		if (_isMidiPunchInActive.load(std::memory_order_relaxed))
		{
			_CloseMidiPunchWindow(midiLoopLength, true);
			_isMidiPunchInActive.store(false, std::memory_order_relaxed);
			if (STATE_PUNCHEDIN == state)
				state = STATE_OVERDUBBING;
		}

		for (std::size_t i = 0u; i < _midiLoops.size(); ++i)
			_FinalizeMidiOverdubLoop(i, midiLoopLength);

		_ResetMidiOverdubSession();
	}
	else
	{
		for (std::size_t i = 0u; i < _midiLoops.size(); ++i)
		{
			auto midiLoop = _midiLoops[i];
			if (!midiLoop || midiLoop->State() != midi::MidiLoopState::Recording)
				continue;
			if (i >= _midiRecordHeld.size())
				continue;

			const auto channel = static_cast<std::uint8_t>(
				(i < _midiLoopChannels.size()) ?
				(_midiLoopChannels[i] & midi::MidiEvent::ChannelMask) :
				0u);
			const auto& held = _midiRecordHeld[i];
			const auto heldNoteOffSample = (midiLoopLength > 0u) ? (midiLoopLength - 1u) : 0u;
			for (std::uint8_t note = 0u; note < 128u; ++note)
			{
				const auto slot = midi::MidiNote::NoteSlot(channel, note);
				if (!held.Held.test(slot))
					continue;

				midiLoop->RecordEvent(midi::MidiEvent::MakeNoteOff(heldNoteOffSample, channel, note));
			}
		}
	}

	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop->State() == midi::MidiLoopState::Recording)
		{
			midiLoop->EndRecord(midiLoopLength);
			midiLoop->UpdateModelFromEvents(midiLoopLength, true);
		}
	}

	_midiRecordHeld.clear();

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
	_isMidiPunchInActive.store(false, std::memory_order_relaxed);
	_midiRecordHeld.clear();
	_midiTransportStartSamps.store(0u, std::memory_order_release);
	_ResetMidiOverdubSession();

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
	_midiRecordHeld.clear();
}

void LoopTake::Overdub(std::vector<unsigned int> channels,
	std::string stationName,
	std::vector<unsigned int> midiChannels,
	std::vector<std::string> midiDevices,
	std::shared_ptr<LoopTake> sourceTake,
	std::uint64_t transportStartSamps)
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
	_isMidiPunchInActive.store(false, std::memory_order_relaxed);
	_midiRecordHeld.clear();
	_midiTransportStartSamps.store(transportStartSamps, std::memory_order_release);
	_backLoops.clear();
	_RemoveMidiModelChildren();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan, stationName);
		loop->Overdub();
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
			auto midiLoop = std::make_shared<midi::MidiLoop>();
			graphics::MidiModelParams modelParams;
			modelParams.ModelScale = 1.0f;
			auto midiModel = std::make_shared<graphics::MidiModel>(modelParams);
			midiLoop->AttachModel(midiModel);
			midiLoop->StartRecord();
			midiLoop->SetQuantisation(ResolvedMidiQuantisation());
			_midiLoops.push_back(midiLoop);
			_midiLoopChannels.push_back(midiChan);
			_midiLoopDevices.push_back(midiDevice);
			_children.push_back(midiModel);
		}
	}

	_InitMidiOverdubSession(sourceTake);

	if (!midiChannels.empty())
	{
		Init();
		_ArrangeChildren();
	}

	_loopsNeedUpdating = true;
	_changesMade = true;
}

void LoopTake::PunchIn(bool applyAudio, bool applyMidi)
{
	auto state = _state.load(std::memory_order_relaxed);
	const auto canPunchAudio = (STATE_OVERDUBBING == state) ||
		(STATE_OVERDUBBINGRECORDING == state) ||
		(STATE_PLAYING == state);
	const auto canPunchMidi = applyMidi && _midiOverdubSession.Active;
	if (!canPunchAudio && !canPunchMidi)
		return;

	if (applyMidi && canPunchMidi && !_isMidiPunchInActive.load(std::memory_order_relaxed))
	{
		_isMidiPunchInActive.store(true, std::memory_order_release);
		const auto punchSample = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
		_OpenMidiPunchWindow(punchSample);
	}

	if (!applyAudio || !canPunchAudio)
		return;

	_isPunchInActive.store(true, std::memory_order_release);
	if (STATE_OVERDUBBING == state)
		_state.store(STATE_PUNCHEDIN, std::memory_order_release);

	for (auto& loop : _loops)
	{
		loop->PunchIn();
	}
}

void LoopTake::PunchOut(bool applyAudio, bool applyMidi)
{
	auto state = _state.load(std::memory_order_relaxed);
	const auto hasAudioPunch = _isPunchInActive.load(std::memory_order_relaxed) || (STATE_PUNCHEDIN == state);
	const auto hasMidiPunch = _isMidiPunchInActive.load(std::memory_order_relaxed);
	if ((!applyAudio || !hasAudioPunch) && (!applyMidi || !hasMidiPunch))
		return;

	if (applyMidi && hasMidiPunch)
	{
		_isMidiPunchInActive.store(false, std::memory_order_release);
		if (_midiOverdubSession.Active)
		{
		const auto punchSample = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
		_CloseMidiPunchWindow(punchSample, true);
		}
	}

	if (!applyAudio || !hasAudioPunch)
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

std::int32_t LoopTake::_ClampPhaseOffset(std::int64_t offsetSamps) noexcept
{
	if (offsetSamps > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
		return std::numeric_limits<std::int32_t>::max();
	if (offsetSamps < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()))
		return std::numeric_limits<std::int32_t>::min();
	return static_cast<std::int32_t>(offsetSamps);
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
	for (auto& [path, initialState] : pendingLoads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.SourceId = Id();
		job.VstPath = std::move(path);
		job.VstInitialState = std::move(initialState);
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
			return loops[channel].lock();

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
	state->Loops.reserve(_loops.size());
	for (const auto& loop : _loops)
		state->Loops.push_back(loop);
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
	if (_midiOverdubSession.Active)
		_RefreshMidiOverdubPreview(displayLength);

	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop)
			midiLoop->UpdateModelFromEvents(displayLength, force);
	}
}

void LoopTake::_UpdateMidiModelRotation()
{
	const auto loopIndexFrac = LoopIndexFrac();

	for (auto& midiLoop : _midiLoops)
	{
		if (midiLoop && midiLoop->Model())
			midiLoop->Model()->SetLoopIndexFrac(loopIndexFrac);
	}
}

void LoopTake::SetMidiQuantisation(const midi::MidiQuantisationSettings& settings) noexcept
{
	const auto previous = MidiQuantisation();
	_midiQuantisationPacked.store(settings.Pack(), std::memory_order_release);
	_midiTakePhaseOffsetSamps.store(settings.PhaseOffsetSamps, std::memory_order_release);

	if (previous != settings)
	{
		_midiQuantisationUpdatePending = true;
		_changesMade = true;
	}
}

midi::MidiQuantisationSettings LoopTake::MidiQuantisation() const noexcept
{
	auto settings = midi::MidiQuantisationSettings::Unpack(
		_midiQuantisationPacked.load(std::memory_order_acquire));
	settings.PhaseOffsetSamps = _midiTakePhaseOffsetSamps.load(std::memory_order_acquire);
	return settings;
}

midi::MidiQuantisationSettings LoopTake::ResolvedMidiQuantisation() const noexcept
{
	auto settings = MidiQuantisation();
	auto combined = static_cast<std::int64_t>(_NaturalMidiQuantisationPhaseOffset(settings))
		+ static_cast<std::int64_t>(settings.PhaseOffsetSamps)
		+ static_cast<std::int64_t>(_midiInheritedPhaseOffsetSamps.load(std::memory_order_acquire));
	settings.PhaseOffsetSamps = _ClampPhaseOffset(combined);
	return settings;
}

void LoopTake::SetMidiQuantisationInheritedPhaseOffset(std::int32_t offsetSamps) noexcept
{
	const auto previous = ResolvedMidiQuantisation();
	_midiInheritedPhaseOffsetSamps.store(offsetSamps, std::memory_order_release);
	const auto updated = ResolvedMidiQuantisation();

	if (previous != updated)
	{
		_midiQuantisationUpdatePending = true;
		_changesMade = true;
	}
}

void LoopTake::SetMidiQuantisationTransportStartSamps(std::uint64_t startSamps) noexcept
{
	const auto previous = ResolvedMidiQuantisation();
	_midiTransportStartSamps.store(startSamps, std::memory_order_release);
	const auto updated = ResolvedMidiQuantisation();

	if (previous != updated)
	{
		_midiQuantisationUpdatePending = true;
		_changesMade = true;
	}
}

std::uint64_t LoopTake::MidiQuantisationTransportStartSamps() const noexcept
{
	return _midiTransportStartSamps.load(std::memory_order_acquire);
}

std::int32_t LoopTake::_NaturalMidiQuantisationPhaseOffset(const midi::MidiQuantisationSettings& settings) const noexcept
{
	const auto stepSamps = midi::MidiQuantisation::StepSamps(settings);
	if (0u == stepSamps)
		return 0;

	const auto transportStartSamps = _midiTransportStartSamps.load(std::memory_order_acquire);
	const auto startWithinStep = transportStartSamps % static_cast<std::uint64_t>(stepSamps);
	if (0u == startWithinStep)
		return 0;

	return _ClampPhaseOffset(-static_cast<std::int64_t>(startWithinStep));
}

midi::MidiQuantisationGrainCandidates LoopTake::_MidiQuantisationGrainCandidates() const noexcept
{
	midi::MidiQuantisationGrainCandidates candidates;
	candidates.ResolvedTakeGrainSamps = ResolvedMidiQuantisation().GrainSamps;
	candidates.PublishedSceneGrainSamps = MidiQuantisation().GrainSamps;

	std::uint32_t representativeLoopLength = 0u;

	for (const auto& midiLoop : _midiLoops)
	{
		if (!midiLoop)
			continue;

		const auto loopLength = midiLoop->LoopLengthSamps();
		if (loopLength > 0u)
		{
			representativeLoopLength = loopLength;
			break;
		}
	}

	for (const auto& loop : _loops)
	{
		if (!loop)
			continue;

		const auto loopLength = static_cast<std::uint32_t>(loop->LoopLength());
		if (loopLength > 0u)
		{
			if (0u == representativeLoopLength)
				representativeLoopLength = loopLength;
			break;
		}
	}

	if ((representativeLoopLength > 0u)
		&& (candidates.ResolvedTakeGrainSamps > 0u)
		&& (representativeLoopLength == candidates.ResolvedTakeGrainSamps))
	{
		candidates.SingleGrainLoopSamps = representativeLoopLength;
	}

	candidates.RecordedSamps = static_cast<std::uint32_t>(_recordedSampCount.load(std::memory_order_relaxed));
	return candidates;
}

midi::MidiNoteSnapshot LoopTake::_SnapshotSourceMidiAtSample(std::size_t loopIndex,
	std::uint32_t targetSample) const noexcept
{
	midi::MidiNoteSnapshot stateSnapshot;
	if (!_midiOverdubSession.Active || loopIndex >= _midiOverdubSession.Loops.size())
		return stateSnapshot;

	const auto& loopState = _midiOverdubSession.Loops[loopIndex];
	if (0u == loopState.SourceLoopLengthSamps)
		return stateSnapshot;

	const auto sourceSample = NormalizeMidiLoopOffset(
		static_cast<std::uint32_t>(loopState.SourceStartSample + targetSample),
		loopState.SourceLoopLengthSamps);

	for (std::size_t eventIndex = 0u; eventIndex < loopState.SourceEventCount; ++eventIndex)
	{
		const auto& event = loopState.SourceEvents[eventIndex];
		if (event.sampleOffset >= loopState.SourceLoopLengthSamps || event.sampleOffset >= sourceSample)
			continue;

		if (event.IsNoteOn())
			stateSnapshot.Set(event.Channel(), event.data1, event.data2);
		else if (event.IsNoteOff())
			stateSnapshot.Clear(event.Channel(), event.data1);
	}

	return stateSnapshot;
}

midi::MidiNoteSnapshot LoopTake::_SnapshotLiveMidiState(std::size_t loopIndex) const noexcept
{
	midi::MidiNoteSnapshot stateSnapshot;
	if (!_midiOverdubSession.Active || loopIndex >= _midiOverdubSession.Loops.size())
		return stateSnapshot;

	const auto& loopState = _midiOverdubSession.Loops[loopIndex];
	return loopState.LiveHeld;
}

void LoopTake::_SnapshotPunchBoundaryMidi(std::uint32_t punchSample,
	midi::MidiNoteSnapshot& sourceSnapshot,
	midi::MidiNoteSnapshot& liveSnapshot) const noexcept
{
	for (std::size_t loopIndex = 0u; loopIndex < _midiOverdubSession.Loops.size(); ++loopIndex)
	{
		const auto sourceLoopSnapshot = _SnapshotSourceMidiAtSample(loopIndex, punchSample);
		const auto liveLoopSnapshot = _SnapshotLiveMidiState(loopIndex);
		for (std::size_t slot = 0u; slot < midi::MidiNote::TotalNoteSlots; ++slot)
		{
			if (sourceLoopSnapshot.Held.test(slot))
			{
				sourceSnapshot.Held.set(slot);
				if (0u == sourceSnapshot.Velocity[slot])
					sourceSnapshot.Velocity[slot] = sourceLoopSnapshot.Velocity[slot];
			}

			if (liveLoopSnapshot.Held.test(slot))
			{
				liveSnapshot.Held.set(slot);
				if (0u == liveSnapshot.Velocity[slot])
					liveSnapshot.Velocity[slot] = liveLoopSnapshot.Velocity[slot];
			}
		}
	}
}

std::size_t LoopTake::_PruneSharedPunchStartTransitions(std::size_t loopIndex,
	const std::array<midi::MidiPunchWindow, 128u>& windows,
	std::size_t windowCount,
	std::uint32_t targetLoopLength,
	midi::MidiEvent* events,
	std::size_t eventCount) const noexcept
{
	if (!_midiOverdubSession.Active || loopIndex >= _midiOverdubSession.Loops.size() || !events || 0u == targetLoopLength)
		return eventCount;

	const auto& loopState = _midiOverdubSession.Loops[loopIndex];
	for (std::size_t windowIndex = 0u; windowIndex < windowCount && windowIndex < loopState.SharedHeldAtPunchStart.size(); ++windowIndex)
	{
		const auto& sharedHeld = loopState.SharedHeldAtPunchStart[windowIndex];
		if (sharedHeld.Held.none())
			continue;

		const auto boundarySample = NormalizeMidiLoopOffset(windows[windowIndex].StartSample, targetLoopLength);
		std::bitset<midi::MidiNote::TotalNoteSlots> removed;
		std::size_t writeIndex = 0u;
		for (std::size_t readIndex = 0u; readIndex < eventCount; ++readIndex)
		{
			const auto& event = events[readIndex];
			if ((event.sampleOffset == boundarySample) && event.IsNoteOff())
			{
				const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
				if (sharedHeld.Held.test(slot) && !removed.test(slot))
				{
					removed.set(slot);
					continue;
				}
			}

			events[writeIndex++] = event;
		}

		eventCount = writeIndex;
	}

	return eventCount;
}

void LoopTake::_ResetMidiOverdubSession() noexcept
{
	_midiOverdubSession.Active = false;
	_midiOverdubSession.Loops.clear();
}

void LoopTake::_InitMidiOverdubSession(std::shared_ptr<LoopTake> sourceTake)
{
	_midiOverdubSession.Active = !_midiLoops.empty();
	_midiOverdubSession.Loops.clear();
	if (!_midiOverdubSession.Active)
		return;

	_midiOverdubSession.Loops.resize(_midiLoops.size());

	if (!sourceTake)
		return;

	const auto& sourceLoops = sourceTake->GetMidiLoops();
	const auto& sourceChannels = sourceTake->MidiLoopChannels();
	const auto& sourceDevices = sourceTake->MidiLoopDevices();

	for (std::size_t targetIndex = 0u; targetIndex < _midiLoops.size(); ++targetIndex)
	{
		const auto targetChannel = (targetIndex < _midiLoopChannels.size()) ? _midiLoopChannels[targetIndex] : 0u;
		const auto targetDevice = (targetIndex < _midiLoopDevices.size()) ? _midiLoopDevices[targetIndex] : std::string();

		std::optional<std::size_t> sourceIndex;
		for (std::size_t sourceIdx = 0u; sourceIdx < sourceLoops.size(); ++sourceIdx)
		{
			if (sourceIdx >= sourceChannels.size() || sourceChannels[sourceIdx] != targetChannel)
				continue;

			if (sourceIdx < sourceDevices.size() && sourceDevices[sourceIdx] == targetDevice)
			{
				sourceIndex = sourceIdx;
				break;
			}
		}

		if (!sourceIndex.has_value())
		{
			for (std::size_t sourceIdx = 0u; sourceIdx < sourceLoops.size(); ++sourceIdx)
			{
				if (sourceIdx >= sourceChannels.size() || sourceChannels[sourceIdx] != targetChannel)
					continue;

				if ((targetDevice.empty() && sourceIdx < sourceDevices.size() && sourceDevices[sourceIdx].empty()) ||
					(!targetDevice.empty() && sourceIdx < sourceDevices.size() && sourceDevices[sourceIdx].empty()))
				{
					sourceIndex = sourceIdx;
					break;
				}
			}
		}

		if (!sourceIndex.has_value())
		{
			for (std::size_t sourceIdx = 0u; sourceIdx < sourceLoops.size(); ++sourceIdx)
			{
				if (sourceIdx < sourceChannels.size() && sourceChannels[sourceIdx] == targetChannel)
				{
					sourceIndex = sourceIdx;
					break;
				}
			}
		}

		if (!sourceIndex.has_value())
			continue;

		auto sourceLoop = sourceLoops[sourceIndex.value()];
		if (!sourceLoop)
			continue;

		auto& state = _midiOverdubSession.Loops[targetIndex];
		state.SourceLoopLengthSamps = sourceLoop->LoopLengthSamps();
		if (0u == state.SourceLoopLengthSamps)
			state.SourceLoopLengthSamps = static_cast<std::uint32_t>(sourceTake->VisualLoopLengthSamps());
		state.SourceStartSample = NormalizeMidiLoopOffset(
			static_cast<std::uint32_t>(sourceTake->_midiVisualPlayIndex),
			state.SourceLoopLengthSamps);
		
		for (std::size_t eventIndex = 0u; eventIndex < state.SourceEvents.size(); ++eventIndex)
		{
			midi::MidiEvent event{};
			if (!sourceLoop->TryGetEvent(eventIndex, event))
				break;

			state.SourceEvents[state.SourceEventCount++] = event;
		}
	}
}

std::size_t LoopTake::_BuildMidiOverdubMergedEvents(std::size_t loopIndex,
	std::uint32_t targetLoopLength,
	bool includeOpenPunchWindow) noexcept
{
	if (!_midiOverdubSession.Active ||
		loopIndex >= _midiOverdubSession.Loops.size() ||
		0u == targetLoopLength)
	{
		return 0u;
	}

	auto& state = _midiOverdubSession.Loops[loopIndex];
	std::array<midi::MidiPunchWindow, 128u> localWindows{};
	auto localWindowCount = state.PunchWindowCount;
	for (std::size_t i = 0u; i < localWindowCount && i < localWindows.size(); ++i)
		localWindows[i] = state.PunchWindows[i];

	const auto invalidSample = (std::numeric_limits<std::uint32_t>::max)();
	if (includeOpenPunchWindow && state.ActivePunchStart != invalidSample)
	{
		bool updatedActiveWindow = false;
		if (localWindowCount > 0u)
		{
			auto& lastWindow = localWindows[localWindowCount - 1u];
			if (lastWindow.StartSample == state.ActivePunchStart && lastWindow.EndSample <= lastWindow.StartSample)
			{
				lastWindow.EndSample = (targetLoopLength > lastWindow.StartSample) ?
					targetLoopLength :
					lastWindow.StartSample;
				updatedActiveWindow = true;
			}
		}

		if (!updatedActiveWindow && localWindowCount < localWindows.size())
			localWindows[localWindowCount++] = midi::MidiPunchWindow{ state.ActivePunchStart, targetLoopLength };
	}

	std::array<midi::MidiPunchWindow, 128u> renderWindows{};
	std::size_t renderWindowCount = 0u;
	for (std::size_t i = 0u; i < localWindowCount; ++i)
	{
		auto start = localWindows[i].StartSample;
		auto end = localWindows[i].EndSample;
		if (end <= start)
			continue;

		if (start >= targetLoopLength)
			continue;

		if (end > targetLoopLength)
			end = targetLoopLength;

		if (end <= start || renderWindowCount >= renderWindows.size())
			continue;

		renderWindows[renderWindowCount++] = midi::MidiPunchWindow{ start, end };
	}

	std::array<midi::MidiEvent, midi::MidiLoop::DefaultCapacity> rebasedSourceEvents{};
	const auto rebasedSourceCount = BuildRebasedMidiOverdubSourceEvents(state,
		rebasedSourceEvents.data(),
		rebasedSourceEvents.size());

	midi::MidiOverdubRenderParams renderParams;
	renderParams.SourceEvents = rebasedSourceEvents.data();
	renderParams.SourceEventCount = rebasedSourceCount;
	renderParams.SourceLoopLengthSamps = state.SourceLoopLengthSamps;
	renderParams.TargetLoopLengthSamps = targetLoopLength;
	renderParams.PunchWindows = (renderWindowCount > 0u) ? renderWindows.data() : nullptr;
	renderParams.PunchWindowCount = renderWindowCount;

	auto mergedCount = BuildMidiOverdubBaseEvents(renderParams,
		_midiOverdubSession.BuildScratch.data(),
		_midiOverdubSession.BuildScratch.size());
	mergedCount = _PruneSharedPunchStartTransitions(loopIndex,
		localWindows,
		localWindowCount,
		targetLoopLength,
		_midiOverdubSession.BuildScratch.data(),
		mergedCount);

	for (std::size_t i = 0u; i < mergedCount && i < _midiOverdubSession.MergeScratch.size(); ++i)
		_midiOverdubSession.MergeScratch[i] = _midiOverdubSession.BuildScratch[i];

	for (std::size_t liveIndex = 0u; liveIndex < state.LiveEventCount; ++liveIndex)
	{
		if (mergedCount >= _midiOverdubSession.MergeScratch.size())
			break;

		auto event = state.LiveEvents[liveIndex];
		if (event.sampleOffset >= targetLoopLength)
		{
			if (event.IsNoteOff())
				event.sampleOffset = targetLoopLength - 1u;
			else
				continue;
		}

		_midiOverdubSession.MergeScratch[mergedCount++] = event;
	}

	midi::MidiNote::SortMidiEvents(_midiOverdubSession.MergeScratch.data(), mergedCount);

	if (_loggingConfig.Ui == "verbose")
	{
		std::uint32_t sourceBefore = 0u;
		std::uint32_t sourceAfter = 0u;
		bool haveSourceExample = false;
		for (std::size_t i = 0u; i < state.SourceEventCount; ++i)
		{
			const auto& event = state.SourceEvents[i];
			if (event.sampleOffset >= state.SourceLoopLengthSamps)
				continue;

			sourceBefore = event.sampleOffset;
			sourceAfter = NormalizeMidiLoopOffset(
				event.sampleOffset + state.SourceLoopLengthSamps - NormalizeMidiLoopOffset(state.SourceStartSample, state.SourceLoopLengthSamps),
				state.SourceLoopLengthSamps);
			haveSourceExample = true;
			break;
		}

		std::cout << "MIDI overdub merge: take=" << _id
			<< " loopIndex=" << loopIndex
			<< " sourceStart=" << NormalizeMidiLoopOffset(state.SourceStartSample, state.SourceLoopLengthSamps)
			<< " sourceEvents=" << state.SourceEventCount
			<< " rebasedSourceEvents=" << rebasedSourceCount
			<< " liveEvents=" << state.LiveEventCount
			<< " merged=" << mergedCount;

		if (haveSourceExample)
			std::cout << " sourceSample=" << sourceBefore << "->" << sourceAfter;

		if (state.LiveEventCount > 0u)
			std::cout << " liveSample=" << state.LiveEvents[0].sampleOffset;

		std::cout << '\n';
	}

	return mergedCount;
}

void LoopTake::_RefreshMidiOverdubPreview(std::uint32_t displayLength) noexcept
{
	if (!_midiOverdubSession.Active)
		return;

	for (std::size_t loopIndex = 0u; loopIndex < _midiLoops.size(); ++loopIndex)
	{
		auto midiLoop = _midiLoops[loopIndex];
		if (!midiLoop)
			continue;

		midiLoop->StartRecord();
		const auto mergedCount = _BuildMidiOverdubMergedEvents(loopIndex, displayLength, true);
		for (std::size_t eventIndex = 0u; eventIndex < mergedCount; ++eventIndex)
			midiLoop->AppendEventForBuild(_midiOverdubSession.MergeScratch[eventIndex]);
	}
}

void LoopTake::_OpenMidiPunchWindow(std::uint32_t punchSample) noexcept
{
	if (!_midiOverdubSession.Active)
		return;

	const auto invalidSample = (std::numeric_limits<std::uint32_t>::max)();
	for (std::size_t loopIndex = 0u; loopIndex < _midiOverdubSession.Loops.size(); ++loopIndex)
	{
		auto& state = _midiOverdubSession.Loops[loopIndex];
		if (state.ActivePunchStart != invalidSample)
			continue;

		const auto sourceSnapshot = _SnapshotSourceMidiAtSample(loopIndex, punchSample);
		const auto liveSnapshot = _SnapshotLiveMidiState(loopIndex);

		if (state.PunchWindowCount < state.PunchWindows.size())
		{
			state.PunchWindows[state.PunchWindowCount] = midi::MidiPunchWindow{ punchSample, punchSample };
			state.SharedHeldAtPunchStart[state.PunchWindowCount].Held = sourceSnapshot.Held & liveSnapshot.Held;
			state.PunchWindowCount++;
		}

		state.ActivePunchStart = punchSample;
		state.ActivePunchLiveEventStart = state.LiveEventCount;

		for (std::size_t slot = 0u; slot < state.LiveHeld.Velocity.size(); ++slot)
		{
			const auto velocity = state.LiveHeld.Velocity[slot];
			if (velocity == 0u || sourceSnapshot.Held.test(slot))
				continue;

			const auto channel = static_cast<std::uint8_t>((slot >> 7) & midi::MidiEvent::ChannelMask);
			const auto note = static_cast<std::uint8_t>(slot & 0x7Fu);
			_AppendMidiOverdubLiveEvent(loopIndex, midi::MidiEvent::MakeNoteOn(punchSample, channel, note, velocity));
		}
	}
}

void LoopTake::_CloseMidiPunchWindow(std::uint32_t punchSample, bool synthNoteOffs) noexcept
{
	if (!_midiOverdubSession.Active)
		return;

	const auto invalidSample = (std::numeric_limits<std::uint32_t>::max)();
	for (std::size_t loopIndex = 0u; loopIndex < _midiOverdubSession.Loops.size(); ++loopIndex)
	{
		auto& state = _midiOverdubSession.Loops[loopIndex];
		if (state.ActivePunchStart == invalidSample)
			continue;

		if (state.PunchWindowCount > 0u)
		{
			auto& window = state.PunchWindows[state.PunchWindowCount - 1u];
			window.EndSample = (punchSample > window.StartSample) ? punchSample : window.StartSample;

			if (window.EndSample <= window.StartSample)
			{
				state.LiveEventCount = state.ActivePunchLiveEventStart;
				state.ActivePunchStart = invalidSample;
				continue;
			}
		}

		state.ActivePunchStart = invalidSample;

		if (!synthNoteOffs)
			continue;

		for (std::size_t slot = 0u; slot < state.LiveHeld.Velocity.size(); ++slot)
		{
			if (state.LiveHeld.Velocity[slot] == 0u)
				continue;

			const auto channel = static_cast<std::uint8_t>((slot >> 7) & midi::MidiEvent::ChannelMask);
			const auto note = static_cast<std::uint8_t>(slot & 0x7Fu);
			_AppendMidiOverdubLiveEvent(loopIndex, midi::MidiEvent::MakeNoteOff(punchSample, channel, note));
		}
	}
}

bool LoopTake::_AppendMidiOverdubLiveEvent(std::size_t loopIndex, const midi::MidiEvent& ev) noexcept
{
	if (!_midiOverdubSession.Active || loopIndex >= _midiOverdubSession.Loops.size())
		return false;

	auto& state = _midiOverdubSession.Loops[loopIndex];
	if (state.LiveEventCount >= state.LiveEvents.size())
	{
		state.LiveDropped++;
		return false;
	}

	state.LiveEvents[state.LiveEventCount++] = ev;
	return true;
}

void LoopTake::_FinalizeMidiOverdubLoop(std::size_t loopIndex, std::uint32_t targetLoopLength)
{
	if (!_midiOverdubSession.Active || loopIndex >= _midiOverdubSession.Loops.size() || loopIndex >= _midiLoops.size())
		return;

	auto midiLoop = _midiLoops[loopIndex];
	if (!midiLoop)
		return;

	const auto mergedCount = _BuildMidiOverdubMergedEvents(loopIndex, targetLoopLength, false);
	midiLoop->ReplaceRecordedEvents(_midiOverdubSession.MergeScratch.data(), mergedCount, targetLoopLength);
	midiLoop->UpdateModelFromEvents(targetLoopLength, true);
}

void LoopTake::_ApplyMidiQuantisationGesture(midi::MidiQuantisationGesture gesture,
	midi::MidiQuantisationFraction fraction,
	const char* source) noexcept
{
	const auto previous = MidiQuantisation();
	auto resolvedGrain = ResolvedMidiQuantisation().GrainSamps;
	if (0u == resolvedGrain)
		resolvedGrain = midi::MidiQuantisation::ResolveGestureGrain(_MidiQuantisationGrainCandidates());

	const auto updated = midi::MidiQuantisation::ApplyGesture(previous,
		gesture,
		fraction,
		resolvedGrain);

	if (_loggingConfig.Ui == "verbose" && previous.Fraction != updated.Fraction)
		_LogMidiQuantisationFractionChange(previous.Fraction, updated.Fraction, source);

	SetMidiQuantisation(updated);
}

void LoopTake::_LogMidiQuantisationFractionChange(midi::MidiQuantisationFraction previous,
	midi::MidiQuantisationFraction updated,
	const char* source) const
{
	std::cout << "MIDI quantisation fraction: take=" << _id
		<< " source=" << source
		<< " " << midi::MidiQuantisation::FractionLabel(previous)
		<< " -> " << midi::MidiQuantisation::FractionLabel(updated) << '\n';
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

void LoopTake::SetParentVisualScale(float scale) noexcept
{
	_parentVisualScale = std::max(0.0f, scale);
}

void LoopTake::LoadVstPlugin(std::wstring path,
	std::vector<std::uint8_t> initialState)
{
	_pendingVstLoads.push_back({ std::move(path), std::move(initialState) });
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
