///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst3Plugin.h"
#include "Vst2Plugin.h"
#include <algorithm>
#include <array>
#include <cwctype>
#include <iostream>
#include <mutex>
#include <vector>
#include <string>

#ifdef JAMMA_VST3_ENABLED
#include "vst3sdk/pluginterfaces/base/ipluginbase.h"
#include "vst3sdk/pluginterfaces/vst/ivstmessage.h"
#include "vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"
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
	HostComponentHandler()
	{
		FUNKNOWN_CTOR
	}

	~HostComponentHandler() noexcept { FUNKNOWN_DTOR }

	tresult PLUGIN_API beginEdit(ParamID id) override
	{
		(void)id;
		return kResultOk;
	}

	tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override
	{
		(void)id;
		(void)valueNormalized;
		return kResultOk;
	}

	tresult PLUGIN_API endEdit(ParamID id) override
	{
		(void)id;
		return kResultOk;
	}

	tresult PLUGIN_API restartComponent(int32 flags) override
	{
		(void)flags;
		return kResultOk;
	}

	DECLARE_FUNKNOWN_METHODS
};

IMPLEMENT_FUNKNOWN_METHODS(HostComponentHandler, IComponentHandler, IComponentHandler::iid)

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

	void AddMidiEvent(const midi::MidiEvent& midiEvent, bool isRealtime) noexcept
	{
		if (_count >= MaxEvents || 0u == _blockNumSamples)
			return;

		const auto blockEnd = _blockStartSample + _blockNumSamples;
		const bool inWindow = (midiEvent.sampleOffset >= _blockStartSample && midiEvent.sampleOffset < blockEnd);
		if (!inWindow && !isRealtime)
			return;

		Event event{};
		event.busIndex = 0;
		event.sampleOffset = inWindow ? static_cast<int32>(midiEvent.sampleOffset - _blockStartSample) : 0;
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
		inputEvents(std::make_unique<FixedEventList>())
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
	auto processingRes = _impl->processor->setProcessing(true);
	_isActivated.store(true, std::memory_order_release);
	std::cout << "[Vst3Plugin] activateBus/setActive/setProcessing: input=" << inActivateRes
		<< ", output=" << outActivateRes
		<< ", setActive=" << activeRes
		<< ", setProcessing=" << processingRes << std::endl;

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
	_impl->processData.inputParameterChanges = nullptr;
	_impl->processData.outputParameterChanges = nullptr;
	_impl->processData.processContext = nullptr;

	// 10. Optionally retrieve IEditController (may be the same object or separate)
	IEditController* rawController = nullptr;
	if (_impl->component->queryInterface(IEditController::iid, (void**)&rawController) == kResultOk
		&& rawController)
	{
		_impl->controller = IPtr<IEditController>(rawController, false);
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
			if (factory->createInstance(controllerCid, IEditController::iid, (void**)&rawController) == kResultOk
				&& rawController)
			{
				_impl->controller = IPtr<IEditController>(rawController, false);
				auto initRes = _impl->controller->initialize(_impl->hostApplication);
				std::cout << "[Vst3Plugin] Separate controller created, initialize=" << initRes << std::endl;
			}
		}
	}

	if (!_impl->controller)
		std::cout << "[Vst3Plugin] No controller available (editor may not open)" << std::endl;
	else
	{
		_impl->controller->setComponentHandler(_impl->componentHandler.get());
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
		if (_impl->processor)
			_impl->processor->setProcessing(false);
		if (_impl->component)
			_impl->component->setActive(false);
	}

	if (_impl->componentConnection && _impl->controllerConnection)
	{
		_impl->componentConnection->disconnect(_impl->controllerConnection);
		_impl->controllerConnection->disconnect(_impl->componentConnection);
	}
	_impl->_connectionsDone = false;

	if (_impl->controller)
		_impl->controller->setComponentHandler(nullptr);

	_impl->processor = nullptr;
	_impl->controller = nullptr;
	_impl->componentConnection = nullptr;
	_impl->controllerConnection = nullptr;
	_impl->plugView = nullptr;

	if (terminateComponent && _impl->component)
	{
		_impl->component->terminate();
		_impl->component = nullptr;
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

	_impl->processor->process(_impl->processData);

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
	_impl->processor->process(_impl->processData);

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
	_impl->processor->process(_impl->processData);
	CopyOutputToMulti(_impl->outputChannelPtrs.data(), _impl->outputChannels, numSamples, channelBufs, numChannels);
#else
	(void)channelBufs; (void)numChannels; (void)numSamples;
#endif
}

void Vst3Plugin::BeginMidiBlock(std::uint32_t blockStartSample,
	std::uint32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl && _impl->inputEvents)
		_impl->inputEvents->BeginBlock(blockStartSample, numSamples);
#else
	(void)blockStartSample; (void)numSamples;
#endif
}

void Vst3Plugin::SendMidiEvent(const midi::MidiEvent& event,
	bool isRealtime) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl && _impl->inputEvents)
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

	IPlugView* rawView = _impl->controller->createView(Steinberg::Vst::ViewType::kEditor);
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
		_impl->plugView = nullptr;
		return false;
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
