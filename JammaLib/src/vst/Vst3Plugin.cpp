///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst3Plugin.h"
#include "../midi/MidiBlockTiming.h"
#include "Vst2Plugin.h"
#include "Vst3ControllerEditQueue.h"
#include "Vst3MidiMapping.h"
#include "Vst3StateBlob.h"
#include "VstGlContextScope.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef JAMMA_VST3_ENABLED
#include "vst3sdk/pluginterfaces/base/ibstream.h"
#include "vst3sdk/pluginterfaces/base/ipluginbase.h"
#include "vst3sdk/pluginterfaces/vst/ivstmessage.h"
#include "vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"
#include "vst3sdk/pluginterfaces/vst/ivstparameterchanges.h"
#include "vst3sdk/pluginterfaces/vst/ivsteditcontroller.h"
#include "vst3sdk/pluginterfaces/vst/ivstevents.h"
#include "vst3sdk/pluginterfaces/vst/ivstmidicontrollers.h"
#include "vst3sdk/pluginterfaces/gui/iplugview.h"
#include "vst3sdk/public.sdk/source/vst/hosting/hostclasses.h"
#endif

using namespace vst;

#ifdef JAMMA_VST3_ENABLED
using namespace Steinberg;
using namespace Steinberg::Vst;

class HostPlugFrame final : public IPlugFrame
{
public:
	HostPlugFrame() :
		_hostWindow(nullptr),
		_frameWindow(nullptr)
	{
		FUNKNOWN_CTOR
	}

	~HostPlugFrame() noexcept { FUNKNOWN_DTOR }

	void SetHostWindow(HWND hostWindow) noexcept
	{
		_hostWindow = hostWindow;
		_frameWindow = hostWindow ? GetAncestor(hostWindow, GA_ROOT) : nullptr;
		if (!_frameWindow)
			_frameWindow = hostWindow;
	}

	tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
	{
		if (!view || !newSize || !_hostWindow)
			return kInvalidArgument;

		const auto width = std::max<int32>(0, newSize->getWidth());
		const auto height = std::max<int32>(0, newSize->getHeight());

		if (_frameWindow)
		{
			RECT frameRect{ 0, 0, width, height };
			const auto style = static_cast<DWORD>(GetWindowLongPtr(_frameWindow, GWL_STYLE));
			const auto exStyle = static_cast<DWORD>(GetWindowLongPtr(_frameWindow, GWL_EXSTYLE));
			AdjustWindowRectEx(&frameRect, style, FALSE, exStyle);

			SetWindowPos(_frameWindow,
				nullptr,
				0,
				0,
				frameRect.right - frameRect.left,
				frameRect.bottom - frameRect.top,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		if (_hostWindow != _frameWindow)
		{
			SetWindowPos(_hostWindow,
				nullptr,
				0,
				0,
				width,
				height,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		const auto onSizeResult = view->onSize(newSize);
		return onSizeResult;
	}

	DECLARE_FUNKNOWN_METHODS

private:
	HWND _hostWindow;
	HWND _frameWindow;
};

IMPLEMENT_FUNKNOWN_METHODS(HostPlugFrame, IPlugFrame, IPlugFrame::iid)

class HostComponentHandler final : public IComponentHandler
{
public:
	HostComponentHandler() :
		_owner(nullptr)
	{
		FUNKNOWN_CTOR
	}

	~HostComponentHandler() noexcept { FUNKNOWN_DTOR }

	void SetOwner(Vst3Plugin* owner) noexcept
	{
		_owner = owner;
	}

	tresult PLUGIN_API beginEdit(ParamID id) override
	{
		if (_owner)
			_owner->OnBeginEdit(static_cast<std::uint32_t>(id));
		return kResultOk;
	}

	tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override
	{
		if (_owner)
			_owner->OnControllerEdit(static_cast<std::uint32_t>(id), static_cast<float>(valueNormalized));
		return kResultOk;
	}

	tresult PLUGIN_API endEdit(ParamID id) override
	{
		if (_owner)
			_owner->OnEndEdit(static_cast<std::uint32_t>(id));
		return kResultOk;
	}

	tresult PLUGIN_API restartComponent(int32 flags) override
	{
		if (_owner)
			_owner->OnRestartComponent(static_cast<std::int32_t>(flags));
		return kResultOk;
	}

	private:
	Vst3Plugin* _owner;

	public:
	DECLARE_FUNKNOWN_METHODS
};

IMPLEMENT_FUNKNOWN_METHODS(HostComponentHandler, IComponentHandler, IComponentHandler::iid)

// Result of classifying a MIDI event's sample offset against the current
// process block window. Shared by FixedEventList::AddMidiEvent (native VST3
// events) and Vst3Plugin::SendMidiEvent (mapped controller parameter points)
// so the two paths cannot drift apart.
struct MidiBlockPlacement
{
	bool Accepted = false; // false => drop entirely (late arrival, non-realtime)
	int32 SampleOffset = 0;
};

class FixedEventList final : public IEventList
{
public:
	static constexpr int32 MaxEvents = 256;

	FixedEventList() :
		_count(0),
		_blockStartSample(0u),
		_blockNumSamples(0u),
		_events{}
	{
		FUNKNOWN_CTOR
	}

	~FixedEventList() noexcept { FUNKNOWN_DTOR }

	void BeginBlock(std::uint32_t blockStartSample, std::uint32_t numSamples) noexcept
	{
		_blockStartSample = blockStartSample;
		_blockNumSamples = numSamples;
		_count = 0;
	}

	std::uint32_t BlockStartSample() const noexcept { return _blockStartSample; }
	std::uint32_t BlockNumSamples() const noexcept { return _blockNumSamples; }

	// Classifies midiEvent's sample offset against [blockStartSample,
	// blockStartSample + blockNumSamples). Static so both the native-event path
	// (below) and the mapped-parameter path in Vst3Plugin::SendMidiEvent reuse
	// the exact same rule.
	static MidiBlockPlacement ComputeBlockPlacement(const midi::MidiEvent& midiEvent,
		std::uint32_t blockStartSample, std::uint32_t blockNumSamples, bool isRealtime) noexcept
	{
		MidiBlockPlacement placement;
		if (blockNumSamples == 0u)
			return placement;

		const auto position = midi::ClassifyMidiSampleInBlock(midiEvent.sampleOffset,
			blockStartSample, blockNumSamples);
		const bool inWindow = position == midi::MidiBlockSamplePosition::Due;
		if (!inWindow && !isRealtime)
			return placement;

		placement.Accepted = true;
		placement.SampleOffset = inWindow
			? static_cast<int32>(midi::MidiSampleDelta(midiEvent.sampleOffset, blockStartSample))
			: 0;
		return placement;
	}

	void AddMidiEvent(const midi::MidiEvent& midiEvent, bool isRealtime) noexcept
	{
		if (_count >= MaxEvents || 0u == _blockNumSamples)
			return;

		const auto placement = ComputeBlockPlacement(midiEvent, _blockStartSample, _blockNumSamples, isRealtime);
		if (!placement.Accepted)
			return;

		Event event{};
		event.busIndex = 0;
		event.sampleOffset = placement.SampleOffset;
		event.ppqPosition = 0.0;
		event.flags = isRealtime ? Event::kIsLive : 0;

		if (midiEvent.IsNoteOn())
		{
			event.type = Event::kNoteOnEvent;
			event.noteOn.channel = midiEvent.Channel();
			event.noteOn.pitch = midiEvent.data1;
			event.noteOn.tuning = 0.0f;
			event.noteOn.velocity = static_cast<float>(midiEvent.data2) / 127.0f;
			event.noteOn.length = 0;
			event.noteOn.noteId = -1;
		}
		else if (midiEvent.IsNoteOff())
		{
			event.type = Event::kNoteOffEvent;
			event.noteOff.channel = midiEvent.Channel();
			event.noteOff.pitch = midiEvent.data1;
			event.noteOff.tuning = 0.0f;
			event.noteOff.velocity = static_cast<float>(midiEvent.data2) / 127.0f;
			event.noteOff.noteId = -1;
		}
		else if (midiEvent.MessageType() == 0xA0) // Polyphonic Key Pressure
		{
			event.type = Event::kPolyPressureEvent;
			event.polyPressure.channel = midiEvent.Channel();
			event.polyPressure.pitch = midiEvent.data1;
			event.polyPressure.pressure = static_cast<float>(midiEvent.data2) / 127.0f;
			event.polyPressure.noteId = -1;
		}
		else if (midiEvent.MessageType() == 0xB0) // Control Change
		{
			event.type = Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = midiEvent.data1;
			event.midiCCOut.channel = static_cast<int8>(midiEvent.Channel());
			event.midiCCOut.value = static_cast<int8>(midiEvent.data2);
			event.midiCCOut.value2 = 0;
		}
		else if (midiEvent.MessageType() == 0xC0) // Program Change
		{
			event.type = Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = static_cast<uint8>(kCtrlProgramChange);
			event.midiCCOut.channel = static_cast<int8>(midiEvent.Channel());
			event.midiCCOut.value = static_cast<int8>(midiEvent.data1);
			event.midiCCOut.value2 = 0;
		}
		else if (midiEvent.MessageType() == 0xD0) // Channel Pressure
		{
			event.type = Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = static_cast<uint8>(kAfterTouch);
			event.midiCCOut.channel = static_cast<int8>(midiEvent.Channel());
			event.midiCCOut.value = static_cast<int8>(midiEvent.data1);
			event.midiCCOut.value2 = 0;
		}
		else if (midiEvent.MessageType() == 0xE0) // Pitch Bend
		{
			event.type = Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = static_cast<uint8>(kPitchBend);
			event.midiCCOut.channel = static_cast<int8>(midiEvent.Channel());
			event.midiCCOut.value = static_cast<int8>(midiEvent.data1);
			event.midiCCOut.value2 = static_cast<int8>(midiEvent.data2);
		}
		else
		{
			return;
		}

		_events[static_cast<size_t>(_count++)] = event;
	}

	int32 PLUGIN_API getEventCount() override { return _count; }

	tresult PLUGIN_API getEvent(int32 index, Event& e) override
	{
		if (index < 0 || index >= _count)
			return kInvalidArgument;

		e = _events[static_cast<size_t>(index)];
		return kResultOk;
	}

	tresult PLUGIN_API addEvent(Event& e) override
	{
		if (_count >= MaxEvents)
			return kOutOfMemory;

		_events[static_cast<size_t>(_count++)] = e;
		return kResultOk;
	}

	DECLARE_FUNKNOWN_METHODS

private:
	int32 _count;
	std::uint32_t _blockStartSample;
	std::uint32_t _blockNumSamples;
	std::array<Event, MaxEvents> _events;
};

IMPLEMENT_FUNKNOWN_METHODS(FixedEventList, IEventList, IEventList::iid)

class FixedParamValueQueue final : public IParamValueQueue
{
public:
	static constexpr int32 MaxPoints = 32;

	FixedParamValueQueue() :
		_paramId(0),
		_count(0),
		_offsets{},
		_values{}
	{
		FUNKNOWN_CTOR
	}

	~FixedParamValueQueue() noexcept { FUNKNOWN_DTOR }

	void Begin(ParamID paramId) noexcept
	{
		_paramId = paramId;
		_count = 0;
	}

	void Clear() noexcept
	{
		_count = 0;
	}

	ParamID PLUGIN_API getParameterId() override
	{
		return _paramId;
	}

	int32 PLUGIN_API getPointCount() override
	{
		return _count;
	}

	tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override
	{
		if (index < 0 || index >= _count)
			return kInvalidArgument;

		sampleOffset = _offsets[static_cast<size_t>(index)];
		value = _values[static_cast<size_t>(index)];
		return kResultOk;
	}

	tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override
	{
		if (_count >= MaxPoints)
		{
			index = _count - 1;
			if (index >= 0)
			{
				_offsets[static_cast<size_t>(index)] = sampleOffset;
				_values[static_cast<size_t>(index)] = value;
				return kResultOk;
			}
			return kOutOfMemory;
		}

		index = _count;
		_offsets[static_cast<size_t>(_count)] = sampleOffset;
		_values[static_cast<size_t>(_count)] = value;
		++_count;
		return kResultOk;
	}

	DECLARE_FUNKNOWN_METHODS

private:
	ParamID _paramId;
	int32 _count;
	std::array<int32, MaxPoints> _offsets;
	std::array<ParamValue, MaxPoints> _values;
};

IMPLEMENT_FUNKNOWN_METHODS(FixedParamValueQueue, IParamValueQueue, IParamValueQueue::iid)

class FixedParameterChanges final : public IParameterChanges
{
public:
	static constexpr int32 MaxQueues = 256;

	FixedParameterChanges() :
		_count(0),
		_queues{}
	{
		FUNKNOWN_CTOR
	}

	~FixedParameterChanges() noexcept { FUNKNOWN_DTOR }

	void BeginBlock() noexcept
	{
		_count = 0;
	}

	int32 PLUGIN_API getParameterCount() override
	{
		return _count;
	}

	IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
	{
		if (index < 0 || index >= _count)
			return nullptr;
		return &_queues[static_cast<size_t>(index)];
	}

	IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
	{
		for (int32 i = 0; i < _count; ++i)
		{
			auto& queue = _queues[static_cast<size_t>(i)];
			if (queue.getParameterId() == id)
			{
				index = i;
				return &queue;
			}
		}

		if (_count >= MaxQueues)
		{
			index = _count - 1;
			return nullptr;
		}

		auto& queue = _queues[static_cast<size_t>(_count)];
		queue.Begin(id);
		index = _count;
		++_count;
		return &queue;
	}

	DECLARE_FUNKNOWN_METHODS

private:
	int32 _count;
	std::array<FixedParamValueQueue, MaxQueues> _queues;
};

IMPLEMENT_FUNKNOWN_METHODS(FixedParameterChanges, IParameterChanges, IParameterChanges::iid)

// Minimal IBStream adapter over an owned std::vector<std::uint8_t>. Used for
// both directions of Vst3Plugin::GetState/SetState: constructed empty and
// grown by write() when capturing IComponent/IEditController state, or
// constructed as a fixed-size view over existing bytes and read by
// IComponent::setState / IEditController::setComponentState / ::setState
// when restoring. Not RT-safe; only ever used from GetState/SetState, which
// are non-RT-only entry points. Deliberately local to this translation unit
// rather than reusing the SDK's own MemoryStream, to avoid pulling another
// SDK implementation source into JammaLib.
class MemoryIBStream final : public Steinberg::IBStream
{
public:
	MemoryIBStream() noexcept : _data(), _position(0)
	{
		FUNKNOWN_CTOR
	}

	MemoryIBStream(const std::uint8_t* data, std::size_t size) : _data(), _position(0)
	{
		FUNKNOWN_CTOR
		if (data && size > 0)
			_data.assign(data, data + size);
	}

	~MemoryIBStream() noexcept { FUNKNOWN_DTOR }

	MemoryIBStream(const MemoryIBStream&) = delete;
	MemoryIBStream& operator=(const MemoryIBStream&) = delete;

	const std::vector<std::uint8_t>& Data() const noexcept { return _data; }

	tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
	{
		if (numBytesRead)
			*numBytesRead = 0;

		if (numBytes < 0 || (numBytes > 0 && !buffer) || _position < 0)
			return kInvalidArgument;

		if (numBytes == 0)
			return kResultOk;

		const auto pos = static_cast<std::size_t>(_position);
		if (pos >= _data.size())
			return kResultOk; // At/past EOF: zero bytes read is a valid short read.

		const auto available = _data.size() - pos;
		const auto toRead = (std::min)(static_cast<std::size_t>(numBytes), available);
		std::memcpy(buffer, _data.data() + pos, toRead);
		_position += static_cast<int64>(toRead);
		if (numBytesRead)
			*numBytesRead = static_cast<int32>(toRead);
		return kResultOk;
	}

	tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
	{
		if (numBytesWritten)
			*numBytesWritten = 0;

		if (numBytes < 0 || (numBytes > 0 && !buffer) || _position < 0)
			return kInvalidArgument;

		if (numBytes == 0)
			return kResultOk;

		const auto pos = static_cast<std::size_t>(_position);
		const auto count = static_cast<std::size_t>(numBytes);
		if (pos > (std::numeric_limits<std::size_t>::max)() - count)
			return kInvalidArgument; // Checked-add overflow guard.

		const auto required = pos + count;
		if (_data.size() < required)
			_data.resize(required);

		std::memcpy(_data.data() + pos, buffer, count);
		_position += static_cast<int64>(count);
		if (numBytesWritten)
			*numBytesWritten = numBytes;
		return kResultOk;
	}

	tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
	{
		int64 newPos = 0;
		switch (mode)
		{
		case IBStream::kIBSeekSet:
			newPos = pos;
			break;
		case IBStream::kIBSeekCur:
			if ((pos > 0 && _position > (std::numeric_limits<int64>::max)() - pos)
				|| (pos < 0 && _position < (std::numeric_limits<int64>::min)() - pos))
				return kInvalidArgument; // Checked-add overflow guard.
			newPos = _position + pos;
			break;
		case IBStream::kIBSeekEnd:
		{
			const auto size = static_cast<int64>(_data.size());
			if (pos > 0 && size > (std::numeric_limits<int64>::max)() - pos)
				return kInvalidArgument;
			newPos = size + pos;
			break;
		}
		default:
			return kInvalidArgument;
		}

		if (newPos < 0)
			return kInvalidArgument;

		_position = newPos;
		if (result)
			*result = _position;
		return kResultOk;
	}

	tresult PLUGIN_API tell(int64* pos) override
	{
		if (!pos)
			return kInvalidArgument;
		*pos = _position;
		return kResultOk;
	}

	DECLARE_FUNKNOWN_METHODS

private:
	std::vector<std::uint8_t> _data;
	int64 _position;
};

IMPLEMENT_FUNKNOWN_METHODS(MemoryIBStream, IBStream, IBStream::iid)

class Vst3Plugin::Impl
{
public:
	// Pre-init state: populated by PreInit() on the main thread.
	Steinberg::IPtr<Steinberg::IPluginFactory> factory;
	bool moduleInitialized = false; // true if InitDll() was called

	Steinberg::IPtr<Steinberg::Vst::HostApplication> hostApplication;
	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	// True only when controller was created via the separate IComponent2/
	// legacy-CID fallback path in Load() and successfully initialize()'d —
	// i.e. it is a distinct COM object from component that must receive its
	// own terminate() exactly once. False when controller is the same
	// object as component (obtained via component->queryInterface()),
	// which must NOT be terminated a second time.
	bool controllerIsSeparateObject = false;

	// Explicit lifecycle state, tracked directly rather than guessed from
	// non-null pointers, so failure-cleanup and normal-unload paths can
	// never call terminate()/setActive(false)/setProcessing(false) on an
	// object that was never successfully initialized/activated.
	bool componentInitialized = false;
	bool controllerInitialized = false;
	bool componentActive = false;
	bool processorProcessing = false;
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentConnection;
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> controllerConnection;
	bool _connectionsDone = false; // true after connect() called in OpenEditor()
	Steinberg::IPtr<Steinberg::IPlugView> plugView;
	std::unique_ptr<HostComponentHandler> componentHandler;
	std::unique_ptr<HostPlugFrame> plugFrame;

	Steinberg::Vst::ProcessData processData;
	Steinberg::Vst::AudioBusBuffers inputBus;
	Steinberg::Vst::AudioBusBuffers outputBus;
	Steinberg::int32 requestedChannels;
	Steinberg::int32 inputChannels;
	Steinberg::int32 outputChannels;
	std::vector<float*> inputChannelPtrs;
	std::vector<float*> outputChannelPtrs;
	std::vector<float> inputScratchStorage;
	std::vector<float> outputScratchStorage;
	std::unique_ptr<FixedEventList> inputEvents;
	std::unique_ptr<FixedParameterChanges> inputParameterChanges;
	Steinberg::Vst::ProcessContext processContext;
	vst::HostTimeState hostTime;
	std::vector<ParamID> hostIndexToParamId;
	std::unordered_map<std::uint32_t, unsigned int> paramIdToHostIndex;

	void ClearParameterState() noexcept
	{
		hostIndexToParamId.clear();
		paramIdToHostIndex.clear();
		if (inputParameterChanges)
			inputParameterChanges->BeginBlock();
	}

	void BuildParameterMaps() noexcept
	{
		hostIndexToParamId.clear();
		paramIdToHostIndex.clear();
		if (!controller)
			return;

		const auto count = controller->getParameterCount();
		if (count <= 0)
			return;

		hostIndexToParamId.resize(static_cast<size_t>(count), static_cast<ParamID>(0));
		for (int32 i = 0; i < count; ++i)
		{
			ParameterInfo info{};
			if (controller->getParameterInfo(i, info) != kResultOk)
				continue;

			hostIndexToParamId[static_cast<size_t>(i)] = info.id;
			paramIdToHostIndex[static_cast<std::uint32_t>(info.id)] = static_cast<unsigned int>(i);
		}
	}

	bool TryGetHostIndexForParamId(ParamID paramId, unsigned int& hostIndex) const noexcept
	{
		auto it = paramIdToHostIndex.find(static_cast<std::uint32_t>(paramId));
		if (it == paramIdToHostIndex.end())
			return false;

		hostIndex = it->second;
		return true;
	}

	bool TryGetParamIdForHostIndex(unsigned int hostIndex, ParamID& paramId) const noexcept
	{
		if (hostIndex >= hostIndexToParamId.size())
			return false;

		paramId = hostIndexToParamId[hostIndex];
		return true;
	}

	// --- Step 3: VST3 MIDI controller mapping -----------------------------
	//
	// midiMapping is queried from the controller once (after creation and
	// after every state restore). midiMappingMailbox publishes a completed
	// Vst3MidiMapping::ParameterTable to the audio thread without ever
	// freeing memory there (see Vst3MidiMapping.h). midiMappingAdoptedSlot is
	// audio-thread-only bookkeeping, touched only from BeginMidiBlock/
	// SendMidiEvent. midiMapRebuildRequested / midiMapRebuildMutex serialize
	// whichever non-RT thread(s) call RebuildMidiControllerMap() (the editor
	// idle timer via IdleEditor(), and GetState() via
	// PollPendingControllerChanges()).
	Steinberg::IPtr<Steinberg::Vst::IMidiMapping> midiMapping;
	Vst3MidiMapping::ParameterTableMailbox midiMappingMailbox;
	int midiMappingAdoptedSlot = 0;
	std::atomic<bool> midiMapRebuildRequested{ false };
	std::mutex midiMapRebuildMutex;

	// Rebuilds the fixed MIDI-controller-to-parameter table from scratch and
	// publishes it. Not RT-safe; called only from Load(), SetState(), and
	// PollPendingControllerChanges(). Returns false only when the mailbox
	// producer slot was not yet free (previous publish not yet adopted by
	// the audio thread) — the caller should leave the rebuild-requested flag
	// set so it is retried on the next poll.
	bool RebuildMidiControllerMap() noexcept
	{
		std::lock_guard<std::mutex> lock(midiMapRebuildMutex);

		if (!midiMapping || !component)
			return true; // Nothing to build; leave the table empty.

		if (component->getBusCount(kEvent, kInput) <= 0)
			return true; // No input event bus: nothing can ever be mapped.

		auto* writeSlot = midiMappingMailbox.AcquireWriteSlot();
		if (!writeSlot)
			return false;

		*writeSlot = Vst3MidiMapping::MakeEmptyTable();
		for (int16 channel = 0; channel < static_cast<int16>(Vst3MidiMapping::ChannelCount); ++channel)
		{
			for (std::uint32_t ctrl = 0; ctrl < Vst3MidiMapping::ControllerCount; ++ctrl)
			{
				ParamID paramId = 0;
				if (midiMapping->getMidiControllerAssignment(0, channel, static_cast<CtrlNumber>(ctrl), paramId) == kResultTrue)
					(*writeSlot)[static_cast<std::size_t>(channel)][ctrl] = static_cast<std::uint32_t>(paramId);
			}
		}
		midiMappingMailbox.Publish();
		return true;
	}

	// --- Step 4: editor automation forwarding ------------------------------
	//
	// controllerEditQueue carries UI-thread performEdit() values to the audio
	// thread so they reach the processor's IParameterChanges on the next
	// process block (see Vst3ControllerEditQueue.h for the threading
	// contract). PrepareProcessBlock() is the sole consumer, called from
	// ProcessBlock/ProcessBlockStereo/ProcessBlockMulti immediately before
	// processor->process().
	Vst3ControllerEditQueue controllerEditQueue;

	void PrepareProcessBlock() noexcept
	{
		Vst3ControllerEditQueue::Edit edit;
		while (controllerEditQueue.Pop(edit))
		{
			int32 queueIndex = -1;
			auto* queue = inputParameterChanges->addParameterData(static_cast<ParamID>(edit.ParamId), queueIndex);
			if (queue)
			{
				int32 pointIndex = -1;
				queue->addPoint(0, static_cast<ParamValue>(edit.Value), pointIndex);
			}
			// Fixed-capacity queue full: drop silently, consistent with the
			// existing fixed-capacity RT policy elsewhere in this file.
		}
	}

	// Small fixed set of parameters currently mid-gesture (between
	// beginEdit/endEdit). UI-thread only: the VST3 SDK guarantees
	// beginEdit/performEdit/endEdit all come from a single UI thread, so this
	// needs no synchronization. Advisory bookkeeping only — performEdit
	// (OnControllerEdit) is what actually delivers and publishes values;
	// gestures just give tests/future recording logic an explicit boundary.
	static constexpr std::size_t MaxActiveGestures = 8;
	std::array<ParamID, MaxActiveGestures> activeGestureParamIds{};
	std::size_t activeGestureCount = 0;

	void BeginGesture(ParamID paramId) noexcept
	{
		for (std::size_t i = 0; i < activeGestureCount; ++i)
		{
			if (activeGestureParamIds[i] == paramId)
				return; // Already tracked (e.g. duplicate beginEdit).
		}
		if (activeGestureCount < MaxActiveGestures)
			activeGestureParamIds[activeGestureCount++] = paramId;
	}

	void EndGesture(ParamID paramId) noexcept
	{
		for (std::size_t i = 0; i < activeGestureCount; ++i)
		{
			if (activeGestureParamIds[i] == paramId)
			{
				activeGestureParamIds[i] = activeGestureParamIds[activeGestureCount - 1];
				--activeGestureCount;
				return;
			}
		}
	}

	// Additional restartComponent() request flags (kMidiCCAssignmentChanged
	// is handled above by midiMapRebuildRequested). Consumed on the same
	// non-RT poll path (PollPendingControllerChanges).
	std::atomic<bool> paramTitlesRebuildRequested{ false };
	std::atomic<bool> paramValuesRefreshRequested{ false };

	Impl() :
		factory(nullptr),
		moduleInitialized(false),
		hostApplication(Steinberg::IPtr<Steinberg::Vst::HostApplication>(new Steinberg::Vst::HostApplication(), false)),
		component(nullptr),
		processor(nullptr),
		controller(nullptr),
		componentHandler(std::make_unique<HostComponentHandler>()),
		plugFrame(std::make_unique<HostPlugFrame>()),
		plugView(nullptr),
		_connectionsDone(false),
		processData(),
		inputBus(),
		outputBus(),
		requestedChannels(1),
		inputChannels(1),
		outputChannels(1),
		inputChannelPtrs(),
		outputChannelPtrs(),
		inputScratchStorage(),
		outputScratchStorage(),
		inputEvents(std::make_unique<FixedEventList>()),
		inputParameterChanges(std::make_unique<FixedParameterChanges>()),
		processContext(),
		hostTime(),
		hostIndexToParamId(),
		paramIdToHostIndex()
	{
	}
};
#endif

Vst3Plugin::Vst3Plugin() :
	_isLoaded(false),
	_isActivated(false),
	_name(),
	_isBypassed(false),
	_editorOpening(false),
	_editorSize({ 0, 0 }),
	_moduleHandle(nullptr),
	_impl(
	#ifdef JAMMA_VST3_ENABLED
		std::make_unique<Impl>()
	#else
		nullptr
	#endif
	)
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl && _impl->componentHandler)
		_impl->componentHandler->SetOwner(this);
#endif
}

Vst3Plugin::~Vst3Plugin()
{
	Unload();
}

bool Vst3Plugin::PreInit(const std::wstring& path)
{
#ifdef JAMMA_VST3_ENABLED
	if (_moduleHandle)
		return true; // Already pre-initialised (e.g. called twice)

	// PreInit runs on Jamma's UI thread, which also owns the OpenGL render
	// context. InitDll()/GetPluginFactory()/createInstance()/initialize() can
	// all run arbitrary plugin module code, and some VST3 plugins (like some
	// VST2 plugins) make their own GL context current during that bootstrap
	// sequence. One outer scope guard covers the whole sequence below.
	VstGlContextScope glScope;

	std::wcout << L"[Vst3Plugin] PreInit (main thread): path='" << path << L"'" << std::endl;

	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[Vst3Plugin] PreInit: LoadLibraryW failed: " << GetLastError() << std::endl;
		return false;
	}

	// Call InitDll() if exported (VST3 spec says hosts SHOULD call it).
	// This is paired with ExitDll() in Unload().
	using InitModuleFunc = bool (PLUGIN_API*)();
	auto initDll = reinterpret_cast<InitModuleFunc>(GetProcAddress(_moduleHandle, "InitDll"));
	if (initDll)
	{
		initDll();
		_impl->moduleInitialized = true;
		std::cout << "[Vst3Plugin] PreInit: InitDll() called" << std::endl;
	}

	// Call GetPluginFactory() on the UI thread so thread-affine GUI state binds
	// there before attached() runs on the same thread.
	using GetFactoryProc = IPluginFactory* (PLUGIN_API*)();
	auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(_moduleHandle, "GetPluginFactory"));
	if (!getFactory)
	{
		std::cerr << "[Vst3Plugin] PreInit: GetPluginFactory not found" << std::endl;
		if (_impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll) exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	IPluginFactory* rawFactory = getFactory();
	if (!rawFactory)
	{
		std::cerr << "[Vst3Plugin] PreInit: GetPluginFactory() returned null" << std::endl;
		if (_impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll) exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	_impl->factory = IPtr<IPluginFactory>(rawFactory, false);

	// Find the first kVstAudioEffectClass component CID.
	PFactoryInfo factoryInfo;
	_impl->factory->getFactoryInfo(&factoryInfo);
	_name = factoryInfo.vendor;

	int32 numClasses = _impl->factory->countClasses();
	FUID componentCid;
	bool found = false;

	for (int32 i = 0; i < numClasses; i++)
	{
		PClassInfo classInfo;
		_impl->factory->getClassInfo(i, &classInfo);

		if (std::string(classInfo.category) == kVstAudioEffectClass)
		{
			componentCid = FUID::fromTUID(classInfo.cid);
			_name = classInfo.name;
			found = true;
			break;
		}
	}

	if (!found)
	{
		std::cerr << "[Vst3Plugin] PreInit: no audio effect class found in plugin" << std::endl;
		_impl->factory = nullptr;
		if (_impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll) exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	// Create and initialize on the UI thread so later editor attach work does
	// not depend on a job-thread message pump.
	IComponent* rawComponent = nullptr;
	if (_impl->factory->createInstance(componentCid, IComponent::iid, (void**)&rawComponent) != kResultOk
		|| !rawComponent)
	{
		std::cerr << "[Vst3Plugin] PreInit: createInstance(IComponent) failed" << std::endl;
		_impl->factory = nullptr;
		if (_impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll) exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}
	_impl->component = IPtr<IComponent>(rawComponent, false);

	if (_impl->component->initialize(_impl->hostApplication) != kResultOk)
	{
		std::cerr << "[Vst3Plugin] PreInit: IComponent::initialize() failed" << std::endl;
		_impl->component = nullptr;
		_impl->factory = nullptr;
		if (_impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll) exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}
	_impl->componentInitialized = true;

	std::cout << "[Vst3Plugin] PreInit: success — factory, component, and initialize on main thread, plugin=" << _name << std::endl;
	return true;
#else
	(void)path;
	return false;
#endif
}

bool Vst3Plugin::Load(const std::wstring& path,
	float sampleRate,
	unsigned int blockSize,
	unsigned int numChannels,
	HostedLayoutMode layoutMode)
{
#ifdef JAMMA_VST3_ENABLED
	// Only do a full unload if we're replacing a previously loaded plugin.
	// If PreInit() ran, _moduleHandle is set but _isLoaded is false — we must
	// NOT unload here or we'd throw away the main-thread pre-initialisation.
	if (_isLoaded)
		Unload();

	std::wcout << L"[Vst3Plugin] Load request: path='" << path
		<< L"', sampleRate=" << sampleRate
		<< L", blockSize=" << blockSize
		<< L", requestedChannels=" << numChannels
		<< std::endl;

	const auto requestedChannels = static_cast<Steinberg::int32>(std::max(1u, numChannels));
	std::cout << "[Vst3Plugin] Requested host channels=" << requestedChannels << std::endl;
	_impl->requestedChannels = requestedChannels;

	// 1. Load the DLL — skip if PreInit() already loaded it on the main thread
	if (!_moduleHandle)
	{
		_moduleHandle = LoadLibraryW(path.c_str());
		if (!_moduleHandle)
		{
			std::cerr << "[Vst3Plugin] LoadLibraryW failed: " << GetLastError() << std::endl;
			return false;
		}

		// Call InitDll() if exported and we didn't already do so in PreInit().
		using InitModuleFunc = bool (PLUGIN_API*)();
		auto initDll = reinterpret_cast<InitModuleFunc>(GetProcAddress(_moduleHandle, "InitDll"));
		if (initDll)
		{
			initDll();
			_impl->moduleInitialized = true;
		}
	}
	else
	{
		std::cout << "[Vst3Plugin] Load: reusing pre-loaded DLL handle from PreInit()" << std::endl;
	}

	// 2. Get the factory — use cached value from PreInit() when available
	IPtr<IPluginFactory> factory = _impl->factory;
	if (!factory)
	{
		typedef IPluginFactory* (PLUGIN_API* GetFactoryProc)();
		auto getFactory = reinterpret_cast<GetFactoryProc>(
			GetProcAddress(_moduleHandle, "GetPluginFactory"));
		if (!getFactory)
		{
			std::cerr << "[Vst3Plugin] GetPluginFactory not found" << std::endl;
			Unload();
			return false;
		}

		IPluginFactory* rawFactory = getFactory();
		if (!rawFactory)
		{
			std::cerr << "[Vst3Plugin] GetPluginFactory() returned null" << std::endl;
			Unload();
			return false;
		}

		factory = IPtr<IPluginFactory>(rawFactory, false);
		_impl->factory = factory; // cache for later
	}
	else
	{
		std::cout << "[Vst3Plugin] Load: reusing pre-loaded factory from PreInit()" << std::endl;
	}

	const bool componentWasPreInitialized = _impl->component != nullptr;
	auto cleanupFailedLoad = [&]() {
		ResetLoadedObjects(!componentWasPreInitialized);
		_isLoaded = false;
		_editorSize = { 0, 0 };
		return false;
	};

	// 3. Find the first IAudioProcessor class (skip if PreInit() already found
	//    the component and name)
	if (!_impl->component)
	{
		PFactoryInfo factoryInfo;
		factory->getFactoryInfo(&factoryInfo);
		_name = factoryInfo.vendor;

		int32 numClasses = factory->countClasses();
		FUID componentCid;
		bool found = false;

		for (int32 i = 0; i < numClasses; i++)
		{
			PClassInfo classInfo;
			factory->getClassInfo(i, &classInfo);

			if (std::string(classInfo.category) == kVstAudioEffectClass)
			{
				componentCid = FUID::fromTUID(classInfo.cid);
				_name = classInfo.name;
				found = true;
				break;
			}
		}

		if (!found)
		{
			std::cerr << "[Vst3Plugin] No audio effect class found in plugin" << std::endl;
			return cleanupFailedLoad();
		}

		// 4. Create IComponent only when PreInit() did not already do the UI-thread setup.
		// Doing this here can bind editor-thread state to the job thread and hang attached().
		// This path can run on the job thread when PreInit() was skipped, so
		// guard it too: a job thread normally has no current GL context, so
		// the scope is a harmless no-op there, but it still protects the
		// case where a caller invokes Load() directly from the UI thread.
		VstGlContextScope componentGlScope;
		IComponent* rawComponent = nullptr;
		if (factory->createInstance(componentCid, IComponent::iid, (void**)&rawComponent) != kResultOk
			|| !rawComponent)
		{
			std::cerr << "[Vst3Plugin] createInstance(IComponent) failed" << std::endl;
			return cleanupFailedLoad();
		}
		_impl->component = IPtr<IComponent>(rawComponent, false);

		// 5. Initialize the component
		if (_impl->component->initialize(_impl->hostApplication) != kResultOk)
		{
			std::cerr << "[Vst3Plugin] IComponent::initialize() failed" << std::endl;
			return cleanupFailedLoad();
		}
		_impl->componentInitialized = true;
	}
	else
	{
		std::cout << "[Vst3Plugin] Load: reusing pre-initialized component from PreInit()" << std::endl;
	}

	auto inputBusCount = _impl->component->getBusCount(kAudio, kInput);
	auto outputBusCount = _impl->component->getBusCount(kAudio, kOutput);
	std::cout << "[Vst3Plugin] Audio buses: inputs=" << inputBusCount
		<< ", outputs=" << outputBusCount << std::endl;
	int32_t inputBusChannels = 0;
	int32_t outputBusChannels = 0;

	if (inputBusCount > 0)
	{
		BusInfo inBus{};
		if (_impl->component->getBusInfo(kAudio, kInput, 0, inBus) == kResultOk)
		{
			inputBusChannels = inBus.channelCount;
			std::cout << "[Vst3Plugin] Input bus 0 channels=" << inBus.channelCount << std::endl;
		}
	}

	if (outputBusCount > 0)
	{
		BusInfo outBus{};
		if (_impl->component->getBusInfo(kAudio, kOutput, 0, outBus) == kResultOk)
		{
			outputBusChannels = outBus.channelCount;
			std::cout << "[Vst3Plugin] Output bus 0 channels=" << outBus.channelCount << std::endl;
		}
	}

	auto hostedLayout = ResolveHostedBusLayout(layoutMode, numChannels, inputBusChannels, outputBusChannels);
	_impl->inputChannels = hostedLayout.InputChannels;
	_impl->outputChannels = hostedLayout.OutputChannels;
	std::cout << "[Vst3Plugin] Negotiated layout: requested=" << hostedLayout.RequestedChannels
		<< ", input=" << _impl->inputChannels
		<< ", output=" << _impl->outputChannels << std::endl;

	// 6. Query IAudioProcessor
	IAudioProcessor* rawProcessor = nullptr;
	if (_impl->component->queryInterface(IAudioProcessor::iid, (void**)&rawProcessor) != kResultOk
		|| !rawProcessor)
	{
		std::cerr << "[Vst3Plugin] queryInterface(IAudioProcessor) failed" << std::endl;
		return cleanupFailedLoad();
	}
	_impl->processor = IPtr<IAudioProcessor>(rawProcessor, false);

	if (inputBusCount > 0 || outputBusCount > 0)
	{
		SpeakerArrangement inputArrangement = 0;
		SpeakerArrangement outputArrangement = 0;
		const auto hasInputArrangement = (inputBusCount <= 0)
			|| TryGetSpeakerArrangementForChannelCount(_impl->inputChannels, inputArrangement);
		const auto hasOutputArrangement = (outputBusCount <= 0)
			|| TryGetSpeakerArrangementForChannelCount(_impl->outputChannels, outputArrangement);

		if (hasInputArrangement && hasOutputArrangement)
		{
			auto busArrangementRes = _impl->processor->setBusArrangements(
				(inputBusCount > 0) ? &inputArrangement : nullptr,
				(inputBusCount > 0) ? 1 : 0,
				(outputBusCount > 0) ? &outputArrangement : nullptr,
				(outputBusCount > 0) ? 1 : 0);
			std::cout << "[Vst3Plugin] setBusArrangements=" << busArrangementRes << std::endl;
		}
		else
		{
			std::cout << "[Vst3Plugin] Skipping setBusArrangements: unsupported requested channel count"
				<< " input=" << _impl->inputChannels
				<< " output=" << _impl->outputChannels << std::endl;
		}

		if (inputBusCount > 0)
		{
			SpeakerArrangement actualInputArrangement = SpeakerArr::kMono;
			if (_impl->processor->getBusArrangement(kInput, 0, actualInputArrangement) == kResultOk)
				_impl->inputChannels = (std::max)(int32{ 1 }, SpeakerArr::getChannelCount(actualInputArrangement));
		}

		if (outputBusCount > 0)
		{
			SpeakerArrangement actualOutputArrangement = SpeakerArr::kMono;
			if (_impl->processor->getBusArrangement(kOutput, 0, actualOutputArrangement) == kResultOk)
				_impl->outputChannels = (std::max)(int32{ 1 }, SpeakerArr::getChannelCount(actualOutputArrangement));
		}

		std::cout << "[Vst3Plugin] Active layout after setBusArrangements: input=" << _impl->inputChannels
			<< ", output=" << _impl->outputChannels << std::endl;
	}

	if (!IsHostedLayoutCompatible(hostedLayout, _impl->inputChannels, _impl->outputChannels))
	{
		std::cerr << "[Vst3Plugin] Incompatible negotiated layout: requested=" << hostedLayout.RequestedChannels
			<< ", input=" << _impl->inputChannels
			<< ", output=" << _impl->outputChannels << std::endl;
		return cleanupFailedLoad();
	}

	// 7. Setup processing
	ProcessSetup setup;
	setup.processMode = kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = static_cast<int32>(blockSize);
	setup.sampleRate = static_cast<SampleRate>(sampleRate);

	if (_impl->processor->setupProcessing(setup) != kResultOk)
	{
		std::cerr << "[Vst3Plugin] setupProcessing() failed" << std::endl;
		return cleanupFailedLoad();
	}

	// 8. Activate buses, then start processing on the job thread.
	// This is safe because PreInit() already performed the UI-thread setup.
	auto inActivateRes = (inputBusCount > 0) ? _impl->component->activateBus(kAudio, kInput, 0, true) : kResultOk;
	auto outActivateRes = (outputBusCount > 0) ? _impl->component->activateBus(kAudio, kOutput, 0, true) : kResultOk;

	auto activeRes = _impl->component->setActive(true);
	if (activeRes == kResultOk)
		_impl->componentActive = true;

	auto processingRes = static_cast<tresult>(kResultFalse);
	if (_impl->componentActive)
	{
		processingRes = _impl->processor->setProcessing(true);
		if (processingRes == kResultOk)
			_impl->processorProcessing = true;
	}

	std::cout << "[Vst3Plugin] activateBus/setActive/setProcessing: input=" << inActivateRes
		<< ", output=" << outActivateRes
		<< ", setActive=" << activeRes
		<< ", setProcessing=" << processingRes << std::endl;

	if (_impl->componentActive && _impl->processorProcessing)
	{
		_isActivated.store(true, std::memory_order_release);
	}
	else
	{
		std::cerr << "[Vst3Plugin] Activation failed (setActive=" << activeRes
			<< ", setProcessing=" << processingRes << "); tearing down" << std::endl;

		// Reverse whichever transition succeeded, in the mandated teardown
		// order: setProcessing(false) precedes setActive(false).
		if (_impl->processorProcessing)
		{
			_impl->processor->setProcessing(false);
			_impl->processorProcessing = false;
		}
		if (_impl->componentActive)
		{
			_impl->component->setActive(false);
			_impl->componentActive = false;
		}
		if (inputBusCount > 0 && inActivateRes == kResultOk)
			_impl->component->activateBus(kAudio, kInput, 0, false);
		if (outputBusCount > 0 && outActivateRes == kResultOk)
			_impl->component->activateBus(kAudio, kOutput, 0, false);

		return cleanupFailedLoad();
	}

	// 9. Pre-allocate ProcessData so ProcessBlock() is heap-allocation-free
	_impl->inputScratchStorage.assign(static_cast<size_t>(_impl->inputChannels) * constants::MaxBlockSize, 0.0f);
	_impl->outputScratchStorage.assign(static_cast<size_t>(_impl->outputChannels) * constants::MaxBlockSize, 0.0f);
	_impl->inputChannelPtrs.resize(_impl->inputChannels);
	_impl->outputChannelPtrs.resize(_impl->outputChannels);
	for (Steinberg::int32 c = 0; c < _impl->inputChannels; c++)
		_impl->inputChannelPtrs[c] = _impl->inputScratchStorage.data() + (static_cast<size_t>(c) * constants::MaxBlockSize);
	for (Steinberg::int32 c = 0; c < _impl->outputChannels; c++)
		_impl->outputChannelPtrs[c] = _impl->outputScratchStorage.data() + (static_cast<size_t>(c) * constants::MaxBlockSize);

	_impl->inputBus.numChannels = _impl->inputChannels;
	_impl->inputBus.channelBuffers32 = _impl->inputChannelPtrs.data();
	_impl->inputBus.silenceFlags = 0;

	_impl->outputBus.numChannels = _impl->outputChannels;
	_impl->outputBus.channelBuffers32 = _impl->outputChannelPtrs.data();
	_impl->outputBus.silenceFlags = 0;

	_impl->processData.processMode = kRealtime;
	_impl->processData.symbolicSampleSize = kSample32;
	_impl->processData.numSamples = static_cast<int32>(blockSize);
	_impl->processData.numInputs = (inputBusCount > 0) ? 1 : 0;
	_impl->processData.numOutputs = (outputBusCount > 0) ? 1 : 0;
	_impl->processData.inputs = (inputBusCount > 0) ? &_impl->inputBus : nullptr;
	_impl->processData.outputs = (outputBusCount > 0) ? &_impl->outputBus : nullptr;
	_impl->processData.inputEvents = _impl->inputEvents.get();
	_impl->processData.outputEvents = nullptr;
	_impl->processData.inputParameterChanges = _impl->inputParameterChanges.get();
	_impl->processData.outputParameterChanges = nullptr;
	_impl->processData.processContext = &_impl->processContext;
	_impl->inputParameterChanges->BeginBlock();

	// 10. Optionally retrieve IEditController (may be the same object or separate)
	IEditController* rawController = nullptr;
	if (_impl->component->queryInterface(IEditController::iid, (void**)&rawController) == kResultOk
		&& rawController)
	{
		_impl->controller = IPtr<IEditController>(rawController, false);
		// Same object as component: already initialized transitively by
		// component->initialize() above, no separate initialize()/terminate().
		_impl->controllerInitialized = true;
		std::cout << "[Vst3Plugin] Controller queryInterface: ok" << std::endl;
	}
	else
	{
		// Try separate controller via IComponent2 CID
		// (This path is only needed for old-style VST3 plugins that separate
		// the component and the controller into distinct CIDs.)
		// For now we just leave _controller null — OpenEditor will be a no-op.
		TUID controllerCid;
		if (_impl->component->getControllerClassId(controllerCid) == kResultOk)
		{
			VstGlContextScope controllerGlScope;
			if (factory->createInstance(controllerCid, IEditController::iid, (void**)&rawController) == kResultOk
				&& rawController)
			{
				_impl->controller = IPtr<IEditController>(rawController, false);
				auto initRes = _impl->controller->initialize(_impl->hostApplication);
				std::cout << "[Vst3Plugin] Separate controller created, initialize=" << initRes << std::endl;

				if (initRes == kResultOk)
				{
					_impl->controllerIsSeparateObject = true;
					_impl->controllerInitialized = true;
				}
				else
				{
					// Failed initialize: tear down and continue processor-only
					// rather than leaving a half-initialized controller object
					// that would later receive a spurious terminate() (or none
					// at all — either is a lifecycle bug).
					std::cerr << "[Vst3Plugin] Separate controller initialize() failed; continuing processor-only" << std::endl;
					_impl->controller->terminate();
					_impl->controller = nullptr;
				}
			}
		}
	}

	if (!_impl->controller)
		std::cout << "[Vst3Plugin] No controller available (editor may not open)" << std::endl;
	else
	{
		_impl->BuildParameterMaps();
		_impl->controller->setComponentHandler(_impl->componentHandler.get());

		IMidiMapping* rawMidiMapping = nullptr;
		if (_impl->controller->queryInterface(IMidiMapping::iid, (void**)&rawMidiMapping) == kResultOk
			&& rawMidiMapping)
		{
			_impl->midiMapping = IPtr<IMidiMapping>(rawMidiMapping, false);
			_impl->RebuildMidiControllerMap();
			std::cout << "[Vst3Plugin] IMidiMapping available; controller mapping table built" << std::endl;
		}
	}

	// Defer connect() to OpenEditor() on the main thread.
	// Calling it here can route editor work through the job thread and hang attached().
	if (_impl->controller)
	{
		IConnectionPoint* rawComponentConn = nullptr;
		IConnectionPoint* rawControllerConn = nullptr;
		if (_impl->component->queryInterface(IConnectionPoint::iid, (void**)&rawComponentConn) == kResultOk && rawComponentConn)
			_impl->componentConnection = IPtr<IConnectionPoint>(rawComponentConn, false);

		if (_impl->controller->queryInterface(IConnectionPoint::iid, (void**)&rawControllerConn) == kResultOk && rawControllerConn)
			_impl->controllerConnection = IPtr<IConnectionPoint>(rawControllerConn, false);

		if (_impl->componentConnection && _impl->controllerConnection)
			std::cout << "[Vst3Plugin] ConnectionPoint available, will connect in OpenEditor()" << std::endl;
		else
			std::cout << "[Vst3Plugin] ConnectionPoint not available on component/controller" << std::endl;
	}

	_isLoaded = true;
	std::cout << "[Vst3Plugin] Loaded: " << _name << std::endl;
	return true;

#else
	(void)path; (void)sampleRate; (void)blockSize; (void)numChannels;
	std::cerr << "[Vst3Plugin] VST3 support not compiled in (define JAMMA_VST3_ENABLED)" << std::endl;
	return false;
#endif
}

void Vst3Plugin::Unload()
{
#ifdef JAMMA_VST3_ENABLED
	CloseEditor();

	if (_impl)
	{
		ResetLoadedObjects(true);

		// Release factory before FreeLibrary: factory COM vtable lives inside
		// the DLL; releasing after FreeLibrary would call into unloaded code.
		_impl->factory = nullptr;
	}

	if (_moduleHandle)
	{
		// Pair the InitDll() call made in PreInit() or Load() with ExitDll().
		if (_impl && _impl->moduleInitialized)
		{
			using ExitModuleFunc = bool (PLUGIN_API*)();
			auto exitDll = reinterpret_cast<ExitModuleFunc>(GetProcAddress(_moduleHandle, "ExitDll"));
			if (exitDll)
				exitDll();
			_impl->moduleInitialized = false;
		}
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
	}
#endif

	_isLoaded = false;
	_name.clear();
	_editorSize = { 0, 0 };
}

void Vst3Plugin::ResetLoadedObjects(bool terminateComponent)
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	if (_isActivated.exchange(false, std::memory_order_acq_rel))
	{
		if (_impl->processor && _impl->processorProcessing)
			_impl->processor->setProcessing(false);
		if (_impl->component && _impl->componentActive)
			_impl->component->setActive(false);
	}
	_impl->processorProcessing = false;
	_impl->componentActive = false;

	if (_impl->componentConnection && _impl->controllerConnection)
	{
		_impl->componentConnection->disconnect(_impl->controllerConnection);
		_impl->controllerConnection->disconnect(_impl->componentConnection);
	}
	_impl->_connectionsDone = false;

	if (_impl->controller)
		_impl->controller->setComponentHandler(nullptr);

	_impl->ClearParameterState();

	_impl->processor = nullptr;

	// A separately-initialized controller (the IComponent2/legacy fallback
	// path in Load()) owns its own lifecycle and must receive terminate()
	// exactly once. A controller obtained via component->queryInterface() is
	// the same underlying object as the component and must NOT be terminated
	// again here — component->terminate() below already covers it. Guard on
	// controllerInitialized too so a controller whose initialize() failed
	// (already terminated/released at the failure site in Load()) can never
	// be terminated a second time here.
	if (_impl->controller && _impl->controllerIsSeparateObject && _impl->controllerInitialized)
		_impl->controller->terminate();
	_impl->controller = nullptr;
	_impl->controllerIsSeparateObject = false;
	_impl->controllerInitialized = false;

	_impl->componentConnection = nullptr;
	_impl->controllerConnection = nullptr;

	// Defensive: OpenEditor/CloseEditor already require setFrame(nullptr) and
	// removed() before releasing plugView, and Unload() always runs
	// CloseEditor() before ResetLoadedObjects(). This should already be null,
	// but guard the invariant in case ResetLoadedObjects is ever reached with
	// an editor still attached (e.g. a future failed-load path).
	if (_impl->plugView)
	{
		_impl->plugView->setFrame(nullptr);
		_impl->plugView->removed();
		_impl->plugView = nullptr;
	}

	_impl->midiMapping = nullptr;
	_impl->midiMapRebuildRequested.store(false, std::memory_order_relaxed);
	_impl->midiMappingMailbox.Reset();
	_impl->midiMappingAdoptedSlot = 0;

	_impl->controllerEditQueue.Clear();
	_impl->activeGestureCount = 0;
	_impl->paramTitlesRebuildRequested.store(false, std::memory_order_relaxed);
	_impl->paramValuesRefreshRequested.store(false, std::memory_order_relaxed);

	if (terminateComponent && _impl->component)
	{
		if (_impl->componentInitialized)
			_impl->component->terminate();
		_impl->component = nullptr;
		_impl->componentInitialized = false;
	}

	// Null out bus buffer pointers before clearing the backing storage so that
	// a concurrent ProcessBlock (guarded by _isActivated above) cannot chase
	// stale channelBuffers32 pointers into freed memory.
	_impl->inputBus.channelBuffers32 = nullptr;
	_impl->outputBus.channelBuffers32 = nullptr;
	_impl->inputChannelPtrs.clear();
	_impl->outputChannelPtrs.clear();
	_impl->inputScratchStorage.clear();
	_impl->outputScratchStorage.clear();
#else
	(void)terminateComponent;
#endif
}

void Vst3Plugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	if (!_isLoaded || !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| _editorOpening.load(std::memory_order_acquire))
		return;

	if (numSamples <= 0 || static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_impl->inputChannelPtrs.empty() || _impl->outputChannelPtrs.empty())
		return;

	CopyMonoToInputBuffers(monoBuf, numSamples, _impl->inputChannels, _impl->inputChannelPtrs.data());

	// Update sample count (other ProcessData fields are fixed from Load())
	_impl->processData.numSamples = numSamples;

	_impl->PrepareProcessBlock();
	_impl->processor->process(_impl->processData);
	_impl->inputParameterChanges->BeginBlock();

	FoldOutputToMono(_impl->outputChannelPtrs.data(), _impl->outputChannels, numSamples, monoBuf);
#else
	(void)monoBuf; (void)numSamples;
#endif
}

void Vst3Plugin::ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	if (!_isLoaded || !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| _editorOpening.load(std::memory_order_acquire))
		return;

