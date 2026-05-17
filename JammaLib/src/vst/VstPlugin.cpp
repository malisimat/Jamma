///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstPlugin.h"
#include <algorithm>
#include <iostream>

#ifdef JAMMA_VST3_ENABLED
#include "vst3sdk/pluginterfaces/base/ipluginbase.h"
#include "vst3sdk/pluginterfaces/vst/ivstmessage.h"
#include "vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"
#include "vst3sdk/pluginterfaces/vst/ivsteditcontroller.h"
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

class VstPlugin::Impl
{
public:
	static constexpr Steinberg::int32 MaxHostedChannels = 2;

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
	Steinberg::int32 hostedChannels;
	float* inputChannelPtrs[MaxHostedChannels];
	float* outputChannelPtrs[MaxHostedChannels];
	float inputScratch[MaxHostedChannels][constants::MaxBlockSize];
	float outputScratch[MaxHostedChannels][constants::MaxBlockSize];

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
		hostedChannels(1),
		inputChannelPtrs{ nullptr, nullptr },
		outputChannelPtrs{ nullptr, nullptr }
	{
		for (Steinberg::int32 c = 0; c < MaxHostedChannels; c++)
		{
			std::fill(std::begin(inputScratch[c]), std::end(inputScratch[c]), 0.0f);
			std::fill(std::begin(outputScratch[c]), std::end(outputScratch[c]), 0.0f);
		}
	}
};
#endif

VstPlugin::VstPlugin() :
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

VstPlugin::~VstPlugin()
{
	Unload();
}

