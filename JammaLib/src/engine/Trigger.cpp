#include "Trigger.h"

#include <cassert>
#include <cstdint>
#include <stdexcept>

using namespace base;
using namespace engine;
using namespace utils;
using actions::ActionResult;
using actions::KeyAction;
using actions::TriggerAction;
using audio::AudioMixer;

namespace engine
{
	class PreparedTriggerBounceWriter final : public base::BounceWriter
	{
	public:
		explicit PreparedTriggerBounceWriter(std::shared_ptr<AudioMixer> mixer) :
			_mixer(std::move(mixer))
		{
		}

		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const float* srcBuf,
			unsigned int numSamps,
			unsigned int destChannel) override
		{
			Write(_mixer, dest, srcBuf, numSamps, destChannel);
		}

		static void Write(const std::shared_ptr<AudioMixer>& mixer,
			const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			unsigned int numSamps,
			unsigned int destChannel)
		{
			if (!dest || !srcBuf || !mixer)
				return;
			base::AudioWriteRequest request;
			request.samples = srcBuf;
			request.numSamps = numSamps;
			request.stride = 1;
			request.fadeCurrent = 1.0f - static_cast<float>(mixer->Level());
			request.fadeNew = static_cast<float>(mixer->Level());
			request.source = base::Audible::AUDIOSOURCE_BOUNCE;
			dest->OnBlockWriteChannel(destChannel, request, 0);
			mixer->Offset(numSamps);
		}

	private:
		std::shared_ptr<AudioMixer> _mixer;
	};
}

unsigned int Trigger::EncodeMidiBindingValue(io::RigFile::MidiTriggerEvent kind,
	unsigned int channel,
	unsigned int id)
{
	return (static_cast<unsigned int>(kind) << MidiBindingKindShift) |
		((channel & 0x0Fu) << MidiBindingChannelShift) |
			(id & 0x7Fu);
}

DualBinding Trigger::MakeMidiBinding(io::RigFile::MidiTriggerEvent kind,
	unsigned int channel,
	unsigned int id,
	unsigned int state)
{
	DualBinding binding;
	binding.SetDown(TriggerBinding(TriggerSource::TRIGGER_MIDI,
		EncodeMidiBindingValue(kind, channel, id),
		state), true);
	return binding;
}

void Trigger::AddMidiBindingForChannels(const io::RigFile::Trigger::MidiTriggerBindingSpec& bindingSpec,
	const std::function<void(const DualBinding&)>& onBinding)
{
	if (bindingSpec.MatchAnyChannel)
	{
		for (auto channel = 0u; channel < 16u; ++channel)
			onBinding(MakeMidiBinding(bindingSpec.Kind,
				channel,
				bindingSpec.Id,
				bindingSpec.State));
		return;
	}

	onBinding(MakeMidiBinding(bindingSpec.Kind,
		bindingSpec.Channel,
		bindingSpec.Id,
		bindingSpec.State));
}

bool Trigger::IsValidMidiBindingSpec(const io::RigFile::Trigger::MidiTriggerBindingSpec& bindingSpec)
{
	if (bindingSpec.Id > 127u)
		return false;

	if (!bindingSpec.MatchAnyChannel && (bindingSpec.Channel > 15u))
		return false;

	return true;
}

Trigger::Trigger(TriggerParams trigParams) :
	_name(trigParams.Name),
	_activateBindings(trigParams.Activate),
	_ditchBindings(trigParams.Ditch),
	_inputChannels(trigParams.InputChannels),
	_midiInputDevices(trigParams.MidiInputDevices),
	_state(TRIGSTATE_DEFAULT),
	_debounceTimeMs(trigParams.DebounceMs),
	_lastActivateTime(),
	_lastDitchTime(),
	_isDitchDown(false),
	_isLastActivateDown(false),
	_isLastDitchDown(false),
	_isLastActivateDownRaw(false),
	_isLastDitchDownRaw(false),
	_recordSampCount(0),
	_overdubMixer(std::shared_ptr<audio::AudioMixer>())
{
	// Binding indices are part of the fixed-size audio ingress protocol. Keep
	// every accepted binding representable by its dedicated overflow mailbox.
	if (_activateBindings.size() > _InputFallbackBindingCapacity ||
		_ditchBindings.size() > _InputFallbackBindingCapacity)
		throw std::invalid_argument("Trigger binding count exceeds fixed ingress capacity");
	_overdubMixer = std::make_shared<AudioMixer>(
		GetOverdubMixerParams(trigParams.InputChannels));
	_overdubWriter = CreateBounceWriter(_overdubMixer);
	_publishedTakeHistory.store(std::make_shared<const std::vector<TriggerTake>>(),
		std::memory_order_release);
}

Trigger::~Trigger()
{
}

/*static*/ const char* Trigger::ActionLabel(actions::ActionResultType rt) noexcept
{
	switch (rt)
	{
	case actions::ACTIONRESULT_ACTIVATE: return "Activate";
	case actions::ACTIONRESULT_DITCH:    return "Ditch";
	case actions::ACTIONRESULT_TOGGLE:   return "Toggle";
	default:                             return "Action";
	}
}

std::optional<std::shared_ptr<Trigger>> Trigger::FromFile(TriggerParams trigParams, io::RigFile::Trigger trigStruct)
{
	trigParams.Name = trigStruct.Name;
	if (trigStruct.MidiTrigger.has_value() &&
		(!IsValidMidiBindingSpec(trigStruct.MidiTrigger->Activate) ||
		 !IsValidMidiBindingSpec(trigStruct.MidiTrigger->Ditch)))
		return std::nullopt;
	const auto midiActivateBindingCount = trigStruct.MidiTrigger.has_value() ?
		(trigStruct.MidiTrigger->Activate.MatchAnyChannel ? 16u : 1u) : 0u;
	const auto midiDitchBindingCount = trigStruct.MidiTrigger.has_value() ?
		(trigStruct.MidiTrigger->Ditch.MatchAnyChannel ? 16u : 1u) : 0u;
	const auto addedBindingCount = trigStruct.TriggerPairs.size() +
		midiActivateBindingCount + midiDitchBindingCount;
	if (std::max(trigParams.Activate.size(), trigParams.Ditch.size()) +
		addedBindingCount > _InputFallbackBindingCapacity)
		return std::nullopt;

	auto trigger = std::make_shared<Trigger>(trigParams);
	trigger->_midiInputMode = trigStruct.MidiInputs;

	for (auto trigPair : trigStruct.TriggerPairs)
	{
		auto source = trigPair.Source == io::RigFile::TriggerPair::SOURCE_SERIAL ?
			TriggerSource::TRIGGER_SERIAL :
			TriggerSource::TRIGGER_KEY;
		auto device = source == TriggerSource::TRIGGER_SERIAL ? trigPair.Device : std::string();

		auto activate = DualBinding(
			TriggerBinding{
				source,
				trigPair.ActivateDown,
				1,
				device
			},
			TriggerBinding{
				source,
				trigPair.ActivateUp,
				0,
				device
			});

		auto ditch = DualBinding(
			TriggerBinding{
				source,
				trigPair.DitchDown,
				1,
				device
			},
			TriggerBinding{
				source,
				trigPair.DitchUp,
				0,
				device
			});

		if (!trigger->AddBinding(activate, ditch)) return std::nullopt;
	}

	for (auto inChan : trigStruct.InputChannels)
		trigger->AddInputChannel(inChan);

	for (const auto& midiDevice : trigStruct.MidiInputDevices)
		trigger->AddMidiInputDevice(midiDevice);

	if (trigStruct.MidiTrigger.has_value())
	{
		bool addedAllBindings = true;
		AddMidiBindingForChannels(trigStruct.MidiTrigger->Activate,
			[&trigger, &addedAllBindings](const DualBinding& binding)
			{
				addedAllBindings = trigger->AddBinding(binding, DualBinding()) && addedAllBindings;
			});
		AddMidiBindingForChannels(trigStruct.MidiTrigger->Ditch,
			[&trigger, &addedAllBindings](const DualBinding& binding)
			{
				addedAllBindings = trigger->AddBinding(DualBinding(), binding) && addedAllBindings;
			});
		if (!addedAllBindings) return std::nullopt;
	}

	return trigger;
}