	if (numSamples <= 0 || static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_impl->inputChannelPtrs.empty() || _impl->outputChannelPtrs.empty())
		return;

	if ((_impl->inputChannels != 2) || (_impl->outputChannels != 2))
	{
		// Plugin is not natively stereo.  Process once using the left channel
		// (downmix-to-mono path) and copy the result to the right channel.
		// Calling ProcessBlock twice would let the second pass observe state
		// advanced by the first, causing unintended cross-channel interaction
		// in stateful effects such as delay or reverb.
		ProcessBlock(leftBuf, numSamples);
		std::copy(leftBuf, leftBuf + static_cast<std::ptrdiff_t>(numSamples), rightBuf);
		return;
	}

	float* inputChannels[] = { leftBuf, rightBuf };
	CopyMultiToInputBuffers(inputChannels, 2, numSamples, _impl->inputChannelPtrs.data(), _impl->inputChannels);

	_impl->processData.numSamples = numSamples;
	_impl->PrepareProcessBlock();
	_impl->processor->process(_impl->processData);
	_impl->inputParameterChanges->BeginBlock();

	float* outputChannels[] = { leftBuf, rightBuf };
	CopyOutputToMulti(_impl->outputChannelPtrs.data(), _impl->outputChannels, numSamples, outputChannels, 2);
#else
	(void)leftBuf; (void)rightBuf; (void)numSamples;
#endif
}

