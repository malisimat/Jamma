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
#include "vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"
#include "vst3sdk/pluginterfaces/vst/ivsteditcontroller.h"
#include "vst3sdk/pluginterfaces/gui/iplugview.h"
#endif

using namespace vst;

#ifdef JAMMA_VST3_ENABLED
using namespace Steinberg;
using namespace Steinberg::Vst;

class VstPlugin::Impl
{
public:
	static constexpr Steinberg::int32 NumChannels = 1;

	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	Steinberg::IPtr<Steinberg::IPlugView> plugView;

	Steinberg::Vst::ProcessData processData;
	Steinberg::Vst::AudioBusBuffers inputBus;
	Steinberg::Vst::AudioBusBuffers outputBus;
	float* inputChannelPtr;
	float* outputChannelPtr;
	float inputScratch[constants::MaxBlockSize];
	float outputScratch[constants::MaxBlockSize];

	Impl() :
		component(nullptr),
		processor(nullptr),
		controller(nullptr),
		plugView(nullptr),
		processData(),
		inputBus(),
		outputBus(),
		inputChannelPtr(nullptr),
		outputChannelPtr(nullptr)
	{
		std::fill(std::begin(inputScratch), std::end(inputScratch), 0.0f);
		std::fill(std::begin(outputScratch), std::end(outputScratch), 0.0f);
	}
};
#endif

VstPlugin::VstPlugin() :
	_isLoaded(false),
	_name(),
	_isBypassed(false),
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
	Unload();

	// 1. Load the DLL
	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[VstPlugin] LoadLibraryW failed: " << GetLastError() << std::endl;
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

	// 5. Initialize the component (nullptr host context is accepted by most plugins)
	if (_impl->component->initialize(nullptr) != kResultOk)
	{
		std::cerr << "[VstPlugin] IComponent::initialize() failed" << std::endl;
		Unload();
		return false;
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
	_impl->component->activateBus(kAudio, kInput, 0, true);
	_impl->component->activateBus(kAudio, kOutput, 0, true);
	_impl->component->setActive(true);

	// 9. Pre-allocate ProcessData so ProcessBlock() is heap-allocation-free
	_impl->inputChannelPtr = _impl->inputScratch;
	_impl->outputChannelPtr = _impl->outputScratch;

	_impl->inputBus.numChannels = Impl::NumChannels;
	_impl->inputBus.channelBuffers32 = &_impl->inputChannelPtr;
	_impl->inputBus.silenceFlags = 0;

	_impl->outputBus.numChannels = Impl::NumChannels;
	_impl->outputBus.channelBuffers32 = &_impl->outputChannelPtr;
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
				_impl->controller->initialize(nullptr);
			}
		}
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
		_impl->component->setActive(false);
		_impl->component->terminate();
		_impl->component = nullptr;
	}

	if (_impl)
	{
		_impl->processor = nullptr;
		_impl->controller = nullptr;
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
}

void VstPlugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || _isBypassed.load(std::memory_order_relaxed))
		return;

	// Copy mono input into the scratch buffer
	std::copy(monoBuf, monoBuf + numSamples, _impl->inputScratch);

	// Update sample count (other ProcessData fields are fixed from Load())
	_impl->processData.numSamples = numSamples;

	_impl->processor->process(_impl->processData);

	// Copy processed output back to the caller's buffer
	std::copy(_impl->outputScratch, _impl->outputScratch + numSamples, monoBuf);
#else
	(void)monoBuf; (void)numSamples;
#endif
}

bool VstPlugin::OpenEditor(HWND parentHwnd)
{
#ifdef JAMMA_VST3_ENABLED
	if (!_isLoaded || !_impl || !_impl->controller)
		return false;

	IPlugView* rawView = _impl->controller->createView(Steinberg::Vst::ViewType::kEditor);
	if (!rawView)
		return false;

	_impl->plugView = IPtr<IPlugView>(rawView, false);

	if (_impl->plugView->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk)
	{
		_impl->plugView = nullptr;
		return false;
	}

	if (_impl->plugView->attached(reinterpret_cast<void*>(parentHwnd), kPlatformTypeHWND) != kResultOk)
	{
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