audio::BounceMixBehaviourParams Trigger::GetOverdubBehaviourParams(
	std::vector<unsigned int> channels)
{
	return audio::BounceMixBehaviourParams{ audio::WireMixBehaviourParams(channels) };
}

audio::AudioMixerParams Trigger::GetOverdubMixerParams(std::vector<unsigned int> channels)
{
	audio::AudioMixerParams mixerParams;
	mixerParams.Size = { 1, 1 };
	mixerParams.Position = { 0, 0 };
	mixerParams.Behaviour = GetOverdubBehaviourParams(channels);

	return mixerParams;
}

ActionResult Trigger::OnAction(KeyAction action)
{
	auto keyState = KeyAction::KEY_DOWN == action.KeyActionType ? 1u : 0u;
	return OnEvent(TriggerSource::TRIGGER_KEY, action.KeyChar, keyState, action);
}

bool Trigger::TryEncodeMidiEvent(const midi::MidiEvent& event,
	unsigned int& outValue,
	unsigned int& outState)
{
	if (event.IsNoteOn() || event.IsNoteOff())
	{
		outValue = EncodeMidiBindingValue(io::RigFile::MidiTriggerEvent::NOTE,
			event.Channel(),
			event.data1);
		outState = event.IsNoteOn() ? 1u : 0u;
		return true;
	}
	if (event.MessageType() == MidiCcStatus)
	{
		outValue = EncodeMidiBindingValue(io::RigFile::MidiTriggerEvent::CC,
			event.Channel(),
			event.data1);
		outState = event.data2 > 0u ? 1u : 0u;
		return true;
	}
	return false;
}

ActionResult Trigger::OnEvent(const midi::MidiEvent& event,
	const base::Action& action)
{
	unsigned int value = 0u, state = 0u;
	if (!TryEncodeMidiEvent(event, value, state))
		return ActionResult::NoAction();

	return OnEvent(TriggerSource::TRIGGER_MIDI, value, state, action);
}

ActionResult Trigger::OnEvent(TriggerSource source,
	unsigned int value,
	unsigned int state,
	const base::Action& action,
	const std::string& device)
{
	ActionResult res;
	res.IsEaten = false;
	res.ResultType = actions::ACTIONRESULT_DEFAULT;

	for (auto& b : _activateBindings)
	{
		if (TryChangeState(b, true, source, value, state, action, device))
		{
			ProcessStructuralActionsOnJob(action.GetUserConfig(), action.GetAudioParams());
			_ProcessStructuralResults();
			res.IsEaten = true;
			res.ResultType = actions::ACTIONRESULT_ACTIVATE;
			return res;
		}
	}
	for (auto& b : _ditchBindings)
	{
		if (TryChangeState(b, false, source, value, state, action, device))
		{
			ProcessStructuralActionsOnJob(action.GetUserConfig(), action.GetAudioParams());
			_ProcessStructuralResults();
			res.IsEaten = true;
			res.ResultType = (TRIGSTATE_DEFAULT == GetState()) ?
				actions::ACTIONRESULT_DITCH :
				actions::ACTIONRESULT_DEFAULT;
			return res;
		}
	}

	return res;
}

ActionResult Trigger::QueueExternalControlAction(bool isActivate,
	bool isDown,
	const base::Action& action,
	std::uint64_t rigRevision)
{
	const auto eventTimeUsec = std::chrono::duration_cast<std::chrono::microseconds>(
		action.GetActionTime().time_since_epoch()).count();
	const TriggerInputEdge edge{ rigRevision, _DirectBindingIndex,
		isActivate ? TRIGGER_CONTROL_ACTIVATE : TRIGGER_CONTROL_DITCH,
		isDown ? TRIGGER_EDGE_DOWN : TRIGGER_EDGE_UP, eventTimeUsec };
	if (!_uiInputQueue.Push(edge))
		_PublishInputFallback(TRIGGER_INPUT_UI, edge);

	return {
		true,
		"",
		"",
		actions::ACTIONRESULT_DEFAULT,
		nullptr,
		std::weak_ptr<base::GuiElement>()
	};
}

ActionResult Trigger::QueueInputEvent(TriggerInputDomain domain,
	std::uint64_t rigRevision,
	TriggerSource source,
	unsigned int value,
	unsigned int state,
	const base::Action& action,
	const std::string& device)
{
	auto enqueue = [&](TriggerControl control, std::uint16_t bindingIndex,
		DualBinding::TestResult match)
	{
		const auto eventTimeUsec = std::chrono::duration_cast<std::chrono::microseconds>(
			action.GetActionTime().time_since_epoch()).count();
		const TriggerInputEdge edge{ rigRevision, bindingIndex, control,
			match == DualBinding::MATCH_DOWN ? TRIGGER_EDGE_DOWN : TRIGGER_EDGE_UP,
			eventTimeUsec };
		auto& queue = domain == TRIGGER_INPUT_UI ? _uiInputQueue : _jobInputQueue;
		if (!queue.Push(edge))
			_PublishInputFallback(domain, edge);
		return ActionResult{ true, "", "",
			actions::ACTIONRESULT_DEFAULT,
			nullptr, std::weak_ptr<base::GuiElement>() };
	};

	for (std::size_t i = 0u; i < _activateBindings.size() && i < _DirectBindingIndex; ++i)
	{
		const auto match = _activateBindings[i].Match(source, value, state, device);
		if (match != DualBinding::MATCH_NONE)
			return enqueue(TRIGGER_CONTROL_ACTIVATE, static_cast<std::uint16_t>(i), match);
	}
	for (std::size_t i = 0u; i < _ditchBindings.size() && i < _DirectBindingIndex; ++i)
	{
		const auto match = _ditchBindings[i].Match(source, value, state, device);
		if (match != DualBinding::MATCH_NONE)
			return enqueue(TRIGGER_CONTROL_DITCH, static_cast<std::uint16_t>(i), match);
	}
	return ActionResult::NoAction();
}

ActionResult Trigger::QueueMidiInputEvent(TriggerInputDomain domain,
	std::uint64_t rigRevision,
	const midi::MidiEvent& event,
	const base::Action& action)
{
	unsigned int value = 0u, state = 0u;
	if (!TryEncodeMidiEvent(event, value, state))
		return ActionResult::NoAction();
	return QueueInputEvent(domain, rigRevision, TRIGGER_MIDI, value, state, action);
}

void Trigger::OnTick(Time curTime,
	unsigned int samps,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	OnTick(curTime, samps, cfg, params, 0u);
}

void Trigger::OnTick(Time curTime,
	unsigned int samps,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params,
	std::uint64_t rigRevision)
{
	_ProcessStructuralResults();
	_ProcessQueuedInputActions(rigRevision, cfg, params);

	bool isRecording = (TriggerState::TRIGSTATE_RECORDING == _state) ||
		(TriggerState::TRIGSTATE_OVERDUBBING == _state) ||
		(TriggerState::TRIGSTATE_PUNCHEDIN == _state);
	if (isRecording)
	{
		if (_pendingCompletion == STRUCTURAL_END_RECORDING ||
			_pendingCompletion == STRUCTURAL_END_OVERDUB ||
			_pendingCompletion == STRUCTURAL_DITCH ||
			_pendingCompletion == STRUCTURAL_DITCH_OVERDUB)
			_pendingRecordSamps += samps;
		else
			_recordSampCount.fetch_add(samps, std::memory_order_relaxed);
	}

	for (std::size_t i = 0u; i < _delayedActionCount; ++i)
	{
		auto& action = _delayedActions[i];
		action.SampsLeft = samps >= action.SampsLeft ? 0u : action.SampsLeft - samps;
	}

	_FlushDelayedPunchActions(samps);

	if (0 != _debounceTimeMs && _pendingCompletion == STRUCTURAL_NONE)
	{
		// Eventually flick to new state (if held long enough).
		auto elapsedMs = Timer::GetElapsedSeconds(_lastActivateTime, curTime) * 1000.0;
		if (_isLastActivateDownRaw != _isLastActivateDown && elapsedMs > _debounceTimeMs)
		{
			_lastActivateTime = Timer::GetZero();
			_isLastActivateDown = _isLastActivateDownRaw;
			StateMachine(_isLastActivateDownRaw, true, cfg, params);
		}

		elapsedMs = Timer::GetElapsedSeconds(_lastDitchTime, curTime) * 1000.0;
		if (_isLastDitchDownRaw != _isLastDitchDown && elapsedMs > _debounceTimeMs)
		{
			_lastDitchTime = Timer::GetZero();
			_isLastDitchDown = _isLastDitchDownRaw;
			StateMachine(_isLastDitchDownRaw, false, cfg, params);
		}
	}
	_PublishTriggerStateSnapshot();
}

