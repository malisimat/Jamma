///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst2Plugin.h"
#include <algorithm>
#include <iostream>

using namespace vst;

Vst2Plugin::Vst2Plugin() :
#ifdef JAMMA_VST2_ENABLED
	_effect(nullptr),
#endif
	_moduleHandle(nullptr),
	_isLoaded(false),
	_isActivated(false),
	_name(),
	_isBypassed(false),
	_editorSize({ 0, 0 }),
	_inputChannelPtrs(),
	_outputChannelPtrs(),
	_inputScratchStorage(),
	_outputScratchStorage(),
	_inputChannels(0),
	_outputChannels(0),
	_requestedChannels(1)
{
}

Vst2Plugin::~Vst2Plugin()
{
	Unload();
}

bool Vst2Plugin::PreInit(const std::wstring& path)
{
#ifdef JAMMA_VST2_ENABLED
	if (_moduleHandle)
		return true; // Already pre-initialised

	std::wcout << L"[Vst2Plugin] PreInit: path='" << path << L"'" << std::endl;

	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[Vst2Plugin] PreInit: LoadLibraryW failed: " << GetLastError() << std::endl;
		return false;
	}

	// Verify the plugin entry-point exists.  We don't call it here —
	// that is deferred to Load() on the job thread.
	auto mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
		GetProcAddress(_moduleHandle, "VSTPluginMain"));
	if (!mainProc)
		mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
			GetProcAddress(_moduleHandle, "main"));

	if (!mainProc)
	{
		std::cerr << "[Vst2Plugin] PreInit: no VST2 entry point found" << std::endl;
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	std::cout << "[Vst2Plugin] PreInit: entry-point found, DLL loaded" << std::endl;
	return true;
#else
	(void)path;
	return false;
#endif
}