void Vst3Plugin::ProcessBlockMulti(float* const* channelBufs, int32_t numChannels, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl || !channelBufs)
		return;

	if (!_isLoaded || !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| _editorOpening.load(std::memory_order_acquire))
		return;

	if (_impl->inputChannelPtrs.empty() || _impl->outputChannelPtrs.empty())
		return;

	// Accept numChannels >= requestedChannels: a narrow plugin (e.g. stereo)
	// inserted on a wide bus (e.g. 8-ch Station) should process only its
	// channels and leave the rest of the destination untouched.
	// CopyMultiToInputBuffers reads only up to inputChannels from the source,
	// and CopyOutputToMulti writes only min(outputChannels, numChannels) back,
	// so channels beyond the plugin's width pass through unchanged.
	if ((numChannels <= 0) || (numChannels < _impl->requestedChannels)
		|| (numSamples <= 0) || (static_cast<unsigned int>(numSamples) > constants::MaxBlockSize))
		return;

	CopyMultiToInputBuffers(channelBufs, numChannels, numSamples, _impl->inputChannelPtrs.data(), _impl->inputChannels);
	_impl->processData.numSamples = numSamples;
	_impl->PrepareProcessBlock();
	_impl->processor->process(_impl->processData);
	_impl->inputParameterChanges->BeginBlock();
	CopyOutputToMulti(_impl->outputChannelPtrs.data(), _impl->outputChannels, numSamples, channelBufs, numChannels);