bool Trigger::CanEditRouting() const noexcept
{
	return _publishedCanEditRouting.load(std::memory_order_acquire) &&
		_uiInputQueue.Empty() && _jobInputQueue.Empty();
}

std::shared_ptr<base::BounceWriter> Trigger::CreateBounceWriter(
	const std::shared_ptr<audio::AudioMixer>& mixer)
{
	return std::make_shared<PreparedTriggerBounceWriter>(mixer);
}

bool Trigger::CanApplyCaptureRouting() const noexcept
{
	return _publishedCanApplyCaptureRouting.load(std::memory_order_acquire) &&
		_uiInputQueue.Empty() && _jobInputQueue.Empty();
}

bool Trigger::_CanEditRoutingAtAudioBoundary() const noexcept
{
	return _state == TRIGSTATE_DEFAULT &&
		!_isLastActivateDownRaw && !_isLastDitchDownRaw && !_isDitchDown &&
		_uiInputQueue.Empty() && _jobInputQueue.Empty() &&
		_uiFallbackPublicationCount.load(std::memory_order_acquire) == _consumedUiFallbackPublicationCount &&
		_jobFallbackPublicationCount.load(std::memory_order_acquire) == _consumedJobFallbackPublicationCount &&
		_delayedActionCount == 0u &&
		_delayedPunchActionCount == 0u &&
		_pendingCompletion == STRUCTURAL_NONE &&
		_structuralCommands.Empty() && _structuralResults.Empty() &&
		_jobStructuralActionsInFlight.load(std::memory_order_acquire) == 0u &&
		_loopTakeHistorySize == 0u;
}

bool Trigger::_CanApplyCaptureRoutingAtAudioBoundary() const noexcept
{
	return _state == TRIGSTATE_DEFAULT &&
		!_isLastActivateDownRaw && !_isLastDitchDownRaw && !_isDitchDown &&
		_uiInputQueue.Empty() && _jobInputQueue.Empty() &&
		_uiFallbackPublicationCount.load(std::memory_order_acquire) == _consumedUiFallbackPublicationCount &&
		_jobFallbackPublicationCount.load(std::memory_order_acquire) == _consumedJobFallbackPublicationCount &&
		_delayedActionCount == 0u &&
		_delayedPunchActionCount == 0u &&
		_pendingCompletion == STRUCTURAL_NONE &&
		_structuralCommands.Empty() && _structuralResults.Empty() &&
		_jobStructuralActionsInFlight.load(std::memory_order_acquire) == 0u;
}

bool Trigger::_ApplyInputEdge(const TriggerInputEdge& edge,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params) noexcept
{
	const bool isActivate = edge.Control == TRIGGER_CONTROL_ACTIVATE;
	const bool isDown = edge.Edge == TRIGGER_EDGE_DOWN;
	auto applyStateMachine = [&]()
	{
		const auto priorState = _state;
		const auto changed = StateMachine(isDown, isActivate, cfg, params);
		if (changed && isActivate && isDown && priorState != _state &&
			_pendingCompletion == STRUCTURAL_NONE)
			_activationOutcomeCount.fetch_add(1u, std::memory_order_release);
		return changed;
	};
	if (edge.BindingIndex == _DirectBindingIndex)
	{
		if (isActivate)
		{
			_isLastActivateDownRaw = isDown;
			_isLastActivateDown = isDown;
		}
		else
		{
			_isLastDitchDownRaw = isDown;
			_isLastDitchDown = isDown;
		}
		return applyStateMachine();
	}

	auto& bindings = isActivate ? _activateBindings : _ditchBindings;
	if (edge.BindingIndex >= bindings.size()) return false;
	const auto match = bindings[edge.BindingIndex].ApplyResolved(
		isDown ? DualBinding::MATCH_DOWN : DualBinding::MATCH_RELEASE);
	if (match == DualBinding::MATCH_NONE || !IgnoreRepeats(isActivate, match)) return false;
	const auto eventDuration = std::chrono::duration_cast<Time::duration>(
		std::chrono::microseconds(edge.EventTimeUsec));
	if (!Debounce(isActivate, match, Time(eventDuration))) return false;
	return applyStateMachine();
}

void Trigger::_ProcessQueuedInputActions(std::uint64_t rigRevision,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params) noexcept
{
	const auto uiFallbackPublicationCount =
		_uiFallbackPublicationCount.load(std::memory_order_acquire);
	const auto jobFallbackPublicationCount =
		_jobFallbackPublicationCount.load(std::memory_order_acquire);
	if (uiFallbackPublicationCount == _consumedUiFallbackPublicationCount &&
		jobFallbackPublicationCount == _consumedJobFallbackPublicationCount)
	{
		for (std::size_t consumed = 0u; consumed < (_InputQueueCapacity * 2u); ++consumed)
		{
			if (_pendingCompletion != STRUCTURAL_NONE)
				return;
			TriggerInputEdge uiEdge, jobEdge, edge;
			const bool hasUi = _uiInputQueue.Peek(uiEdge);
			const bool hasJob = _jobInputQueue.Peek(jobEdge);
			if (!hasUi && !hasJob)
				return;
			if (hasUi && (!hasJob || uiEdge.EventTimeUsec <= jobEdge.EventTimeUsec))
				_uiInputQueue.Pop(edge);
			else
				_jobInputQueue.Pop(edge);
			if (edge.RigRevision == rigRevision)
				_ApplyInputEdge(edge, cfg, params);
		}
		return;
	}

	std::array<TriggerInputEdge, _InputFallbackCount> uiFallbackEdges{};
	std::array<TriggerInputEdge, _InputFallbackCount> jobFallbackEdges{};
	std::array<std::uint64_t, _InputFallbackCount> uiFallbackSequences{};
	std::array<std::uint64_t, _InputFallbackCount> jobFallbackSequences{};
	std::array<bool, _InputFallbackCount> hasUiFallback{};
	std::array<bool, _InputFallbackCount> hasJobFallback{};
	for (std::size_t index = 0u; index < _InputFallbackCount; ++index)
	{
		hasUiFallback[index] = _uiInputFallbacks[index].ReadLatest(
			_consumedUiFallbackSequences[index], uiFallbackEdges[index], uiFallbackSequences[index]);
		hasJobFallback[index] = _jobInputFallbacks[index].ReadLatest(
			_consumedJobFallbackSequences[index], jobFallbackEdges[index], jobFallbackSequences[index]);
	}

	constexpr auto maxActions = (_InputQueueCapacity * 2u) + (_InputFallbackCount * 2u);
	for (std::size_t consumed = 0u; consumed < maxActions; ++consumed)
	{
		if (_pendingCompletion != STRUCTURAL_NONE)
			break;
		TriggerInputEdge uiEdge, jobEdge;
		const bool hasUi = _uiInputQueue.Peek(uiEdge);
		const bool hasJob = _jobInputQueue.Peek(jobEdge);
		int selectedDomain = -1;
		std::size_t selectedFallback = _InputFallbackCount;
		std::int64_t selectedTime = (std::numeric_limits<std::int64_t>::max)();
		auto consider = [&](int domain, std::size_t fallbackIndex, bool available,
			const TriggerInputEdge& candidate)
		{
			if (available && (candidate.EventTimeUsec < selectedTime ||
				(candidate.EventTimeUsec == selectedTime &&
					(selectedDomain < 0 || domain < selectedDomain ||
						(domain == selectedDomain && fallbackIndex < selectedFallback)))))
			{
				selectedDomain = domain;
				selectedFallback = fallbackIndex;
				selectedTime = candidate.EventTimeUsec;
			}
		};
		// Stable equal-time priority: UI queue, UI mailboxes, job queue, job mailboxes.
		consider(0, 0u, hasUi, uiEdge);
		for (std::size_t index = 0u; index < _InputFallbackCount; ++index)
			consider(1, index, hasUiFallback[index], uiFallbackEdges[index]);
		consider(2, 0u, hasJob, jobEdge);
		for (std::size_t index = 0u; index < _InputFallbackCount; ++index)
			consider(3, index, hasJobFallback[index], jobFallbackEdges[index]);
		TriggerInputEdge edge;
		switch (selectedDomain)
		{
		case 0: _uiInputQueue.Pop(edge); break;
		case 1:
			edge = uiFallbackEdges[selectedFallback];
			hasUiFallback[selectedFallback] = false;
			_consumedUiFallbackSequences[selectedFallback] = uiFallbackSequences[selectedFallback];
			break;
		case 2: _jobInputQueue.Pop(edge); break;
		case 3:
			edge = jobFallbackEdges[selectedFallback];
			hasJobFallback[selectedFallback] = false;
			_consumedJobFallbackSequences[selectedFallback] = jobFallbackSequences[selectedFallback];
			break;
		default: goto input_fallbacks_consumed;
		}
		if (edge.RigRevision != rigRevision) continue;
		_ApplyInputEdge(edge, cfg, params);
	}
	input_fallbacks_consumed:
	// A structural transition can pause this merge while other mailbox values
	// remain pending. Only enable the queue-only fast path once every mailbox
	// sequence observed by this consumer has actually been consumed.
	if (_InputFallbacksConsumed(_uiInputFallbacks, _consumedUiFallbackSequences))
		_consumedUiFallbackPublicationCount = uiFallbackPublicationCount;
	if (_InputFallbacksConsumed(_jobInputFallbacks, _consumedJobFallbackSequences))
		_consumedJobFallbackPublicationCount = jobFallbackPublicationCount;
}