bool Vst2Plugin::Load(const std::wstring& path,
	float sampleRate,
	unsigned int blockSize,
	unsigned int numChannels,
	HostedLayoutMode /*layoutMode*/)
{
#ifdef JAMMA_VST2_ENABLED
	if (_isLoaded)
		Unload();

	std::wcout << L"[Vst2Plugin] Load: path='" << path
		<< L"', sampleRate=" << sampleRate
		<< L", blockSize=" << blockSize
		<< L", requestedChannels=" << numChannels
		<< std::endl;

	_requestedChannels = static_cast<int32_t>((std::max)(1u, numChannels));

	// 1. Load the DLL if PreInit() hasn't done so already.
	if (!_moduleHandle)
	{
		_moduleHandle = LoadLibraryW(path.c_str());
		if (!_moduleHandle)
		{
			std::cerr << "[Vst2Plugin] Load: LoadLibraryW failed: " << GetLastError() << std::endl;
			return false;
		}
	}
	else
	{
		std::cout << "[Vst2Plugin] Load: reusing pre-loaded DLL from PreInit()" << std::endl;
	}

	// 2. Locate and call the plugin entry-point to obtain the AEffect.
	auto mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
		GetProcAddress(_moduleHandle, "VSTPluginMain"));
	if (!mainProc)
		mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
			GetProcAddress(_moduleHandle, "main"));

	if (!mainProc)
	{
		std::cerr << "[Vst2Plugin] Load: no VST2 entry-point found" << std::endl;
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	_effect = mainProc(Vst2Plugin::HostCallback);
	if (!_effect || (_effect->magic != kEffectMagic))
	{
		std::cerr << "[Vst2Plugin] Load: VSTPluginMain returned null or wrong magic" << std::endl;
		_effect = nullptr;
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	// Store our 'this' pointer in the user field for the host callback.
	_effect->user = this;

	// 3. Query name.
	{
		char effectName[64] = {};
		if (_effect->dispatcher(_effect, effGetEffectName, 0, 0, effectName, 0.0f) != 0)
			_name = effectName;
		if (_name.empty())
		{
			char vendorName[64] = {};
			_effect->dispatcher(_effect, effGetVendorString, 0, 0, vendorName, 0.0f);
			_name = vendorName;
		}
		if (_name.empty())
			_name = "(unknown vst2)";
	}

	// 4. Open and configure.
	_effect->dispatcher(_effect, effOpen, 0, 0, nullptr, 0.0f);
	_effect->dispatcher(_effect, effSetSampleRate, 0, 0, nullptr, sampleRate);
	_effect->dispatcher(_effect, effSetBlockSize, 0,
		static_cast<VstIntPtr>(blockSize), nullptr, 0.0f);

	_inputChannels = (std::max)(1, static_cast<int32_t>(_effect->numInputs));
	_outputChannels = (std::max)(1, static_cast<int32_t>(_effect->numOutputs));

	std::cout << "[Vst2Plugin] Load: name='" << _name
		<< "', inputs=" << _inputChannels
		<< ", outputs=" << _outputChannels << std::endl;

	// 5. Pre-allocate channel pointer arrays and scratch buffers so
	//    ProcessBlock never touches the heap.
	_inputScratchStorage.assign(
		static_cast<size_t>(_inputChannels) * constants::MaxBlockSize, 0.0f);
	_outputScratchStorage.assign(
		static_cast<size_t>(_outputChannels) * constants::MaxBlockSize, 0.0f);

	_inputChannelPtrs.resize(static_cast<size_t>(_inputChannels));
	_outputChannelPtrs.resize(static_cast<size_t>(_outputChannels));

	for (int32_t c = 0; c < _inputChannels; ++c)
		_inputChannelPtrs[static_cast<size_t>(c)] =
			_inputScratchStorage.data() + static_cast<size_t>(c) * constants::MaxBlockSize;

	for (int32_t c = 0; c < _outputChannels; ++c)
		_outputChannelPtrs[static_cast<size_t>(c)] =
			_outputScratchStorage.data() + static_cast<size_t>(c) * constants::MaxBlockSize;

	// 6. Start the effect (resume).
	_effect->dispatcher(_effect, effMainsChanged, 0, 1, nullptr, 0.0f);
	_isActivated.store(true, std::memory_order_release);

	_isLoaded = true;
	std::cout << "[Vst2Plugin] Load: success — " << _name << std::endl;
	return true;
#else
	(void)path; (void)sampleRate; (void)blockSize; (void)numChannels;
	std::cerr << "[Vst2Plugin] VST2 support not compiled in (define JAMMA_VST2_ENABLED)" << std::endl;
	return false;
#endif
}

void Vst2Plugin::Unload()
{
#ifdef JAMMA_VST2_ENABLED
	CloseEditor();

	if (_isActivated.exchange(false, std::memory_order_acq_rel))
	{
		if (_effect)
			_effect->dispatcher(_effect, effMainsChanged, 0, 0, nullptr, 0.0f);
	}

	// Null out pointers before clearing storage so any concurrent
	// ProcessBlock (guarded by _isActivated) cannot chase stale pointers.
	_inputChannelPtrs.clear();
	_outputChannelPtrs.clear();
	_inputScratchStorage.clear();
	_outputScratchStorage.clear();

	if (_effect)
	{
		_effect->dispatcher(_effect, effClose, 0, 0, nullptr, 0.0f);
		_effect = nullptr;
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
	_inputChannels = 0;
	_outputChannels = 0;
}

void Vst2Plugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded
		|| !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| !_effect
		|| !_effect->processReplacing)
		return;

	if (numSamples <= 0
		|| static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_inputChannelPtrs.empty() || _outputChannelPtrs.empty())
		return;

	CopyMonoToInputBuffers(monoBuf, numSamples, _inputChannels,
		_inputChannelPtrs.data());

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);

	FoldOutputToMono(_outputChannelPtrs.data(), _outputChannels,
		numSamples, monoBuf);
#else
	(void)monoBuf; (void)numSamples;
#endif
}

void Vst2Plugin::ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded
		|| !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| !_effect
		|| !_effect->processReplacing)
		return;

	if (numSamples <= 0
		|| static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_inputChannelPtrs.empty() || _outputChannelPtrs.empty())
		return;

	if (_inputChannels != 2 || _outputChannels != 2)
	{
		// Not a native stereo plugin — fold to mono and copy to both channels.
		ProcessBlock(leftBuf, numSamples);
		std::copy(leftBuf, leftBuf + static_cast<std::ptrdiff_t>(numSamples), rightBuf);
		return;
	}

	float* inputChans[] = { leftBuf, rightBuf };
	CopyMultiToInputBuffers(inputChans, 2, numSamples,
		_inputChannelPtrs.data(), _inputChannels);

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);

	float* outputChans[] = { leftBuf, rightBuf };
	CopyOutputToMulti(_outputChannelPtrs.data(), _outputChannels,
		numSamples, outputChans, 2);