#else
	(void)channelBufs; (void)numChannels; (void)numSamples;
#endif
}

void Vst3Plugin::SetParameter(unsigned int index, float value) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl || !_isLoaded || !_impl->inputParameterChanges)
		return;

	ParamID paramId = 0;
	if (!_impl->TryGetParamIdForHostIndex(index, paramId))
		return;

	int32 queueIndex = -1;
	auto* queue = _impl->inputParameterChanges->addParameterData(paramId, queueIndex);
	if (!queue)
		return;

	const auto normalized = std::clamp<ParamValue>(static_cast<ParamValue>(value), 0.0, 1.0);
	int32 pointIndex = -1;
	queue->addPoint(0, normalized, pointIndex);
#else
	(void)index; (void)value;
#endif
}

float Vst3Plugin::GetParameter(unsigned int index) const noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl || !_impl->controller)
		return 0.0f;

	ParamID paramId = 0;
	if (!_impl->TryGetParamIdForHostIndex(index, paramId))
		return 0.0f;

	const auto normalized = _impl->controller->getParamNormalized(paramId);
	return std::clamp(static_cast<float>(normalized), 0.0f, 1.0f);
#else
	(void)index;
	return 0.0f;
#endif
}

void Vst3Plugin::UpdateHostTime(const HostTimeState& state) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	_impl->hostTime = state;
	_impl->processContext.projectTimeSamples = state.samplePos;
	_impl->processContext.tempo = state.tempo;
	_impl->processContext.timeSigNumerator = state.bpi;
	_impl->processContext.timeSigDenominator = 4;
	_impl->processContext.state = state.isPlaying ? 1u : 0u;
	_impl->processContext.sampleRate = state.sampleRate;