std::size_t Trigger::_InputFallbackIndex(const TriggerInputEdge& edge) noexcept
{
	const auto bindingIndex = edge.BindingIndex == _DirectBindingIndex ?
		_InputFallbackBindingCapacity : static_cast<std::size_t>(edge.BindingIndex);
	if (bindingIndex >= _InputFallbackBindingsPerControl)
		return _InputFallbackCount;
	const auto controlOffset = edge.Control == TRIGGER_CONTROL_ACTIVATE ?
		0u : _InputFallbackBindingsPerControl;
	return controlOffset + bindingIndex;
}

void Trigger::_PublishInputFallback(TriggerInputDomain domain,
	const TriggerInputEdge& edge) noexcept
{
	const auto index = _InputFallbackIndex(edge);
	if (index >= _InputFallbackCount)
		return;
	auto& fallbacks = domain == TRIGGER_INPUT_UI ? _uiInputFallbacks : _jobInputFallbacks;
	fallbacks[index].Publish(edge);
	auto& publicationCount = domain == TRIGGER_INPUT_UI ?
		_uiFallbackPublicationCount : _jobFallbackPublicationCount;
	publicationCount.fetch_add(1u, std::memory_order_release);
}

bool Trigger::_InputFallbacksConsumed(
	const std::array<InputFallback, _InputFallbackCount>& fallbacks,
	const std::array<std::uint64_t, _InputFallbackCount>& consumedSequences) const noexcept
{
	for (std::size_t index = 0u; index < _InputFallbackCount; ++index)
	{
		if (fallbacks[index].Sequence.load(std::memory_order_acquire) != consumedSequences[index])
			return false;
	}
	return true;
}

void Trigger::InputFallback::Publish(const TriggerInputEdge& edge) noexcept
{
	Sequence.fetch_add(1u, std::memory_order_acq_rel);
	RigRevision.store(edge.RigRevision, std::memory_order_relaxed);
	BindingIndex.store(edge.BindingIndex, std::memory_order_relaxed);
	Control.store(static_cast<std::uint8_t>(edge.Control), std::memory_order_relaxed);
	Edge.store(static_cast<std::uint8_t>(edge.Edge), std::memory_order_relaxed);
	EventTimeUsec.store(edge.EventTimeUsec, std::memory_order_relaxed);
	Sequence.fetch_add(1u, std::memory_order_release);
}

bool Trigger::InputFallback::ReadLatest(std::uint64_t consumedSequence,
	TriggerInputEdge& edge,
	std::uint64_t& observedSequence) const noexcept
{
	const auto before = Sequence.load(std::memory_order_acquire);
	if (before == 0u || before == consumedSequence || (before & 1u) != 0u)
		return false;
	edge.RigRevision = RigRevision.load(std::memory_order_relaxed);
	edge.BindingIndex = BindingIndex.load(std::memory_order_relaxed);
	edge.Control = static_cast<TriggerControl>(Control.load(std::memory_order_relaxed));
	edge.Edge = static_cast<TriggerEdge>(Edge.load(std::memory_order_relaxed));
	edge.EventTimeUsec = EventTimeUsec.load(std::memory_order_relaxed);
	const auto after = Sequence.load(std::memory_order_acquire);
	if (before != after || (after & 1u) != 0u)
		return false;
	observedSequence = after;
	return true;
}

void Trigger::_PublishTriggerStateSnapshot() noexcept
{
	_publishedActivateInputDown.store(_isLastActivateDownRaw, std::memory_order_release);
	_publishedDitchInputDown.store(_isLastDitchDownRaw, std::memory_order_release);
	_publishedTriggerDitchDown.store(_isDitchDown, std::memory_order_release);
	_publishedTriggerState.store(static_cast<std::uint8_t>(_state), std::memory_order_release);
	_publishedCanEditRouting.store(_CanEditRoutingAtAudioBoundary(), std::memory_order_release);
	_publishedCanApplyCaptureRouting.store(_CanApplyCaptureRoutingAtAudioBoundary(), std::memory_order_release);
}

bool Trigger::AddBinding(DualBinding activate, DualBinding ditch)
{
	if (_activateBindings.size() >= _InputFallbackBindingCapacity ||
		_ditchBindings.size() >= _InputFallbackBindingCapacity)
		return false;
	_activateBindings.push_back(activate);
	_ditchBindings.push_back(ditch);
	return true;
}

void Trigger::RemoveBinding(DualBinding activate, DualBinding ditch)
{
	auto foundBinding = std::find(_activateBindings.begin(), _activateBindings.end(), activate);
	if (foundBinding != _activateBindings.end())
		_activateBindings.erase(foundBinding);

	foundBinding = std::find(_ditchBindings.begin(), _ditchBindings.end(), ditch);
	if (foundBinding != _ditchBindings.end())
		_ditchBindings.erase(foundBinding);
}

void Trigger::ClearBindings()
{
	_activateBindings.clear();
	_ditchBindings.clear();
}

void Trigger::AddInputChannel(unsigned int chan)
{
	_inputChannels.push_back(chan);

	_UpdateBehaviour();
}

void Trigger::RemoveInputChannel(unsigned int chan)
{
	auto inChan = std::find(_inputChannels.begin(), _inputChannels.end(), chan);
	if (inChan != _inputChannels.end())
		_inputChannels.erase(inChan);

	_UpdateBehaviour();
}

void Trigger::ClearInputChannels()
{
	_inputChannels.clear();

	_UpdateBehaviour();
}

void Trigger::AddMidiInputDevice(std::string device)
{
	if (device.empty())
		return;

	if (_midiInputDevices.end() == std::find(_midiInputDevices.begin(), _midiInputDevices.end(), device))
		_midiInputDevices.push_back(std::move(device));
}