#else
	(void)leftBuf; (void)rightBuf; (void)numSamples;
#endif
}

void Vst2Plugin::ProcessBlockMulti(float* const* channelBufs, int32_t numChannels, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded
		|| !_isActivated.load(std::memory_order_acquire)
		|| _isBypassed.load(std::memory_order_relaxed)
		|| !_effect
		|| !_effect->processReplacing
		|| !channelBufs)
		return;

	if (numChannels <= 0
		|| numChannels < _requestedChannels
		|| numSamples <= 0
		|| static_cast<unsigned int>(numSamples) > constants::MaxBlockSize)
		return;

	if (_inputChannelPtrs.empty() || _outputChannelPtrs.empty())
		return;

	CopyMultiToInputBuffers(channelBufs, numChannels, numSamples,
		_inputChannelPtrs.data(), _inputChannels);

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);

	CopyOutputToMulti(_outputChannelPtrs.data(), _outputChannels,
		numSamples, channelBufs, numChannels);
#else
	(void)channelBufs; (void)numChannels; (void)numSamples;
#endif
}

bool Vst2Plugin::OpenEditor(HWND parentHwnd)
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded || !_effect)
		return false;

	if (!(_effect->flags & effFlagsHasEditor))
	{
		std::cout << "[Vst2Plugin] OpenEditor: plugin has no editor" << std::endl;
		return false;
	}

	_effect->dispatcher(_effect, effEditOpen, 0, 0,
		reinterpret_cast<void*>(parentHwnd), 0.0f);

	// Query the editor rect to populate _editorSize.
	ERect* eRect = nullptr;
	_effect->dispatcher(_effect, effEditGetRect, 0, 0, &eRect, 0.0f);
	if (eRect)
	{
		const auto w = static_cast<int>(eRect->right) - static_cast<int>(eRect->left);
		const auto h = static_cast<int>(eRect->bottom) - static_cast<int>(eRect->top);
		_editorSize = {
			static_cast<unsigned int>((std::max)(0, w)),
			static_cast<unsigned int>((std::max)(0, h))
		};
	}

	std::cout << "[Vst2Plugin] OpenEditor: size="
		<< _editorSize.Width << "x" << _editorSize.Height << std::endl;
	return true;
#else
	(void)parentHwnd;
	return false;
#endif
}

void Vst2Plugin::CloseEditor()
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect && (_effect->flags & effFlagsHasEditor))
		_effect->dispatcher(_effect, effEditClose, 0, 0, nullptr, 0.0f);
#endif
	_editorSize = { 0, 0 };
}

void Vst2Plugin::IdleEditor() noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect && (_effect->flags & effFlagsHasEditor))
		_effect->dispatcher(_effect, effEditIdle, 0, 0, nullptr, 0.0f);
#endif
}

#ifdef JAMMA_VST2_ENABLED
VstIntPtr __cdecl Vst2Plugin::HostCallback(AEffect* effect,
	VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float /*opt*/)
{
	(void)index; (void)value; (void)ptr;

	switch (opcode)
	{
	case audioMasterVersion:
		return kVstVersion;
	case audioMasterGetVendorString:
		if (ptr)
			vst_strncpy(static_cast<char*>(ptr), "Jamma", kVstMaxVendorStrLen);
		return 1;
	case audioMasterGetProductString:
		if (ptr)
			vst_strncpy(static_cast<char*>(ptr), "Jamma", kVstMaxProductStrLen);
		return 1;
	case audioMasterGetVendorVersion:
		return 1000;
	case audioMasterAutomate:
		// Parameter automation notification — no action needed for a basic host.
		return 0;
	case audioMasterIdle:
		// Called by some older plugins requesting idle processing.
		if (effect)
		{
			// Dispatch effEditIdle back to any open editor.
			effect->dispatcher(effect, effEditIdle, 0, 0, nullptr, 0.0f);
		}
		return 0;
	default:
		break;
	}

	return 0;
}
#endif