#else
	(void)state;
#endif
}

void Vst3Plugin::OnControllerEdit(std::uint32_t paramId, float normalizedValue) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	const auto clamped = std::clamp(normalizedValue, 0.0f, 1.0f);

	// Always enqueue for the processor, even if this ParamID is unknown to
	// Jamma's host-index map (e.g. a plugin-only meta parameter) — the
	// processor still needs the value on its next process() call.
	_impl->controllerEditQueue.Push(paramId, static_cast<double>(clamped));

	unsigned int hostIndex = 0u;
	if (!_impl->TryGetHostIndexForParamId(static_cast<ParamID>(paramId), hostIndex))
		return; // Unknown to Jamma: enqueued above, nothing to publish.

	PublishLastTouchedParameter(this, hostIndex, clamped);
#else
	(void)paramId;
	(void)normalizedValue;
#endif
}

void Vst3Plugin::OnBeginEdit(std::uint32_t paramId) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl)
		_impl->BeginGesture(static_cast<ParamID>(paramId));
#else
	(void)paramId;
#endif
}

void Vst3Plugin::OnEndEdit(std::uint32_t paramId) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl)
		_impl->EndGesture(static_cast<ParamID>(paramId));
#else
	(void)paramId;
#endif
}

void Vst3Plugin::OnRestartComponent(std::int32_t flags) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	// Only record what changed; the actual non-RT rebuild happens later on
	// IdleEditor() / PollPendingControllerChanges(). This callback may run on
	// whichever thread the plugin chooses to call it from.
	if (flags & kMidiCCAssignmentChanged)
		_impl->midiMapRebuildRequested.store(true, std::memory_order_release);
	if (flags & kParamTitlesChanged)
		_impl->paramTitlesRebuildRequested.store(true, std::memory_order_release);
	if (flags & kParamValuesChanged)
		_impl->paramValuesRefreshRequested.store(true, std::memory_order_release);