bool VstPlugin::PreInit(const std::wstring& path)
{
#ifdef JAMMA_VST3_ENABLED
	if (_moduleHandle)
		return true; // Already pre-initialised (e.g. called twice)

	std::wcout << L"[VstPlugin] PreInit (main thread): path='" << path << L"'" << std::endl;

	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[VstPlugin] PreInit: LoadLibraryW failed: " << GetLastError() << std::endl;
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
		std::cout << "[VstPlugin] PreInit: InitDll() called" << std::endl;
	}

	// Call GetPluginFactory() on the main/UI thread. JUCE-based plugins call
	// initialiseJuce_GUI() and capture the calling thread as JUCE's message thread
	// during GetPluginFactory() or the first createInstance()/initialize() call.
	// All of these MUST happen on the main thread so that plugView->attached()
	// (also main thread) never deadlocks waiting for a job-thread message pump.
	using GetFactoryProc = IPluginFactory* (PLUGIN_API*)();
	auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(_moduleHandle, "GetPluginFactory"));
	if (!getFactory)
	{
		std::cerr << "[VstPlugin] PreInit: GetPluginFactory not found" << std::endl;
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
		std::cerr << "[VstPlugin] PreInit: GetPluginFactory() returned null" << std::endl;
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
		std::cerr << "[VstPlugin] PreInit: no audio effect class found in plugin" << std::endl;
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

	// createInstance() and initialize() are called on the main thread here
	// (not in Load() on the job thread) because JUCE's MessageManager singleton
	// is created on the first call to initialiseJuce_GUI(), which JUCE triggers
	// inside initialize(). By doing this on the main thread, JUCE's message thread
	// == main thread, so attached() on the main thread can call
	// callFunctionOnMessageThread() without needing a job-thread message pump.
	IComponent* rawComponent = nullptr;
	if (_impl->factory->createInstance(componentCid, IComponent::iid, (void**)&rawComponent) != kResultOk
		|| !rawComponent)
	{
		std::cerr << "[VstPlugin] PreInit: createInstance(IComponent) failed" << std::endl;
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
		std::cerr << "[VstPlugin] PreInit: IComponent::initialize() failed" << std::endl;
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

	std::cout << "[VstPlugin] PreInit: success — factory, component, and initialize on main thread, plugin=" << _name << std::endl;
	return true;
#else
	(void)path;
	return false;
#endif
}

bool VstPlugin::Load(const std::wstring& path,
	float sampleRate,
	unsigned int blockSize,
	unsigned int numChannels)
{
#ifdef JAMMA_VST3_ENABLED
	// Only do a full unload if we're replacing a previously loaded plugin.
	// If PreInit() ran, _moduleHandle is set but _isLoaded is false — we must
	// NOT unload here or we'd throw away the main-thread pre-initialisation.
	if (_isLoaded)
		Unload();

	std::wcout << L"[VstPlugin] Load request: path='" << path
		<< L"', sampleRate=" << sampleRate
		<< L", blockSize=" << blockSize
		<< L", requestedChannels=" << numChannels
		<< std::endl;

	_impl->hostedChannels = static_cast<Steinberg::int32>(std::max(1u, std::min(numChannels, static_cast<unsigned int>(Impl::MaxHostedChannels))));
	std::cout << "[VstPlugin] Hosted channels=" << _impl->hostedChannels << std::endl;

	// 1. Load the DLL — skip if PreInit() already loaded it on the main thread
	if (!_moduleHandle)
	{
		_moduleHandle = LoadLibraryW(path.c_str());
		if (!_moduleHandle)
		{
			std::cerr << "[VstPlugin] LoadLibraryW failed: " << GetLastError() << std::endl;
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
		std::cout << "[VstPlugin] Load: reusing pre-loaded DLL handle from PreInit()" << std::endl;
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
			std::cerr << "[VstPlugin] GetPluginFactory not found" << std::endl;
			Unload();
			return false;
		}

		IPluginFactory* rawFactory = getFactory();
		if (!rawFactory)
		{
			std::cerr << "[VstPlugin] GetPluginFactory() returned null" << std::endl;
			Unload();
			return false;
		}

		factory = IPtr<IPluginFactory>(rawFactory, false);
		_impl->factory = factory; // cache for later
	}
	else
	{
		std::cout << "[VstPlugin] Load: reusing pre-loaded factory from PreInit()" << std::endl;
	}

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
			std::cerr << "[VstPlugin] No audio effect class found in plugin" << std::endl;
			Unload();
			return false;
		}

		// 4. Create IComponent (only if PreInit() didn't already do it on the main thread)
		// NOTE: If PreInit() ran, createInstance() + initialize() were already called on the
		// main thread — critical for JUCE-based plugins whose MessageManager is captured
		// on the first initialize() call. Calling them here (job thread) would anchor JUCE's
		// message thread to the job thread, causing attached() on the main thread to deadlock.
		IComponent* rawComponent = nullptr;
		if (factory->createInstance(componentCid, IComponent::iid, (void**)&rawComponent) != kResultOk
			|| !rawComponent)
		{
			std::cerr << "[VstPlugin] createInstance(IComponent) failed" << std::endl;
			Unload();
			return false;
		}
		_impl->component = IPtr<IComponent>(rawComponent, false);

		// 5. Initialize the component
		if (_impl->component->initialize(_impl->hostApplication) != kResultOk)
		{
			std::cerr << "[VstPlugin] IComponent::initialize() failed" << std::endl;
			Unload();
			return false;
		}
	}
	else
	{
		std::cout << "[VstPlugin] Load: reusing pre-initialized component from PreInit()" << std::endl;
	}

	auto inputBusCount = _impl->component->getBusCount(kAudio, kInput);
	auto outputBusCount = _impl->component->getBusCount(kAudio, kOutput);
	std::cout << "[VstPlugin] Audio buses: inputs=" << inputBusCount
		<< ", outputs=" << outputBusCount << std::endl;

	if (inputBusCount > 0)
	{
		BusInfo inBus{};
		if (_impl->component->getBusInfo(kAudio, kInput, 0, inBus) == kResultOk)
			std::cout << "[VstPlugin] Input bus 0 channels=" << inBus.channelCount << std::endl;
	}

	if (outputBusCount > 0)
	{
		BusInfo outBus{};
		if (_impl->component->getBusInfo(kAudio, kOutput, 0, outBus) == kResultOk)
		{
			std::cout << "[VstPlugin] Output bus 0 channels=" << outBus.channelCount << std::endl;
			if (outBus.channelCount != _impl->hostedChannels)
			{
				std::cout << "[VstPlugin] Warning: host currently processes " << _impl->hostedChannels
					<< " channel(s), plugin output bus wants " << outBus.channelCount
					<< " channel(s). Audio result may be unchanged or degraded." << std::endl;
			}
		}
	}

	// 6. Query IAudioProcessor
	IAudioProcessor* rawProcessor = nullptr;
	if (_impl->component->queryInterface(IAudioProcessor::iid, (void**)&rawProcessor) != kResultOk
		|| !rawProcessor)
	{
		std::cerr << "[VstPlugin] queryInterface(IAudioProcessor) failed" << std::endl;
		Unload();
		return false;
	}
	_impl->processor = IPtr<IAudioProcessor>(rawProcessor, false);

	// 7. Setup processing
	ProcessSetup setup;
	setup.processMode = kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = static_cast<int32>(blockSize);
	setup.sampleRate = static_cast<SampleRate>(sampleRate);

	if (_impl->processor->setupProcessing(setup) != kResultOk)
	{
		std::cerr << "[VstPlugin] setupProcessing() failed" << std::endl;
		Unload();
		return false;
	}

	// 8. Activate buses, then set component active and start processing.
	// setActive/setProcessing are called here (on the job thread) because audio
	// processing must be active regardless of whether an editor is ever opened.
	// This is now safe for JUCE-based plugins because PreInit() called
	// createInstance()+initialize() on the main thread, anchoring JUCE's
	// MessageManager to the main thread. setProcessing(true) here only invokes
	// the audio-processor path (prepareToPlay), which does not require the
	// message thread. attached() on the main thread will therefore call
	// callFunctionOnMessageThread() inline (already on message thread), not block.
	auto inActivateRes = _impl->component->activateBus(kAudio, kInput, 0, true);
	auto outActivateRes = _impl->component->activateBus(kAudio, kOutput, 0, true);
	auto activeRes = _impl->component->setActive(true);
	auto processingRes = _impl->processor->setProcessing(true);
	_isActivated.store(true, std::memory_order_release);
	std::cout << "[VstPlugin] activateBus/setActive/setProcessing: input=" << inActivateRes
		<< ", output=" << outActivateRes
		<< ", setActive=" << activeRes
		<< ", setProcessing=" << processingRes << std::endl;

	// 9. Pre-allocate ProcessData so ProcessBlock() is heap-allocation-free
	for (Steinberg::int32 c = 0; c < _impl->hostedChannels; c++)
	{
		_impl->inputChannelPtrs[c] = _impl->inputScratch[c];
		_impl->outputChannelPtrs[c] = _impl->outputScratch[c];
	}

	_impl->inputBus.numChannels = _impl->hostedChannels;
	_impl->inputBus.channelBuffers32 = _impl->inputChannelPtrs;
	_impl->inputBus.silenceFlags = 0;

	_impl->outputBus.numChannels = _impl->hostedChannels;
	_impl->outputBus.channelBuffers32 = _impl->outputChannelPtrs;
	_impl->outputBus.silenceFlags = 0;

	_impl->processData.processMode = kRealtime;
	_impl->processData.symbolicSampleSize = kSample32;
	_impl->processData.numSamples = static_cast<int32>(blockSize);
	_impl->processData.numInputs = 1;
	_impl->processData.numOutputs = 1;
	_impl->processData.inputs = &_impl->inputBus;
	_impl->processData.outputs = &_impl->outputBus;
	_impl->processData.inputEvents = nullptr;
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
		std::cout << "[VstPlugin] Controller queryInterface: ok" << std::endl;
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
				std::cout << "[VstPlugin] Separate controller created, initialize=" << initRes << std::endl;
			}
		}
	}

	if (!_impl->controller)
		std::cout << "[VstPlugin] No controller available (editor may not open)" << std::endl;
	else
	{
		_impl->controller->setComponentHandler(_impl->componentHandler.get());
	}

	// IConnectionPoint::connect() is deferred from Load() to OpenEditor() (called
	// after attached() succeeds) to match editorhost's behavior. editorhost never
	// calls connect() at all, and calling it on the job thread before attached()
	// may anchor JUCE cross-thread message dispatch to the job thread, causing
	// attached() on the main thread to deadlock waiting for the job-thread pump.
	if (_impl->controller)
	{
		IConnectionPoint* rawComponentConn = nullptr;
		IConnectionPoint* rawControllerConn = nullptr;
		if (_impl->component->queryInterface(IConnectionPoint::iid, (void**)&rawComponentConn) == kResultOk && rawComponentConn)
			_impl->componentConnection = IPtr<IConnectionPoint>(rawComponentConn, false);

		if (_impl->controller->queryInterface(IConnectionPoint::iid, (void**)&rawControllerConn) == kResultOk && rawControllerConn)
			_impl->controllerConnection = IPtr<IConnectionPoint>(rawControllerConn, false);

		if (_impl->componentConnection && _impl->controllerConnection)
			std::cout << "[VstPlugin] ConnectionPoint available, will connect in OpenEditor()" << std::endl;
		else
			std::cout << "[VstPlugin] ConnectionPoint not available on component/controller" << std::endl;
	}

	_isLoaded = true;
	std::cout << "[VstPlugin] Loaded: " << _name << std::endl;
	return true;