void Trigger::ApplyCaptureRouting(std::shared_ptr<base::ActionReceiver>& receiver,
	std::vector<unsigned int>& inputChannels,
	std::vector<std::string>& midiInputDevices,
	io::RigFile::Trigger::MidiInputMode& midiInputMode,
	std::shared_ptr<audio::AudioMixer>& overdubMixer,
	std::shared_ptr<base::BounceWriter>& overdubWriter) noexcept
{
	_receiver.swap(receiver);
	_inputChannels.swap(inputChannels);
	_midiInputDevices.swap(midiInputDevices);
	std::swap(_midiInputMode, midiInputMode);
	_overdubMixer.swap(overdubMixer);
	_overdubWriter.swap(overdubWriter);
}

TriggerState Trigger::GetState() const
{
	return static_cast<TriggerState>(_publishedTriggerState.load(std::memory_order_acquire));
}

bool Trigger::IsActivateInputDown() const
{
	return _publishedActivateInputDown.load(std::memory_order_acquire);
}

bool Trigger::IsDitchInputDown() const
{
	return _publishedDitchInputDown.load(std::memory_order_acquire);
}

bool Trigger::IsDitchDown() const
{
	return _publishedTriggerDitchDown.load(std::memory_order_acquire);
}

void Trigger::Reset()
{
	_state = TRIGSTATE_DEFAULT;
	_isDitchDown = false;
	_isLastActivateDown = false;
	_isLastDitchDown = false;
	_isLastActivateDownRaw = false;
	_isLastDitchDownRaw = false;
	_lastActivateTime = Timer::GetZero();
	_lastDitchTime = Timer::GetZero();
	
	for (auto& b : _activateBindings)
		b.Reset();

	for (auto& b : _ditchBindings)
		b.Reset();

	_state = TriggerState::TRIGSTATE_DEFAULT;
	_PublishTriggerStateSnapshot();
	_recordSampCount = 0;
	_loopTakeHistorySize = 0u;
	_activeHistoryIndex.reset();
	_delayedActionCount = 0u;
	_delayedPunchActionCount = 0u;
	_pendingCompletion = STRUCTURAL_NONE;
	_pendingSequence = 0u;
	_pendingHistoryToken = 0u;
	_pendingDitchDelayedActionCount = 0u;
	_pendingDitchDelayedPunchActionCount = 0u;
	_structuralCommands.Clear();
	_structuralResults.Clear();
	for (std::size_t i = 0u; i < _jobTakeHistorySize; ++i)
	{
		_jobTakeHistory[i] = TriggerTake{};
		_jobTakeTokens[i] = 0u;
	}
	_jobTakeHistorySize = 0u;
	_PublishJobHistory();
	_uiInputQueue.Clear();
	_jobInputQueue.Clear();
	for (auto& fallback : _uiInputFallbacks)
		fallback.Sequence.store(0u, std::memory_order_release);
	for (auto& fallback : _jobInputFallbacks)
		fallback.Sequence.store(0u, std::memory_order_release);
	_uiFallbackPublicationCount.store(0u, std::memory_order_release);
	_jobFallbackPublicationCount.store(0u, std::memory_order_release);
	_consumedUiFallbackSequences.fill(0u);
	_consumedJobFallbackSequences.fill(0u);
	_consumedUiFallbackPublicationCount = 0u;
	_consumedJobFallbackPublicationCount = 0u;
}

std::string Trigger::Name() const
{
	return _name;
}

void Trigger::SetName(std::string name)
{
	_name = name;
}

std::vector<TriggerTake> Trigger::GetTakes() const
{
	const auto history = _publishedTakeHistory.load(std::memory_order_acquire);
	return history ? *history : std::vector<TriggerTake>();
}

void Trigger::RestoreTakes(std::vector<TriggerTake> takes)
{
	// Restore the most recent entries into both bounded ledgers before audio starts.
	const auto first = takes.size() > _HistoryCapacity ? takes.size() - _HistoryCapacity : 0u;
	for (auto index = first; index < takes.size(); ++index)
	{
		const auto slot = _loopTakeHistorySize++;
		const auto token = _nextHistoryToken++;
		_loopTakeHistory[slot].SourceType = takes[index].SourceType;
		_loopTakeHistory[slot].Token = token;
		takes[index].Receiver = _receiver;
		_jobTakeHistory[slot] = std::move(takes[index]);
		_jobTakeTokens[slot] = token;
		++_jobTakeHistorySize;
	}
	_PublishJobHistory();
}

void Trigger::WriteBlock(const std::shared_ptr<MultiAudioSink> dest,
	const float* srcBuf,
	unsigned int numSamps,
	unsigned int destChannel)
{
	std::size_t write = 0u;
	for (std::size_t i = 0u; i < _delayedActionCount; ++i)
	{
		const auto action = _delayedActions[i];
		if (action.SampsLeft == 0u)
			_overdubMixer->SetUnmutedLevel(action.Target);
		else
			_delayedActions[write++] = action;
	}
	_delayedActionCount = write;

	PreparedTriggerBounceWriter::Write(_overdubMixer, dest, srcBuf, numSamps, destChannel);
}

std::optional<std::size_t> Trigger::_FindJobHistory(std::uint64_t token) const noexcept
{
	for (std::size_t i = 0u; i < _jobTakeHistorySize; ++i)
		if (_jobTakeTokens[i] == token) return i;
	return std::nullopt;
}

void Trigger::_PublishJobHistory()
{
	auto history = std::make_shared<const std::vector<TriggerTake>>(
		_jobTakeHistory.begin(), _jobTakeHistory.begin() + _jobTakeHistorySize);
	_publishedTakeHistory.store(std::move(history), std::memory_order_release);
}

bool Trigger::_QueueStructuralCommand(TriggerAction::TriggerActionType actionType,
	StructuralCompletion completion,
	std::uint64_t historyToken,
	unsigned long sampleCount,
	bool applyToTargetTake,
	bool applyToSourceTake,
	bool applyToTargetAudio,
	bool applyToTargetMidi) noexcept
{
	StructuralCommand command;
	command.Sequence = _nextStructuralSequence++;
	command.ActionType = actionType;
	command.Completion = completion;
	command.HistoryToken = historyToken;
	command.SampleCount = sampleCount;
	command.ApplyToTargetTake = applyToTargetTake;
	command.ApplyToSourceTake = applyToSourceTake;
	command.ApplyToTargetAudio = applyToTargetAudio;
	command.ApplyToTargetMidi = applyToTargetMidi;
	if (!_structuralCommands.Push(command))
	{
		_structuralCommandDropCount.fetch_add(1u, std::memory_order_relaxed);
		return false;
	}
	if (completion != STRUCTURAL_NONE)
	{
		_pendingPriorState = _state;
		_pendingPriorActiveIndex = _activeHistoryIndex;
		_pendingCompletion = completion;
		_pendingSequence = command.Sequence;
		_pendingHistoryToken = historyToken;
		_pendingRecordSamps = 0u;
		_pendingDitchDelayedActionCount = 0u;
		_pendingDitchDelayedPunchActionCount = 0u;
	}
	return true;
}

void Trigger::_FlushDelayedPunchActions(unsigned int samps) noexcept
{
	std::size_t write = 0u;
	for (std::size_t i = 0u; i < _delayedPunchActionCount; ++i)
	{
		auto delayed = _delayedPunchActions[i];
		delayed.SampsLeft = samps >= delayed.SampsLeft ? 0u : delayed.SampsLeft - samps;
		if (delayed.SampsLeft == 0u)
		{
			if (delayed.TargetTake)
			{
				if (delayed.IsPunchIn) delayed.TargetTake->TriggerPunchInAudio();
				else delayed.TargetTake->TriggerPunchOutAudio();
			}
		}
		else
			_delayedPunchActions[write++] = delayed;
	}
	_delayedPunchActionCount = write;
}