#else
	(void)flags;
#endif
}

void Vst3Plugin::BeginMidiBlock(std::uint32_t blockStartSample,
	std::uint32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	if (_impl->inputEvents)
		_impl->inputEvents->BeginBlock(blockStartSample, numSamples);

	// Real-time safe: bounded atomic loads/stores only, no allocation.
	_impl->midiMappingMailbox.AdoptPending(_impl->midiMappingAdoptedSlot);
#else
	(void)blockStartSample; (void)numSamples;
#endif
}

void Vst3Plugin::SendMidiEvent(const midi::MidiEvent& event,
	bool isRealtime) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	// Try mapped delivery (CC / channel pressure / pitch bend with an
	// IMidiMapping assignment) as an automation parameter point first. Reuses
	// the exact same block-window classification as the legacy event path
	// (FixedEventList::ComputeBlockPlacement) so the two paths cannot drift.
	Vst3MidiMapping::ControllerValue controllerValue;
	if (_impl->inputParameterChanges && _impl->inputEvents
		&& Vst3MidiMapping::TryClassify(event, controllerValue))
	{
		std::uint32_t mappedParamId = Vst3MidiMapping::NoParamId;
		const auto& table = _impl->midiMappingMailbox.Slot(_impl->midiMappingAdoptedSlot);
		if (Vst3MidiMapping::TryLookup(table, event.Channel(), controllerValue.ControllerNumber, mappedParamId))
		{
			const auto placement = FixedEventList::ComputeBlockPlacement(event,
				_impl->inputEvents->BlockStartSample(),
				_impl->inputEvents->BlockNumSamples(),
				isRealtime);
			if (placement.Accepted)
			{
				int32 queueIndex = -1;
				auto* queue = _impl->inputParameterChanges->addParameterData(static_cast<ParamID>(mappedParamId), queueIndex);
				if (queue)
				{
					int32 pointIndex = -1;
					queue->addPoint(placement.SampleOffset, static_cast<ParamValue>(controllerValue.NormalizedValue), pointIndex);
				}
				// If the fixed parameter queue is full, addParameterData()
				// returns null and the point is silently dropped — consistent
				// with the existing fixed-capacity RT policy elsewhere in this
				// file. Either way, a mapping exists, so never also emit a
				// duplicate legacy CC/pressure/bend event below.
			}
			return;
		}
	}

	// No mapping (or not a CC/pressure/bend message): preserve the existing
	// legacy MIDI event representation.
	if (_impl->inputEvents)
		_impl->inputEvents->AddMidiEvent(event, isRealtime);
#else
	(void)event; (void)isRealtime;
#endif
}