#else
	(void)path; (void)sampleRate; (void)blockSize; (void)numChannels;
	std::cerr << "[VstPlugin] VST3 support not compiled in (define JAMMA_VST3_ENABLED)" << std::endl;
	return false;
#endif
}

void VstPlugin::Unload()
{
#ifdef JAMMA_VST3_ENABLED
	CloseEditor();

	if (_impl && _impl->component)
	{
		if (_isActivated.exchange(false, std::memory_order_acq_rel))
		{
			if (_impl->processor)
				_impl->processor->setProcessing(false);
			_impl->component->setActive(false);
		}

		if (_impl->componentConnection && _impl->controllerConnection)
		{
			_impl->componentConnection->disconnect(_impl->controllerConnection);
			_impl->controllerConnection->disconnect(_impl->componentConnection);
		}
		_impl->_connectionsDone = false;

		_impl->component->terminate();
		_impl->component = nullptr;
	}

	if (_impl)
	{
		_impl->processor = nullptr;
		_impl->controller = nullptr;
		_impl->componentConnection = nullptr;
		_impl->controllerConnection = nullptr;
		_impl->plugView = nullptr;

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

void VstPlugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| _editorOpening.load(std::memory_order_acquire))
		return;

	if (!_impl)
		return;

	if (numSamples <= 0 || static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	std::copy(monoBuf, monoBuf + numSamples, _impl->inputScratch[0]);
	if (_impl->hostedChannels > 1)
		std::fill(_impl->inputScratch[1], _impl->inputScratch[1] + numSamples, 0.0f);

	// Update sample count (other ProcessData fields are fixed from Load())
	_impl->processData.numSamples = numSamples;

	_impl->processor->process(_impl->processData);

	std::copy(_impl->outputScratch[0], _impl->outputScratch[0] + numSamples, monoBuf);
#else
	(void)monoBuf; (void)numSamples;
#endif
}

void VstPlugin::ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| _editorOpening.load(std::memory_order_acquire))
		return;

	if (!_impl)
		return;

	if (numSamples <= 0 || static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_impl->hostedChannels < 2)
	{
		ProcessBlock(leftBuf, numSamples);
		ProcessBlock(rightBuf, numSamples);
		return;
	}

	std::copy(leftBuf, leftBuf + numSamples, _impl->inputScratch[0]);
	std::copy(rightBuf, rightBuf + numSamples, _impl->inputScratch[1]);

	_impl->processData.numSamples = numSamples;
	_impl->processor->process(_impl->processData);

	std::copy(_impl->outputScratch[0], _impl->outputScratch[0] + numSamples, leftBuf);
	std::copy(_impl->outputScratch[1], _impl->outputScratch[1] + numSamples, rightBuf);