void Trigger::ProcessStructuralActionsOnJob(
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	StructuralCommand command;
	if (!_structuralCommands.Peek(command))
		return;
	// The queue remains visibly non-empty until this counter is raised, closing
	// the Peek->Pop publication gap for audio-boundary checks.
	_jobStructuralActionsInFlight.fetch_add(1u, std::memory_order_acq_rel);
	for (std::size_t consumed = 0u;
		consumed < _StructuralQueueCapacity && _structuralCommands.Pop(command);
		++consumed)
	{
		std::shared_ptr<base::ActionReceiver> receiver;
		const TriggerTake* take = nullptr;
		std::optional<std::size_t> jobHistoryIndex;
		const bool isStart = command.ActionType == TriggerAction::TRIGGER_REC_START ||
			command.ActionType == TriggerAction::TRIGGER_OVERDUB_START;
		if (isStart)
			receiver = _receiver;
		else if ((jobHistoryIndex = _FindJobHistory(command.HistoryToken)))
		{
			take = &_jobTakeHistory[*jobHistoryIndex];
			receiver = take->Receiver;
		}

		ActionResult actionResult = ActionResult::NoAction();
		if (receiver)
		{
			TriggerAction action;
			action.ActionType = command.ActionType;
			action.SampleCount = command.SampleCount;
			action.ApplyToTargetTake = command.ApplyToTargetTake;
			action.ApplyToSourceTake = command.ApplyToSourceTake;
			action.ApplyToTargetAudio = command.ApplyToTargetAudio;
			action.ApplyToTargetMidi = command.ApplyToTargetMidi;
			if (take)
			{
				action.SourceId = take->SourceTakeId;
				action.TargetId = take->TargetTakeId;
			}
			if (isStart)
			{
				action.InputChannels = _inputChannels;
				action.MidiInputDevices = _midiInputDevices;
				if (command.ActionType == TriggerAction::TRIGGER_OVERDUB_START)
					action.OverdubWriter = _overdubWriter;
			}
			if (cfg) action.SetUserConfig(*cfg);
			if (params) action.SetAudioParams(*params);
			actionResult = receiver->OnAction(action);
			if (isStart && actionResult.IsEaten)
			{
				if (_jobTakeHistorySize < _HistoryCapacity)
				{
					_jobTakeTokens[_jobTakeHistorySize] = command.HistoryToken;
					_jobTakeHistory[_jobTakeHistorySize++] = {
						TriggerTake::SOURCE_ADC, actionResult.SourceId,
						actionResult.TargetId, receiver };
					_PublishJobHistory();
				}
				else
					actionResult.IsEaten = false;
			}

			if (command.ActionType == TriggerAction::TRIGGER_DITCH &&
				(actionResult.DitchResult == actions::DitchDisposition::Removed ||
				 actionResult.DitchResult == actions::DitchDisposition::AlreadyAbsent) && take)
			{
				TriggerAction unmute;
				unmute.ActionType = TriggerAction::TRIGGER_DITCH_UNMUTE;
				unmute.TargetId = take->SourceTakeId;
				unmute.SampleCount = command.SampleCount;
				if (cfg) unmute.SetUserConfig(*cfg);
				if (params) unmute.SetAudioParams(*params);
				receiver->OnAction(unmute);
			}
			if ((command.ActionType == TriggerAction::TRIGGER_DITCH ||
				command.ActionType == TriggerAction::TRIGGER_OVERDUB_DITCH) &&
				(actionResult.DitchResult == actions::DitchDisposition::Removed ||
				 actionResult.DitchResult == actions::DitchDisposition::AlreadyAbsent) &&
				jobHistoryIndex)
			{
				for (auto i = *jobHistoryIndex + 1u; i < _jobTakeHistorySize; ++i)
				{
					_jobTakeHistory[i - 1u] = std::move(_jobTakeHistory[i]);
					_jobTakeTokens[i - 1u] = _jobTakeTokens[i];
				}
				--_jobTakeHistorySize;
				_jobTakeHistory[_jobTakeHistorySize] = TriggerTake{};
				_jobTakeTokens[_jobTakeHistorySize] = 0u;
				_PublishJobHistory();
			}
		}

		if (command.Completion != STRUCTURAL_NONE)
		{
			StructuralResult result;
			result.Sequence = command.Sequence;
			result.Completion = command.Completion;
			result.IsEaten = actionResult.IsEaten;
			result.DitchResult = actionResult.DitchResult;
			result.HistoryToken = command.HistoryToken;
			if (const auto sourceTake = actionResult.TriggerSourceTake.lock())
				result.SourceTake = sourceTake.get();
			if (const auto targetTake = actionResult.TriggerTargetTake.lock())
				result.TargetTake = targetTake.get();
			const auto pushed = _structuralResults.Push(result);
			assert(pushed && "one pending structural transition must always have result capacity");
			(void)pushed;
		}
	}
	_jobStructuralActionsInFlight.fetch_sub(1u, std::memory_order_release);
}

void Trigger::_EraseHistory(std::size_t index) noexcept
{
	if (index >= _loopTakeHistorySize)
		return;
	for (auto i = index + 1u; i < _loopTakeHistorySize; ++i)
		_loopTakeHistory[i - 1u] = std::move(_loopTakeHistory[i]);
	--_loopTakeHistorySize;
	_loopTakeHistory[_loopTakeHistorySize] = RuntimeTriggerTake{};
}

void Trigger::_ApplyStructuralResult(const StructuralResult& result) noexcept
{
	if (result.Completion == STRUCTURAL_NONE ||
		result.Sequence != _pendingSequence || result.Completion != _pendingCompletion)
		return;

	switch (result.Completion)
	{
	case STRUCTURAL_START_RECORDING:
	case STRUCTURAL_START_OVERDUB:
		if (result.IsEaten && _loopTakeHistorySize < _HistoryCapacity)
		{
			auto& take = _loopTakeHistory[_loopTakeHistorySize];
			take.SourceType = TriggerTake::SOURCE_ADC;
			take.Token = result.HistoryToken;
			take.SourceTake = result.SourceTake;
			take.TargetTake = result.TargetTake;
			_activeHistoryIndex = _loopTakeHistorySize++;
			_state = result.Completion == STRUCTURAL_START_RECORDING ?
				TRIGSTATE_RECORDING : TRIGSTATE_OVERDUBBING;
			_activationOutcomeCount.fetch_add(1u, std::memory_order_release);
		}
		else
		{
			_state = TRIGSTATE_DEFAULT;
			_activeHistoryIndex.reset();
		}
		break;
	case STRUCTURAL_END_RECORDING:
	case STRUCTURAL_END_OVERDUB:
		if (result.IsEaten)
		{
			_state = TRIGSTATE_DEFAULT;
			_activeHistoryIndex.reset();
			_activationOutcomeCount.fetch_add(1u, std::memory_order_release);
		}
		else
		{
			_state = _pendingPriorState;
			_activeHistoryIndex = _pendingPriorActiveIndex;
			_recordSampCount.fetch_add(_pendingRecordSamps, std::memory_order_relaxed);
		}
		break;
	case STRUCTURAL_DITCH:
	case STRUCTURAL_DITCH_OVERDUB:
		if (result.DitchResult == actions::DitchDisposition::Removed ||
			result.DitchResult == actions::DitchDisposition::AlreadyAbsent)
		{
			std::size_t historyIndex = _HistoryCapacity;
			for (std::size_t i = 0u; i < _loopTakeHistorySize; ++i)
				if (_loopTakeHistory[i].Token == _pendingHistoryToken) { historyIndex = i; break; }
			if (historyIndex < _loopTakeHistorySize)
				_EraseHistory(historyIndex);
			_ditchOutcomeCount.fetch_add(1u, std::memory_order_release);
		}
		if (result.DitchResult == actions::DitchDisposition::Removed ||
			result.DitchResult == actions::DitchDisposition::AlreadyAbsent)
		{
			_state = TRIGSTATE_DEFAULT;
			_activeHistoryIndex.reset();
			_pendingDitchDelayedActionCount = 0u;
			_pendingDitchDelayedPunchActionCount = 0u;
		}
		else
		{
			_state = _pendingPriorState;
			_activeHistoryIndex = _pendingPriorActiveIndex;
			_recordSampCount.fetch_add(_pendingRecordSamps, std::memory_order_relaxed);
			_delayedActionCount = _pendingDitchDelayedActionCount;
			for (std::size_t i = 0u; i < _delayedActionCount; ++i)
			{
				_delayedActions[i] = _pendingDitchDelayedActions[i];
				_delayedActions[i].SampsLeft = _pendingRecordSamps >= _delayedActions[i].SampsLeft ?
					0u : _delayedActions[i].SampsLeft - static_cast<unsigned int>(_pendingRecordSamps);
			}
			_delayedPunchActionCount = _pendingDitchDelayedPunchActionCount;
			for (std::size_t i = 0u; i < _delayedPunchActionCount; ++i)
			{
				_delayedPunchActions[i] = _pendingDitchDelayedPunchActions[i];
				_delayedPunchActions[i].SampsLeft = _pendingRecordSamps >= _delayedPunchActions[i].SampsLeft ?
					0u : _delayedPunchActions[i].SampsLeft - static_cast<unsigned int>(_pendingRecordSamps);
			}
		}
		break;
	default:
		break;
	}
	_pendingCompletion = STRUCTURAL_NONE;
	_pendingSequence = 0u;
	_pendingHistoryToken = 0u;
	_pendingRecordSamps = 0u;
}

