///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstPlugin.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include "VstDiagnostics.h"
#include "../utils/StringUtils.h"

#ifdef JAMMA_VST3_ENABLED
#include "vst3sdk/pluginterfaces/base/ipluginbase.h"
#include "vst3sdk/pluginterfaces/vst/ivstmessage.h"
#include "vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"
#include "vst3sdk/pluginterfaces/vst/ivsteditcontroller.h"
#include "vst3sdk/pluginterfaces/gui/iplugview.h"
#include "vst3sdk/public.sdk/source/vst/hosting/hostclasses.h"
#endif

using namespace vst;

namespace
{
	std::string BoolString(const bool value)
	{
		return value ? "true" : "false";
	}

	std::string PointerString(const void* ptr)
	{
		std::ostringstream ss;
		ss << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(ptr);
		return ss.str();
	}
}

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

	Steinberg::IPtr<Steinberg::Vst::HostApplication> hostApplication;
	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentConnection;
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> controllerConnection;
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
		hostApplication(Steinberg::IPtr<Steinberg::Vst::HostApplication>(new Steinberg::Vst::HostApplication(), false)),
		component(nullptr),
		processor(nullptr),
		controller(nullptr),
		componentHandler(std::make_unique<HostComponentHandler>()),
		plugFrame(std::make_unique<HostPlugFrame>()),
		plugView(nullptr),
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

bool VstPlugin::Load(const std::wstring& path,
	float sampleRate,
	unsigned int blockSize,
	unsigned int numChannels)
{
#ifdef JAMMA_VST3_ENABLED
	vst::VstDiagnostics::Log("VstPlugin", "load-begin", std::string("path=") + utils::EncodeUtf8(path)
		+ ", sampleRate=" + std::to_string(sampleRate)
		+ ", blockSize=" + std::to_string(blockSize)
		+ ", channels=" + std::to_string(numChannels));
	Unload();
	std::wcout << L"[VstPlugin] Load request: path='" << path
		<< L"', sampleRate=" << sampleRate
		<< L", blockSize=" << blockSize
		<< L", requestedChannels=" << numChannels
		<< std::endl;

	_impl->hostedChannels = static_cast<Steinberg::int32>(std::max(1u, std::min(numChannels, static_cast<unsigned int>(Impl::MaxHostedChannels))));
	std::cout << "[VstPlugin] Hosted channels=" << _impl->hostedChannels << std::endl;

	// 1. Load the DLL
	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[VstPlugin] LoadLibraryW failed: " << GetLastError() << std::endl;
		vst::VstDiagnostics::Log("VstPlugin", "load-failed", std::string("LoadLibraryW error=") + std::to_string(GetLastError()));
		return false;
	}

	// 2. Get the factory function
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

	IPtr<IPluginFactory> factory(rawFactory, false);

	// 3. Find the first IAudioProcessor class
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

	// 4. Create IComponent
	IComponent* rawComponent = nullptr;
	if (factory->createInstance(componentCid, IComponent::iid, (void**)&rawComponent) != kResultOk
		|| !rawComponent)
	{
		std::cerr << "[VstPlugin] createInstance(IComponent) failed" << std::endl;
		Unload();
		return false;
	}
	_impl->component = IPtr<IComponent>(rawComponent, false);

	// 5. Initialize the component with a real host application context.
	if (_impl->component->initialize(_impl->hostApplication) != kResultOk)
	{
		std::cerr << "[VstPlugin] IComponent::initialize() failed" << std::endl;
		Unload();
		return false;
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

	// 8. Activate buses and component
	// Attempt to activate the first input/output audio bus (index 0).
	// Plugins that have no bus at index 0 will gracefully return kResultFalse.
	auto inActivateRes = _impl->component->activateBus(kAudio, kInput, 0, true);
	auto outActivateRes = _impl->component->activateBus(kAudio, kOutput, 0, true);
	auto activeRes = _impl->component->setActive(true);
	auto processingRes = _impl->processor->setProcessing(true);
	std::cout << "[VstPlugin] activateBus results: input=" << inActivateRes
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

	// For separate component/controller plugins, connect both endpoints so
	// the UI and processor can exchange state/parameter messages.
	if (_impl->controller)
	{
		IConnectionPoint* rawComponentConn = nullptr;
		IConnectionPoint* rawControllerConn = nullptr;
		if (_impl->component->queryInterface(IConnectionPoint::iid, (void**)&rawComponentConn) == kResultOk && rawComponentConn)
			_impl->componentConnection = IPtr<IConnectionPoint>(rawComponentConn, false);

		if (_impl->controller->queryInterface(IConnectionPoint::iid, (void**)&rawControllerConn) == kResultOk && rawControllerConn)
			_impl->controllerConnection = IPtr<IConnectionPoint>(rawControllerConn, false);

		if (_impl->componentConnection && _impl->controllerConnection)
		{
			auto c2k = _impl->componentConnection->connect(_impl->controllerConnection);
			auto k2c = _impl->controllerConnection->connect(_impl->componentConnection);
			std::cout << "[VstPlugin] ConnectionPoint connect: component->controller=" << c2k
				<< ", controller->component=" << k2c << std::endl;
		}
		else
		{
			std::cout << "[VstPlugin] ConnectionPoint not available on component/controller" << std::endl;
		}
	}

	_isLoaded = true;
	std::cout << "[VstPlugin] Loaded: " << _name << std::endl;
	vst::VstDiagnostics::Log("VstPlugin", "load-success", std::string("plugin=") + _name);
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
	vst::VstDiagnostics::Log("VstPlugin", "unload-begin", _name.empty() ? std::string("plugin=<unknown>") : std::string("plugin=") + _name);
	CloseEditor();

	if (_impl && _impl->component)
	{
		if (_impl->processor)
			_impl->processor->setProcessing(false);

		if (_impl->componentConnection && _impl->controllerConnection)
		{
			_impl->componentConnection->disconnect(_impl->controllerConnection);
			_impl->controllerConnection->disconnect(_impl->componentConnection);
		}

		_impl->component->setActive(false);
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
	}

	if (_moduleHandle)
	{
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
	}
#endif

	_isLoaded = false;
	_name.clear();
	_editorSize = { 0, 0 };
	vst::VstDiagnostics::Log("VstPlugin", "unload-complete");
}

void VstPlugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || _isBypassed.load(std::memory_order_relaxed)
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
	if (!_isLoaded || _isBypassed.load(std::memory_order_relaxed)
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
	const auto openStart = std::chrono::steady_clock::now();
	vst::VstDiagnostics::Log("VstPlugin", "open-editor-begin", std::string("plugin=") + _name + ", hwnd=" + PointerString(parentHwnd));
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
		vst::VstDiagnostics::Log("VstPlugin", "open-editor-failed",
			std::string("loaded=") + BoolString(_isLoaded)
			+ ", hasImpl=" + BoolString(_impl != nullptr)
			+ ", hasController=" + BoolString(_impl && _impl->controller));
		return false;
	}

	IPlugView* rawView = _impl->controller->createView(Steinberg::Vst::ViewType::kEditor);
	if (!rawView)
	{
		std::cout << "[VstPlugin] OpenEditor failed: createView returned null" << std::endl;
		vst::VstDiagnostics::Log("VstPlugin", "open-editor-failed", "createView returned null");
		return false;
	}

	_impl->plugView = IPtr<IPlugView>(rawView, false);

	if (_impl->plugView->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk)
	{
		std::cout << "[VstPlugin] OpenEditor failed: kPlatformTypeHWND not supported" << std::endl;
		vst::VstDiagnostics::Log("VstPlugin", "open-editor-failed", "kPlatformTypeHWND unsupported");
		_impl->plugView = nullptr;
		return false;
	}

	_impl->plugFrame->SetHostWindow(parentHwnd);
	_impl->plugView->setFrame(_impl->plugFrame.get());

	const auto attachStart = std::chrono::steady_clock::now();
	const auto attachedResult = _impl->plugView->attached(reinterpret_cast<void*>(parentHwnd), kPlatformTypeHWND);
	const auto attachMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attachStart).count();
	if (attachedResult != kResultOk)
	{
		std::cout << "[VstPlugin] OpenEditor failed: attached(HWND) failed" << std::endl;
		vst::VstDiagnostics::Log("VstPlugin", "open-editor-failed",
			std::string("attached failed, result=") + std::to_string(attachedResult)
			+ ", attachMs=" + std::to_string(attachMs));
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
	vst::VstDiagnostics::Log("VstPlugin", "open-editor-success",
		std::string("plugin=") + _name
		+ ", attachMs=" + std::to_string(attachMs)
		+ ", totalMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - openStart).count())
		+ ", size=" + std::to_string(_editorSize.Width) + "x" + std::to_string(_editorSize.Height));

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
		const auto closeStart = std::chrono::steady_clock::now();
		vst::VstDiagnostics::Log("VstPlugin", "close-editor-begin", std::string("plugin=") + _name);
		_impl->plugView->setFrame(nullptr);
		_impl->plugView->removed();
		_impl->plugView = nullptr;
		_editorSize = { 0, 0 };
		vst::VstDiagnostics::Log("VstPlugin", "close-editor-complete",
			std::string("plugin=") + _name
			+ ", totalMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - closeStart).count()));
	}
#endif
}

utils::Size2d VstPlugin::GetEditorSize() const noexcept
{
	return _editorSize;
}