bool Vst3Plugin::OpenEditor(HWND parentHwnd)
{
#ifdef JAMMA_VST3_ENABLED
	// Pause real-time audio processing while opening the editor. Some VST3
	// plug-ins (e.g. Valhalla VSTGUI-based plug-ins) deadlock inside
	// IPlugView::attached() if process() runs concurrently because their
	// internal initialization grabs locks shared with the audio path.
	_editorOpening.store(true, std::memory_order_release);
	struct ResetOnExit {
		std::atomic<bool>& flag;
		~ResetOnExit() { flag.store(false, std::memory_order_release); }
	} resetOnExit{ _editorOpening };

	if (!_isLoaded || !_impl || !_impl->controller)
	{
		std::cout << "[Vst3Plugin] OpenEditor failed: loaded=" << _isLoaded
			<< ", hasImpl=" << (_impl ? 1 : 0)
			<< ", hasController=" << ((_impl && _impl->controller) ? 1 : 0)
			<< std::endl;
		return false;
	}

	// Connect on the main thread before createView().
	// Some plugins require this to expose the editor, and doing it on the job
	// thread can hang attached().
	if (!_impl->_connectionsDone && _impl->componentConnection && _impl->controllerConnection)
	{
		auto c2k = _impl->componentConnection->connect(_impl->controllerConnection);
		auto k2c = _impl->controllerConnection->connect(_impl->componentConnection);
		_impl->_connectionsDone = true;
		std::cout << "[Vst3Plugin] ConnectionPoint connect: c2k=" << c2k
			<< ", k2c=" << k2c << std::endl;
	}

	IPlugView* rawView = nullptr;
	{
		// createView() and attached() may create or switch a GL context
		// (VSTGUI-based VST3 plugins commonly do). Restore ours afterwards.
		VstGlContextScope glScope;
		rawView = _impl->controller->createView(Steinberg::Vst::ViewType::kEditor);
		if (!rawView)
		{
			std::cout << "[Vst3Plugin] OpenEditor failed: createView returned null" << std::endl;
			return false;
		}

		_impl->plugView = IPtr<IPlugView>(rawView, false);

		if (_impl->plugView->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk)
		{
			std::cout << "[Vst3Plugin] OpenEditor failed: kPlatformTypeHWND not supported" << std::endl;
			_impl->plugView = nullptr;
			return false;
		}

		_impl->plugFrame->SetHostWindow(parentHwnd);
		_impl->plugView->setFrame(_impl->plugFrame.get());

		const auto attachedResult = _impl->plugView->attached(reinterpret_cast<void*>(parentHwnd), kPlatformTypeHWND);
		if (attachedResult != kResultOk)
		{
			std::cout << "[Vst3Plugin] OpenEditor failed: attached(HWND) failed" << std::endl;
			// Do not leave the frame attached to a view we are about to drop.
			_impl->plugView->setFrame(nullptr);
			_impl->plugView = nullptr;
			return false;
		}
	}

	// Query preferred size
	ViewRect rect{};
	if (_impl->plugView->getSize(&rect) == kResultOk)
	{
		_editorSize = {
			static_cast<unsigned int>(rect.getWidth()),
			static_cast<unsigned int>(rect.getHeight())
		};
	}

	std::cout << "[Vst3Plugin] OpenEditor success: size=" << _editorSize.Width << "x" << _editorSize.Height << std::endl;

	return true;
#else
	(void)parentHwnd;
	return false;
#endif
}

void Vst3Plugin::CloseEditor()
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl && _impl->plugView)
	{
		// removed() may switch the current GL context; restore ours after.
		VstGlContextScope glScope;
		_impl->plugView->setFrame(nullptr);
		_impl->plugView->removed();
		_impl->plugView = nullptr;
	}
	if (_impl && _impl->plugFrame)
		_impl->plugFrame->SetHostWindow(nullptr);
	_editorSize = { 0, 0 };
#endif
}

utils::Size2d Vst3Plugin::GetEditorSize() const noexcept
{
	return _editorSize;
}

void Vst3Plugin::IdleEditor() noexcept
{
#ifdef JAMMA_VST3_ENABLED
	PollPendingControllerChanges();
#endif
}

void Vst3Plugin::PollPendingControllerChanges() const noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_impl)
		return;

	if (_impl->midiMapRebuildRequested.exchange(false, std::memory_order_acq_rel))
	{
		// RebuildMidiControllerMap() returns false only when the mailbox's
		// previous publish has not yet been adopted by the audio thread;
		// leave the flag set so the next poll retries.
		if (!_impl->RebuildMidiControllerMap())
			_impl->midiMapRebuildRequested.store(true, std::memory_order_release);
	}

	if (_impl->paramTitlesRebuildRequested.exchange(false, std::memory_order_acq_rel))
	{
		// Parameter list/titles changed (e.g. after a program change):
		// rebuild the host-index <-> ParamID maps used by SetParameter/
		// GetParameter/OnControllerEdit.
		_impl->BuildParameterMaps();
	}

	if (_impl->paramValuesRefreshRequested.exchange(false, std::memory_order_acq_rel))
	{
		// kParamValuesChanged: the plugin changed parameter values
		// internally (e.g. a program change). Jamma does not cache
		// parameter values anywhere — GetParameter() always reads live from
		// the controller — so there is nothing further to rebuild here;
		// simply acknowledging the flag is correct.
	}
#endif
}

// ---------------------------------------------------------------------------
// State save / restore  (non-RT, job/UI thread only)
// ---------------------------------------------------------------------------
//
// Blob layout: see Vst3StateBlob.h. Restore order follows the VST3 SDK's own
// preset-file container (public.sdk/source/vst/vstpresetfile.cpp):
//   1. IComponent::setState receives the component bytes.
//   2. IEditController::setComponentState receives the SAME component bytes
//      (rewound into a fresh stream — IBStream position is not reset by #1).
//   3. IEditController::setState receives the optional controller-only bytes.
// Callers (Loop/LoopTake/Station via JamFile load) already call SetState only
// on a plugin instance not yet published into its live audio chain, so no
// process() callback can observe the plugin mid-restore. That invariant is a
// precondition here, matching Load/Unload; no lock or deactivate/reactivate
// cycle is added.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> Vst3Plugin::GetState() const
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || !_impl || !_impl->component)
		return {};

	// Give plugins without an open editor a chance to refresh their MIDI
	// controller-mapping table before we capture state.
	PollPendingControllerChanges();

	MemoryIBStream componentStream;
	if (_impl->component->getState(&componentStream) != kResultOk)
		return {};

	std::vector<std::uint8_t> controllerState;
	if (_impl->controller)
	{
		MemoryIBStream controllerStream;
		// kNotImplemented (or any other failure) means "no controller blob",
		// not a fatal error — the component state above is still valid.
		if (_impl->controller->getState(&controllerStream) == kResultOk)
			controllerState = controllerStream.Data();
	}

	return Vst3StateBlob::Frame(componentStream.Data(), controllerState);
#else
	return {};
#endif
}

void Vst3Plugin::SetState(const std::vector<std::uint8_t>& blob)
{
#ifdef JAMMA_VST3_ENABLED
	if (blob.empty() || !_isLoaded || !_impl || !_impl->component)
		return;

	Vst3StateBlob::ParsedState parsed;
	if (!Vst3StateBlob::TryParse(blob, parsed))
	{
		std::cerr << "[Vst3Plugin] SetState: malformed or unrecognised state blob" << std::endl;
		return;
	}

	MemoryIBStream componentStream(parsed.ComponentState.data(), parsed.ComponentState.size());
	if (_impl->component->setState(&componentStream) != kResultOk)
	{
		std::cerr << "[Vst3Plugin] SetState: IComponent::setState failed" << std::endl;
		return; // Do not touch the controller with an unrestored component.
	}

	if (_impl->controller)
	{
		MemoryIBStream componentStreamForController(parsed.ComponentState.data(), parsed.ComponentState.size());
		if (_impl->controller->setComponentState(&componentStreamForController) != kResultOk)
			std::cerr << "[Vst3Plugin] SetState: IEditController::setComponentState failed" << std::endl;

		if (!parsed.ControllerState.empty())
		{
			MemoryIBStream controllerStream(parsed.ControllerState.data(), parsed.ControllerState.size());
			if (_impl->controller->setState(&controllerStream) != kResultOk)
				std::cerr << "[Vst3Plugin] SetState: IEditController::setState failed" << std::endl;
		}

		// Restored programs may change parameter IDs or MIDI-CC assignments.
		_impl->BuildParameterMaps();
		if (_impl->midiMapping)
			_impl->RebuildMidiControllerMap();
	}
#else
	(void)blob;
#endif
}

namespace
{
	std::mutex& UiDestroyQueueMutex() noexcept
	{
		static std::mutex m;
		return m;
	}

	std::vector<std::shared_ptr<vst::IVstPlugin>>& UiDestroyQueue()
	{
		static std::vector<std::shared_ptr<vst::IVstPlugin>> q;
		return q;
	}
}

void vst::QueueForUiThreadDestroy(std::shared_ptr<vst::IVstPlugin> plugin)
{
	if (!plugin)
		return;

	std::lock_guard<std::mutex> lock(UiDestroyQueueMutex());
	UiDestroyQueue().push_back(std::move(plugin));
}

std::size_t vst::DrainUiThreadDestroyQueue() noexcept
{
	std::vector<std::shared_ptr<vst::IVstPlugin>> drained;
	{
		std::lock_guard<std::mutex> lock(UiDestroyQueueMutex());
		drained.swap(UiDestroyQueue());
	}
	const std::size_t count = drained.size();
	// drained destructs here on the UI thread, running ~Vst3Plugin → Unload →
	// IComponent::terminate() and FreeLibrary on the correct thread.
	return count;
}

std::shared_ptr<vst::IVstPlugin> vst::MakePluginForPath(const std::wstring& path)
{
	// Determine type from file extension: .dll -> VST2, anything else -> VST3.
	const auto dotPos = path.rfind(L'.');
	if (dotPos != std::wstring::npos)
	{
		std::wstring ext = path.substr(dotPos);
		// Lowercase comparison
		for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
		if (ext == L".dll")
			return std::make_shared<vst::Vst2Plugin>();
	}
	return std::make_shared<vst::Vst3Plugin>();
}