void Trigger::_ProcessStructuralResults() noexcept
{
	StructuralResult result;
	while (_structuralResults.Pop(result))
		_ApplyStructuralResult(result);
}

bool Trigger::IgnoreRepeats(bool isActivate, DualBinding::TestResult trigResult)
{
	bool allowedThrough = true;

	if (isActivate)
	{
		if ((DualBinding::MATCH_DOWN == trigResult) && _isLastActivateDownRaw)
			allowedThrough = false;
		else if ((DualBinding::MATCH_RELEASE == trigResult) && !_isLastActivateDownRaw)
			allowedThrough = false;
	}
	else
	{
		if ((DualBinding::MATCH_DOWN == trigResult) && _isLastDitchDownRaw)
			allowedThrough = false;
		else if ((DualBinding::MATCH_RELEASE == trigResult) && !_isLastDitchDownRaw)
			allowedThrough = false;
	}

	return allowedThrough;
}

bool Trigger::Debounce(bool isActivate,
	DualBinding::TestResult trigResult,
	Time actionTime)
{
	auto allowedThrough = false;

	if (isActivate)
	{
		auto isDebounceBypassed = Timer::IsZero(_lastActivateTime) || (0 == _debounceTimeMs);
		auto elapsedMs = Timer::GetElapsedSeconds(_lastActivateTime, actionTime) * 1000.0;

		if ((DualBinding::MATCH_DOWN == trigResult) && !_isLastActivateDownRaw)
		{
			_lastActivateTime = actionTime;
			_isLastActivateDownRaw = true;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastActivateDown = true;
			}
		}
		else if ((DualBinding::MATCH_RELEASE == trigResult) && _isLastActivateDownRaw)
		{
			_lastActivateTime = actionTime;
			_isLastActivateDownRaw = false;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastActivateDown = false;
			}
		}
	}
	else
	{
		auto isDebounceBypassed = Timer::IsZero(_lastDitchTime) || (0 == _debounceTimeMs);
		auto elapsedMs = Timer::GetElapsedSeconds(_lastDitchTime, actionTime) * 1000.0;

		if ((DualBinding::MATCH_DOWN == trigResult) && !_isLastDitchDownRaw)
		{
			_lastDitchTime = actionTime;
			_isLastDitchDownRaw = true;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastDitchDown = true;
			}
		}
		else if ((DualBinding::MATCH_RELEASE == trigResult) && _isLastDitchDownRaw)
		{
			_lastDitchTime = actionTime;
			_isLastDitchDownRaw = false;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastDitchDown = false;
			}
		}
	}

	return allowedThrough;
}

bool Trigger::TryChangeState(DualBinding& binding,
	bool isActivate,
	TriggerSource source,
	unsigned int value,
	unsigned int state,
	const base::Action& action,
	const std::string& device)
{
	auto trigResult = binding.OnTrigger(
		source,
		value,
		state,
		device);

	bool allowedThrough = IgnoreRepeats(isActivate, trigResult);
	if (!allowedThrough)
	{
		return false;
	}

	allowedThrough = Debounce(isActivate,
		trigResult,
		action.GetActionTime());

	if (!allowedThrough)
	{
		return false;
	}

	switch (trigResult)
	{
	case DualBinding::MATCH_DOWN:
		StateMachine(true, isActivate, action.GetUserConfig(), action.GetAudioParams());
		return true;
	case DualBinding::MATCH_RELEASE:
		StateMachine(false, isActivate, action.GetUserConfig(), action.GetAudioParams());
		return true;
	}

	return false;
}

bool Trigger::StateMachine(bool isDown,
	bool isActivate,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	bool changedState = false;

	switch (_state)
	{
	case TRIGSTATE_DEFAULT:
		if (isActivate)
		{
			if (isDown)
			{
				if (_isDitchDown)
				{
					StartOverdub(cfg, params);
					_isDitchDown = false; // Prevent next release ditching the playing loop
					_isLastDitchDownRaw = false;
					_isLastDitchDown = false;
					for (auto& binding : _ditchBindings)
						binding.Reset();
				}
				else
					StartRecording(cfg, params);
			}

			changedState = true;
		}
		else
		{
			if (isDown)
				_isDitchDown = true;
			else if (_isDitchDown)
			{
				Ditch(cfg, params);
				_isDitchDown = false;
				changedState = true;
			}
		}
		break;
	case TRIGSTATE_RECORDING:
		if (isActivate)
		{
			if (isDown)
			{
				EndRecording(cfg, params);
				_isDitchDown = false; // Prevent next release ditching the playing loop
				_isLastDitchDownRaw = false;
				_isLastDitchDown = false;
				for (auto& binding : _ditchBindings)
					binding.Reset();
				changedState = true;
			}
		}
		else
		{
			if (isDown)
				_isDitchDown = true;
			else if (_isDitchDown)
			{
				Ditch(cfg, params);
				_isDitchDown = false;
				changedState = true;
			}
		}
		break;
	case TRIGSTATE_OVERDUBBING:
		if (isActivate)
		{
			if (isDown)
			{
				if (_isDitchDown)
				{
					EndOverdub(cfg, params);
					_isDitchDown = false; // Prevent next release ditching the playing loop
					_isLastDitchDownRaw = false;
					_isLastDitchDown = false;
					for (auto& binding : _ditchBindings)
						binding.Reset();
					changedState = true;
				}
				else
				{
					changedState = StartPunchIn(cfg, params);
				}
			}
		}
		else
		{
			if (isDown)
			{
				_isDitchDown = true;
			}
			else if (_isDitchDown)
			{
				Ditch(cfg, params);
				_isDitchDown = false;
				changedState = true;
			}
		}
		break;
	case TRIGSTATE_PUNCHEDIN:
		if (isActivate)
		{
			if (!isDown)
			{
				// End punch-in but maintain overdub mode (release)
				changedState = EndPunchIn(cfg, params);
			}
		}
		else
		{
			if (isDown)
				_isDitchDown = true;
			else
				_isDitchDown = false;
		}

		break;
	}

	_PublishTriggerStateSnapshot();
	return changedState;
}

void Trigger::StartRecording(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	_recordSampCount = 0;
	_delayedActionCount = 0u;
	_activeHistoryIndex.reset();

	if (_receiver && _loopTakeHistorySize < _HistoryCapacity)
	{
		auto historyToken = _nextHistoryToken++;
		if (historyToken == 0u) historyToken = _nextHistoryToken++;
		_QueueStructuralCommand(TriggerAction::TRIGGER_REC_START,
			STRUCTURAL_START_RECORDING, historyToken, 0u);
	}
}