#else
	(void)leftBuf; (void)rightBuf; (void)numSamples;
#endif
}

bool VstPlugin::OpenEditor(HWND parentHwnd)
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
		std::cout << "[VstPlugin] OpenEditor failed: loaded=" << _isLoaded
			<< ", hasImpl=" << (_impl ? 1 : 0)
			<< ", hasController=" << ((_impl && _impl->controller) ? 1 : 0)
			<< std::endl;
		return false;
	}

	// Connect component↔controller IConnectionPoint endpoints here on the main thread,
	// BEFORE createView(). In JUCE's single-object VST3 design, connect() transfers the
	// shared AudioProcessor reference from the component side to the controller side;
	// without it createView() returns null. Calling connect() on the job thread (in
	// Load()) caused a hang in attached() because JUCE posted cross-thread messages
	// anchored to the job thread. Main-thread connect() avoids that.
	if (!_impl->_connectionsDone && _impl->componentConnection && _impl->controllerConnection)
	{
		auto c2k = _impl->componentConnection->connect(_impl->controllerConnection);
		auto k2c = _impl->controllerConnection->connect(_impl->componentConnection);
		_impl->_connectionsDone = true;
		std::cout << "[VstPlugin] ConnectionPoint connect: c2k=" << c2k
			<< ", k2c=" << k2c << std::endl;
	}

	IPlugView* rawView = _impl->controller->createView(Steinberg::Vst::ViewType::kEditor);
	if (!rawView)
	{
		std::cout << "[VstPlugin] OpenEditor failed: createView returned null" << std::endl;
		return false;
	}

	_impl->plugView = IPtr<IPlugView>(rawView, false);

	if (_impl->plugView->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk)
	{
		std::cout << "[VstPlugin] OpenEditor failed: kPlatformTypeHWND not supported" << std::endl;
		_impl->plugView = nullptr;
		return false;
	}

	_impl->plugFrame->SetHostWindow(parentHwnd);
	_impl->plugView->setFrame(_impl->plugFrame.get());

	const auto attachedResult = _impl->plugView->attached(reinterpret_cast<void*>(parentHwnd), kPlatformTypeHWND);
	if (attachedResult != kResultOk)
	{
		std::cout << "[VstPlugin] OpenEditor failed: attached(HWND) failed" << std::endl;
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

	std::cout << "[VstPlugin] OpenEditor success: size=" << _editorSize.Width << "x" << _editorSize.Height << std::endl;

	return true;
#else
	(void)parentHwnd;
	return false;
#endif
}

void VstPlugin::CloseEditor()
{
#ifdef JAMMA_VST3_ENABLED
	if (_impl && _impl->plugView)
	{
		_impl->plugView->setFrame(nullptr);
		_impl->plugView->removed();
		_impl->plugView = nullptr;
		_editorSize = { 0, 0 };
	}
#endif
}

utils::Size2d VstPlugin::GetEditorSize() const noexcept
{
	return _editorSize;
}