void Trigger::EndRecording(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	if (_activeHistoryIndex && *_activeHistoryIndex < _loopTakeHistorySize)
		_QueueStructuralCommand(TriggerAction::TRIGGER_REC_END,
			STRUCTURAL_END_RECORDING,
			_loopTakeHistory[*_activeHistoryIndex].Token, _recordSampCount);
}

void Trigger::Ditch(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	const auto historyIndex = _activeHistoryIndex ? _activeHistoryIndex :
		(_loopTakeHistorySize == 0u ? std::optional<std::size_t>() :
			std::optional<std::size_t>(_loopTakeHistorySize - 1u));

	if (historyIndex && *historyIndex < _loopTakeHistorySize &&
		_QueueStructuralCommand(TriggerAction::TRIGGER_DITCH,
			STRUCTURAL_DITCH,
			_loopTakeHistory[*historyIndex].Token, _recordSampCount))
	{
		_pendingDitchDelayedActionCount = _delayedActionCount;
		std::copy_n(_delayedActions.begin(), _delayedActionCount,
			_pendingDitchDelayedActions.begin());
		_pendingDitchDelayedPunchActionCount = _delayedPunchActionCount;
		std::copy_n(_delayedPunchActions.begin(), _delayedPunchActionCount,
			_pendingDitchDelayedPunchActions.begin());
		_delayedActionCount = 0u;
		_delayedPunchActionCount = 0u;
	}
}

void Trigger::StartOverdub(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	_recordSampCount = 0;
	_delayedActionCount = 0u;
	_delayedPunchActionCount = 0u;
	_activeHistoryIndex.reset();
	_overdubMixer->SetUnmutedLevel(1.0);

	if (_receiver && _loopTakeHistorySize < _HistoryCapacity)
	{
		auto historyToken = _nextHistoryToken++;
		if (historyToken == 0u) historyToken = _nextHistoryToken++;
		_QueueStructuralCommand(TriggerAction::TRIGGER_OVERDUB_START,
			STRUCTURAL_START_OVERDUB, historyToken, 0u);
	}
}

void Trigger::EndOverdub(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	if (_activeHistoryIndex && *_activeHistoryIndex < _loopTakeHistorySize)
		_QueueStructuralCommand(TriggerAction::TRIGGER_OVERDUB_END,
			STRUCTURAL_END_OVERDUB,
			_loopTakeHistory[*_activeHistoryIndex].Token, _recordSampCount);
}

void Trigger::DitchOverdub(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	(void)cfg;
	(void)params;
	const auto historyIndex = _activeHistoryIndex;

	if (historyIndex && *historyIndex < _loopTakeHistorySize &&
		_QueueStructuralCommand(TriggerAction::TRIGGER_OVERDUB_DITCH,
			STRUCTURAL_DITCH_OVERDUB,
			_loopTakeHistory[*historyIndex].Token, _recordSampCount))
	{
		_pendingDitchDelayedActionCount = _delayedActionCount;
		std::copy_n(_delayedActions.begin(), _delayedActionCount,
			_pendingDitchDelayedActions.begin());
		_pendingDitchDelayedPunchActionCount = _delayedPunchActionCount;
		std::copy_n(_delayedPunchActions.begin(), _delayedPunchActionCount,
			_pendingDitchDelayedPunchActions.begin());
		_delayedActionCount = 0u;
		_delayedPunchActionCount = 0u;
	}
}

bool Trigger::StartPunchIn(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	if (!_activeHistoryIndex || *_activeHistoryIndex >= _loopTakeHistorySize)
		return false;
	const auto& history = _loopTakeHistory[*_activeHistoryIndex];
	const auto hasTargetAudio = !_inputChannels.empty() || !cfg.has_value() ||
		cfg.value().Audio.NumChannelsIn > 0u;
	const auto hasTargetMidi = !_midiInputDevices.empty();
	if (!_QueueStructuralCommand(TriggerAction::TRIGGER_PUNCHIN_START,
		STRUCTURAL_NONE, history.Token, _recordSampCount,
		hasTargetMidi, false, false, hasTargetMidi))
		return false;

	_state = TRIGSTATE_PUNCHEDIN;
	auto sampsDelay = CalcInputAlignedDelaySamps(cfg, params);
	// Mute overdub input immediately; latency compensation applies only to mixer fade
	if (sampsDelay == 0u)
		_overdubMixer->SetUnmutedLevel(0.0);
	else if (_delayedActionCount < _DelayedActionCapacity)
		_delayedActions[_delayedActionCount++] = { sampsDelay, 0.0 };
	else
		_overdubMixer->SetUnmutedLevel(0.0);

	if (history.SourceTake) history.SourceTake->SetTriggerSourceMutedAudio(true);

	auto targetDelay = CalcPunchStateDelaySamps(cfg);
	if (hasTargetAudio && history.TargetTake)
	{
		if (0u == targetDelay)
			history.TargetTake->TriggerPunchInAudio();
		else if (_delayedPunchActionCount < _DelayedActionCapacity)
			_delayedPunchActions[_delayedPunchActionCount++] = {
				history.TargetTake, targetDelay, true };
		else
			history.TargetTake->TriggerPunchInAudio();
	}
	return true;
}

bool Trigger::EndPunchIn(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	if (!_activeHistoryIndex || *_activeHistoryIndex >= _loopTakeHistorySize)
		return false;
	const auto& history = _loopTakeHistory[*_activeHistoryIndex];
	const auto hasTargetAudio = !_inputChannels.empty() || !cfg.has_value() ||
		cfg.value().Audio.NumChannelsIn > 0u;
	const auto hasTargetMidi = !_midiInputDevices.empty();
	if (!_QueueStructuralCommand(TriggerAction::TRIGGER_PUNCHIN_END,
		STRUCTURAL_NONE, history.Token, _recordSampCount,
		hasTargetMidi, false, false, hasTargetMidi))
		return false;

	_state = TRIGSTATE_OVERDUBBING;
	auto sampsDelay = CalcInputAlignedDelaySamps(cfg, params);
	// Unmute overdub input immediately; latency compensation applies only to mixer fade
	if (sampsDelay == 0u)
		_overdubMixer->SetUnmutedLevel(1.0);
	else if (_delayedActionCount < _DelayedActionCapacity)
		_delayedActions[_delayedActionCount++] = { sampsDelay, 1.0 };
	else
		_overdubMixer->SetUnmutedLevel(1.0);

	if (history.SourceTake) history.SourceTake->SetTriggerSourceMutedAudio(false);

	auto targetDelay = CalcPunchStateDelaySamps(cfg);
	if (hasTargetAudio && history.TargetTake)
	{
		if (0u == targetDelay)
			history.TargetTake->TriggerPunchOutAudio();
		else if (_delayedPunchActionCount < _DelayedActionCapacity)
			_delayedPunchActions[_delayedPunchActionCount++] = {
				history.TargetTake, targetDelay, false };
		else
			history.TargetTake->TriggerPunchOutAudio();
	}
	return true;
}

unsigned int Trigger::CalcInputAlignedDelaySamps(const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params) const
{
	if (!cfg.has_value())
		return 0u;

	// Resolve latency from runtime stream params first; fallback to configured
	// input latency if stream params are unavailable or report 0.
	auto inputLatency = params.has_value() ?
		params.value().InputLatency :
		0u;

	if (0u == inputLatency)
		inputLatency = cfg.value().Audio.LatencyIn;

	// Audio written this callback corresponds to older captured ADC samples:
	// hardware input latency + additional ring-buffer read delay.
	auto readDelay = cfg.value().AdcBufferDelay(inputLatency);
	return inputLatency + readDelay;
}

unsigned int Trigger::CalcPunchStateDelaySamps(const std::optional<io::UserConfig>& cfg) const
{
	if (!cfg.has_value())
		return 0u;

	return cfg.value().TriggerLoopAlignmentSamps();
}

void Trigger::_UpdateBehaviour()
{
	_overdubMixer->SetBehaviour(
		std::move(std::make_unique<audio::BounceMixBehaviour>(
			GetOverdubBehaviourParams(_inputChannels))));
}
