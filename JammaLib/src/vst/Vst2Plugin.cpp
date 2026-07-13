///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst2Plugin.h"
#include "../midi/MidiBlockTiming.h"
#include <algorithm>
#include <cstring>
#include <float.h>
#include <iostream>
#include "../graphics/VstEditorWindow.h"

using namespace vst;

Vst2Plugin::GlContextScope::GlContextScope() noexcept
	: _rc(wglGetCurrentContext()), _dc(wglGetCurrentDC())
{
}

Vst2Plugin::GlContextScope::~GlContextScope()
{
	// Only restore if there was a context to begin with (non-render threads,
	// e.g. the job-thread fallback, have none — leave them untouched).
	if (_rc)
		wglMakeCurrent(_dc, _rc);
}

Vst2Plugin::Vst2Plugin() :
#ifdef JAMMA_VST2_ENABLED
	_effect(nullptr),
	_midiEvents(),
	_midiEventBlock{},
	_midiBlockStartSample(0u),
	_midiBlockNumSamples(0u),
	_midiEventCount(0u),
#endif
	_moduleHandle(nullptr),
	_sampleRate(44100.0f),
	_blockSize(512u),
	_sampleFramePosition(0),
	_isLoaded(false),
	_isActivated(false),
	_audioThreadId(0),
	_name(),
	_isBypassed(false),
	_editorSize({ 0, 0 }),
	_editorParentHwnd(nullptr),
	_isEditorOpen(false),
	_inputChannelPtrs(),
	_outputChannelPtrs(),
	_inputScratchStorage(),
	_outputScratchStorage(),
	_inputChannels(0),
	_outputChannels(0),
	_requestedChannels(1)
{
#ifdef JAMMA_VST2_ENABLED
	for (auto i = 0u; i < MaxMidiEventsPerBlock; ++i)
		_midiEventBlock.events[i] = reinterpret_cast<VstEvent*>(&_midiEvents[i]);
#endif
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

	_moduleHandle = LoadLibraryW(path.c_str());
	if (!_moduleHandle)
	{
		std::cerr << "[Vst2Plugin] PreInit: LoadLibraryW failed: " << GetLastError() << std::endl;
		return false;
	}

	// Instantiate the AEffect and run effOpen HERE, on the UI thread. minihost
	// calls VSTPluginMain()/effOpen and effEditOpen on the same (main) thread.
	// Several plugins (e.g. NI) bind their GUI/idle dispatch to the thread that
	// instantiated the effect; if effOpen runs on the job thread and effEditOpen
	// on the UI thread, the editor paints once then freezes and ignores input.
	// Doing it in PreInit keeps both on the UI thread.
	if (!_InstantiateEffect(path))
	{
		FreeLibrary(_moduleHandle);
		_moduleHandle = nullptr;
		return false;
	}

	return true;
#else
	(void)path;
	return false;
#endif
}

bool Vst2Plugin::_InstantiateEffect(const std::wstring& path)
{
#ifdef JAMMA_VST2_ENABLED
	(void)path;

	if (_effect)
		return true; // Already instantiated

	// This runs on Jamma's UI thread, which also owns the OpenGL render
	// context. Some plugins (e.g. Battery 4) make their own GL context current
	// during VSTPluginMain()/effOpen and never restore ours, which leaves our
	// framebuffer incomplete and the whole app paints white. The scope guard
	// snapshots our GL context now and restores it when this function returns.
	GlContextScope glScope;

	// Locate and call the plugin entry-point to obtain the AEffect.
	auto mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
		GetProcAddress(_moduleHandle, "VSTPluginMain"));
	if (!mainProc)
		mainProc = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(
			GetProcAddress(_moduleHandle, "main"));

	if (!mainProc)
	{
		std::cerr << "[Vst2Plugin] InstantiateEffect: no VST2 entry-point found" << std::endl;
		return false;
	}

	_effect = mainProc(Vst2Plugin::HostCallback);
	if (!_effect || (_effect->magic != kEffectMagic))
	{
		std::cerr << "[Vst2Plugin] InstantiateEffect: VSTPluginMain returned null or wrong magic" << std::endl;
		_effect = nullptr;
		return false;
	}

	// The plugin may call back into HostCallback during this bootstrap sequence.
	// Keep resvd2 null until construction dispatches are finished, then wire it.
	_effect->resvd2 = 0;

	_effect->dispatcher(_effect, effIdentify, 0, 0, nullptr, 0.0f);
	_effect->dispatcher(_effect, effSetSampleRate, 0, 0, nullptr, _sampleRate);
	_effect->dispatcher(_effect, effSetBlockSize, 0,
		static_cast<VstIntPtr>(_blockSize), nullptr, 0.0f);

	// Open the effect. Audio configuration (sample rate, block size, resume)
	// is deferred to Load() once the host format is known.
	_effect->dispatcher(_effect, effOpen, 0, 0, nullptr, 0.0f);

	// Query name after effOpen.
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

	_outputChannels = (std::max)(1, static_cast<int32_t>(_effect->numOutputs));
	_inputChannels = (std::max)((std::max)(1, static_cast<int32_t>(_effect->numInputs)), _outputChannels);

	// Pre-allocate channel pointer arrays and scratch buffers so ProcessBlock
	// never touches the heap.
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

	// Host-owned instance pointer for HostCallback. Do not use effect->user;
	// plugins may use that slot for their own state.
	_effect->resvd2 = reinterpret_cast<VstIntPtr>(this);

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

	_requestedChannels = static_cast<int32_t>((std::max)(1u, numChannels));
	_sampleRate = sampleRate;
	_blockSize = blockSize;
	_sampleFramePosition.store(0, std::memory_order_relaxed);

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

	// 2. Instantiate + effOpen. PreInit() normally does this on the UI thread so
	//    the editor (effEditOpen on the UI thread) shares the effect's thread.
	//    Only instantiate here as a fallback if PreInit() was skipped — note
	//    this fallback path runs on the job thread, so editor repaint/input may
	//    misbehave for plugins that bind GUI dispatch to the instantiating
	//    thread. PreInit() is the supported path.
	if (!_effect)
	{
		std::cerr << "[Vst2Plugin] Load: no pre-instantiated effect — instantiating on job thread (fallback)" << std::endl;
		if (!_InstantiateEffect(path))
		{
			FreeLibrary(_moduleHandle);
			_moduleHandle = nullptr;
			return false;
		}
	}

	// 3. Configure for the host audio format (safe off the editor thread).
	_effect->dispatcher(_effect, effSetSampleRate, 0, 0, nullptr, sampleRate);
	_effect->dispatcher(_effect, effSetBlockSize, 0,
		static_cast<VstIntPtr>(blockSize), nullptr, 0.0f);
	_effect->dispatcher(_effect, effSetProgram, 0, 0, nullptr, 0.0f);
	_effect->dispatcher(_effect, effGetPlugCategory, 0, 0, nullptr, 0.0f);

	{
		static const char kReceiveVstEvents[] = "receiveVstEvents";
		static const char kReceiveVstMidiEvent[] = "receiveVstMidiEvent";
		_effect->dispatcher(_effect, effCanDo, 0, 0,
			const_cast<char*>(kReceiveVstEvents), 0.0f);
		_effect->dispatcher(_effect, effCanDo, 0, 0,
			const_cast<char*>(kReceiveVstMidiEvent), 0.0f);
	}

	for (int32_t input = 0; input < _effect->numInputs; ++input)
		_effect->dispatcher(_effect, effConnectInput, input, 1, nullptr, 0.0f);

	for (int32_t output = 0; output < _effect->numOutputs; ++output)
		_effect->dispatcher(_effect, effConnectOutput, output, 1, nullptr, 0.0f);

	VstSpeakerArrangement mono = {};
	mono.type = kSpeakerArrMono;
	mono.numChannels = 1;
	mono.speakers[0].type = kSpeakerM;

	VstSpeakerArrangement stereo = {};
	stereo.type = kSpeakerArrStereo;
	stereo.numChannels = 2;
	stereo.speakers[0].type = kSpeakerL;
	stereo.speakers[1].type = kSpeakerR;

	auto* in = (_effect->numInputs == 1) ? &mono
		: (_effect->numInputs == 2) ? &stereo : nullptr;
	auto* out = (_effect->numOutputs == 1) ? &mono
		: (_effect->numOutputs == 2) ? &stereo : nullptr;
	if (in && out)
		_effect->dispatcher(_effect, effSetSpeakerArrangement, 0,
			reinterpret_cast<VstIntPtr>(in), out, 0.0f);

	if ((_effect->flags & effFlagsCanDoubleReplacing) != 0)
	{
		_effect->dispatcher(_effect, effSetProcessPrecision, 0,
			static_cast<VstIntPtr>(kVstProcessPrecision32), nullptr, 0.0f);
	}

	// 4. Start the effect (resume).
	_effect->dispatcher(_effect, effMainsChanged, 0, 1, nullptr, 0.0f);
	_effect->dispatcher(_effect, effStartProcess, 0, 0, nullptr, 0.0f);
	_isActivated.store(true, std::memory_order_release);

	_isLoaded = true;
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
		{
			_effect->dispatcher(_effect, effStopProcess, 0, 0, nullptr, 0.0f);
			_effect->dispatcher(_effect, effMainsChanged, 0, 0, nullptr, 0.0f);
		}
	}

	// Null out pointers before clearing storage so any concurrent
	// ProcessBlock (guarded by _isActivated) cannot chase stale pointers.
	_inputChannelPtrs.clear();
	_outputChannelPtrs.clear();
	_inputScratchStorage.clear();
	_outputScratchStorage.clear();

	if (_effect)
	{
		_effect->resvd2 = 0;
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
	_editorParentHwnd.store(nullptr, std::memory_order_release);
	_isEditorOpen.store(false, std::memory_order_release);
	_inputChannels = 0;
	_outputChannels = 0;
}

void Vst2Plugin::ProcessBlock(float* monoBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	_audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

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

	DispatchPendingMidiEvents();

	for (int32_t channel = 0; channel < _outputChannels; ++channel)
		std::fill(_outputChannelPtrs[static_cast<size_t>(channel)],
			_outputChannelPtrs[static_cast<size_t>(channel)] + numSamples,
			0.0f);

	_clearfp();

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);
	_midiEventCount = 0u;
	_midiEventBlock.numEvents = 0;

	_sampleFramePosition.fetch_add(numSamples, std::memory_order_relaxed);

	FoldOutputToMono(_outputChannelPtrs.data(), _outputChannels,
		numSamples, monoBuf);
#else
	(void)monoBuf; (void)numSamples;
#endif
}

void Vst2Plugin::ProcessBlockStereo(float* leftBuf, float* rightBuf, int32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	_audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

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

	DispatchPendingMidiEvents();

	for (int32_t channel = 0; channel < _outputChannels; ++channel)
		std::fill(_outputChannelPtrs[static_cast<size_t>(channel)],
			_outputChannelPtrs[static_cast<size_t>(channel)] + numSamples,
			0.0f);

	_clearfp();

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);
	_midiEventCount = 0u;
	_midiEventBlock.numEvents = 0;

	_sampleFramePosition.fetch_add(numSamples, std::memory_order_relaxed);

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
	_audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

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

	DispatchPendingMidiEvents();

	for (int32_t channel = 0; channel < _outputChannels; ++channel)
		std::fill(_outputChannelPtrs[static_cast<size_t>(channel)],
			_outputChannelPtrs[static_cast<size_t>(channel)] + numSamples,
			0.0f);

	_clearfp();

	_effect->processReplacing(_effect,
		_inputChannelPtrs.data(),
		_outputChannelPtrs.data(),
		numSamples);
	_midiEventCount = 0u;
	_midiEventBlock.numEvents = 0;

	_sampleFramePosition.fetch_add(numSamples, std::memory_order_relaxed);

	CopyOutputToMulti(_outputChannelPtrs.data(), _outputChannels,
		numSamples, channelBufs, numChannels);
#else
	(void)channelBufs; (void)numChannels; (void)numSamples;
#endif
}

void Vst2Plugin::SetParameter(unsigned int index, float value) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect && _effect->setParameter)
		_effect->setParameter(_effect, static_cast<VstInt32>(index), value);
#else
	(void)index; (void)value;
#endif
}

float Vst2Plugin::GetParameter(unsigned int index) const noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect && _effect->getParameter)
		return _effect->getParameter(_effect, static_cast<VstInt32>(index));
#else
	(void)index;
#endif
	return 0.0f;
}

void Vst2Plugin::BeginMidiBlock(std::uint32_t blockStartSample,
	std::uint32_t numSamples) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	_midiBlockStartSample = blockStartSample;
	_midiBlockNumSamples = numSamples;
	_midiEventCount = 0u;
	_midiEventBlock.numEvents = 0;
	_midiEventBlock.reserved = 0;
#else
	(void)blockStartSample; (void)numSamples;
#endif
}

void Vst2Plugin::UpdateHostTime(const HostTimeState& state) noexcept
{
	_hostTime = state;
}

void Vst2Plugin::SendMidiEvent(const midi::MidiEvent& event,
	bool isRealtime) noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (_midiEventCount >= MaxMidiEventsPerBlock || 0u == _midiBlockNumSamples)
		return;

	const auto position = midi::ClassifyMidiSampleInBlock(event.sampleOffset,
		_midiBlockStartSample, _midiBlockNumSamples);
	const bool inWindow = position == midi::MidiBlockSamplePosition::Due;
	if (!inWindow && !isRealtime)
		return;

	auto& midiEvent = _midiEvents[_midiEventCount];
	midiEvent = {};
	midiEvent.type = kVstMidiType;
	midiEvent.byteSize = sizeof(VstMidiEvent);
	{
		const auto sampleDelta = static_cast<int64_t>(midi::MidiSampleDelta(event.sampleOffset,
			_midiBlockStartSample));
		const auto maxDelta = (_midiBlockNumSamples > 0u)
			? static_cast<int64_t>(_midiBlockNumSamples - 1u)
			: 0;
		auto clampedDelta = sampleDelta;
		if (clampedDelta < 0)
			clampedDelta = 0;
		if (clampedDelta > maxDelta)
			clampedDelta = maxDelta;
		midiEvent.deltaFrames = static_cast<VstInt32>(clampedDelta);
	}
	midiEvent.flags = 0;
	midiEvent.midiData[0] = static_cast<char>(event.status);
	midiEvent.midiData[1] = static_cast<char>(event.data1);
	midiEvent.midiData[2] = static_cast<char>(event.data2);
	midiEvent.midiData[3] = 0;

	++_midiEventCount;
	_midiEventBlock.numEvents = static_cast<VstInt32>(_midiEventCount);
#else
	(void)event; (void)isRealtime;
#endif
}

#ifdef JAMMA_VST2_ENABLED
void Vst2Plugin::DispatchPendingMidiEvents() noexcept
{
	if (_midiEventCount == 0u || !_effect)
		return;

	_effect->dispatcher(_effect, effProcessEvents, 0, 0, &_midiEventBlock, 0.0f);
}
#endif

bool Vst2Plugin::OpenEditor(HWND parentHwnd)
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded || !_effect)
		return false;

	if (!(_effect->flags & effFlagsHasEditor))
	{
		std::cerr << "[Vst2Plugin] OpenEditor: plugin has no editor" << std::endl;
		return false;
	}

	if (_isEditorOpen.load(std::memory_order_acquire))
		return true;

	// Restore our GL render context after the plugin builds its editor — some
	// plugins make their own context current during effEditOpen.
	GlContextScope glScope;

	_effect->dispatcher(_effect, effEditOpen, 0, 0,
		reinterpret_cast<void*>(parentHwnd), 0.0f);
	_editorParentHwnd.store(parentHwnd, std::memory_order_release);
	_isEditorOpen.store(true, std::memory_order_release);

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

	return true;
#else
	(void)parentHwnd;
	return false;
#endif
}

void Vst2Plugin::CloseEditor()
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect
		&& (_effect->flags & effFlagsHasEditor)
		&& _isEditorOpen.exchange(false, std::memory_order_acq_rel))
	{
		GlContextScope glScope;
		_effect->dispatcher(_effect, effEditClose, 0, 0, nullptr, 0.0f);
	}
#endif
	_editorSize = { 0, 0 };
	_editorParentHwnd.store(nullptr, std::memory_order_release);
}

void Vst2Plugin::IdleEditor() noexcept
{
#ifdef JAMMA_VST2_ENABLED
	if (_effect
		&& (_effect->flags & effFlagsHasEditor)
		&& _isEditorOpen.load(std::memory_order_acquire))
	{
		// effEditIdle fires from the UI/render thread; restore our GL context
		// afterwards so the next frame's framebuffer stays complete.
		GlContextScope glScope;
		_effect->dispatcher(_effect, effEditIdle, 0, 0, nullptr, 0.0f);
	}
#endif
}

#ifdef JAMMA_VST2_ENABLED
VstIntPtr __cdecl Vst2Plugin::HostCallback(AEffect* effect,
	VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
	auto* self = (effect && effect->resvd2)
		? reinterpret_cast<Vst2Plugin*>(effect->resvd2)
		: nullptr;

	// During VSTPluginMain and construction-time dispatches, resvd2 is not wired
	// yet. Return stable bootstrap answers until the host instance is available.
	if (!self)
	{
		switch (opcode)
		{
		case audioMasterVersion:
			return kVstVersion;
		case audioMasterGetSampleRate:
			return 44100;
		case audioMasterGetBlockSize:
			return 512;
		case audioMasterGetAutomationState:
			return kVstAutomationRead;
		case audioMasterCanDo:
			if (ptr && SupportsHostCanDo(static_cast<const char*>(ptr)))
				return 1;
			return 0;
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
		default:
			return 0;
		}
	}

	switch (opcode)
	{
	case audioMasterVersion:
		return kVstVersion;
	case audioMasterGetSampleRate:
		return static_cast<VstIntPtr>(self->_sampleRate);
	case audioMasterGetBlockSize:
		return static_cast<VstIntPtr>(self->_blockSize);
	case audioMasterGetAutomationState:
		return kVstAutomationRead;
	case audioMasterGetTime:
		if (self)
		{
			auto& ti = self->_timeInfo;
			ti = {};
			ti.samplePos  = self->_hostTime.samplePos;
			ti.sampleRate = self->_hostTime.sampleRate;
			ti.tempo      = self->_hostTime.tempo;
			ti.timeSigNumerator   = self->_hostTime.bpi;
			ti.timeSigDenominator = 4;
			ti.ppqPos = (ti.sampleRate > 0.0)
				? (ti.samplePos / ti.sampleRate) * (ti.tempo / 60.0)
				: 0.0;
			ti.flags = kVstPpqPosValid | kVstTempoValid | kVstTimeSigValid;
			if (self->_hostTime.isPlaying)
				ti.flags |= kVstTransportPlaying;
			return reinterpret_cast<VstIntPtr>(&ti);
		}
		return 0;
	case audioMasterGetCurrentProcessLevel:
		// Report realtime ONLY when the plugin queries from the audio thread.
		// On the UI/editor thread we must report "user", otherwise many plugins
		// assume an audio-callback context and refuse to repaint their editor.
		{
			const DWORD audioTid = self->_audioThreadId.load(std::memory_order_relaxed);
			if (audioTid != 0 && GetCurrentThreadId() == audioTid)
				return kVstProcessLevelRealtime;
			return kVstProcessLevelUser;
		}
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
	case audioMasterCanDo:
		if (ptr)
		{
			const auto canDo = static_cast<const char*>(ptr);
			if (SupportsHostCanDo(canDo))
				return 1;
		}
		return 0;
	case audioMasterGetDirectory:
		if (self->_moduleHandle)
		{
			static thread_local char moduleDir[MAX_PATH] = {};
			const auto len = GetModuleFileNameA(self->_moduleHandle, moduleDir, MAX_PATH);
			if (len > 0 && len < MAX_PATH)
			{
				for (auto i = len; i > 0; --i)
				{
					if (moduleDir[i - 1] == '\\' || moduleDir[i - 1] == '/')
					{
						moduleDir[i - 1] = '\0';
						break;
					}
				}
				return reinterpret_cast<VstIntPtr>(moduleDir);
			}
		}
		return 0;
	case audioMasterAutomate:
		// Parameter automation notification: record the most recently touched
		// parameter so the UI thread can wire it to a MIDI automation lane and the
		// MIDI pump can record live editor-driven automation. Publish the triple
		// first, then bump Sequence (release) so consumers see a coherent event.
		// Do not feed the value back through setParameter here: the plugin already
		// changed its own parameter state before calling audioMasterAutomate.
		PublishLastTouchedParameter(self, static_cast<unsigned int>(index), opt);
		return 0;
	case audioMasterBeginEdit:
	case audioMasterEndEdit:
		// Bracket a parameter gesture. Acknowledge only — the actual value flows
		// through audioMasterAutomate; publishing here (with no value supplied)
		// would clobber the real automation value with a spurious one.
		return 1;
	case audioMasterUpdateDisplay:
		if (self->_isEditorOpen.load(std::memory_order_acquire))
		{
			const HWND parentHwnd = self->_editorParentHwnd.load(std::memory_order_acquire);
			if (parentHwnd && IsWindow(parentHwnd))
			{
				PostMessage(parentHwnd, graphics::VstEditorWindow::MessageVst2Idle, 0, 0);
				return 1;
			}
		}
		return 0;
	case audioMasterSizeWindow:
		if (self->_isEditorOpen.load(std::memory_order_acquire))
		{
			const HWND parentHwnd = self->_editorParentHwnd.load(std::memory_order_acquire);
			if (parentHwnd && IsWindow(parentHwnd))
			{
				const auto requestedWidth = static_cast<WPARAM>((std::max)(0, static_cast<int>(index)));
				const auto requestedHeight = static_cast<LPARAM>((std::max)(0, static_cast<int>(value)));
				const auto postResult = PostMessage(parentHwnd,
					graphics::VstEditorWindow::MessageVst2SizeWindow,
					requestedWidth,
					requestedHeight);
				return postResult ? 1 : 0;
			}
		}
		return 0;
	case audioMasterIdle:
		// Called by some older plugins requesting idle processing.
		if (self->_isEditorOpen.load(std::memory_order_acquire))
		{
			const HWND parentHwnd = self->_editorParentHwnd.load(std::memory_order_acquire);
			if (parentHwnd && IsWindow(parentHwnd))
			{
				PostMessage(parentHwnd, graphics::VstEditorWindow::MessageVst2Idle, 0, 0);
				return 0;
			}
		}
		return 0;
	default:
		break;
	}

	return 0;
}
#endif

// ---------------------------------------------------------------------------
// State save / restore  (non-RT, job/UI thread only)
// ---------------------------------------------------------------------------
//
// Blob layout:
//   Byte 0   : format version (0x01)
//   Byte 1   : type flag — 0 = param array, 1 = opaque VST2 chunk
//   Bytes 2-5: payload size  (uint32, little-endian)
//   Bytes 6+ : payload
//
// For the param path every parameter is stored as a little-endian float32.
// For the chunk path the raw bytes returned by effGetChunk are stored as-is.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> Vst2Plugin::GetState() const
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded || !_effect)
		return {};

	// --- Determine which path to use ----------------------------------
	const bool supportsChunks = (_effect->flags & effFlagsProgramChunks) != 0;

	std::vector<std::uint8_t> payload;
	std::uint8_t typeFlag = 0;

	if (supportsChunks)
	{
		// Path 1: opaque chunk (bank-level, index = 0)
		void* chunkPtr = nullptr;
		const auto chunkSize = _effect->dispatcher(
			_effect, effGetChunk, /*bank=*/0, 0, &chunkPtr, 0.0f);

		if (chunkSize > 0 && chunkPtr)
		{
			typeFlag = 1;
			const auto* bytes = static_cast<const std::uint8_t*>(chunkPtr);
			payload.assign(bytes, bytes + static_cast<size_t>(chunkSize));
		}
		else
		{
			// Plugin reported chunk support but returned nothing — bail out.
			return {};
		}
	}
	else
	{
		// Path 2: parameter array
		const int numParams = _effect->numParams;
		if (numParams <= 0)
			return {};

		typeFlag = 0;
		payload.resize(static_cast<size_t>(numParams) * 4u);

		for (int i = 0; i < numParams; ++i)
		{
			const float v = _effect->getParameter(_effect, i);
			std::uint8_t bytes[4];
			std::memcpy(bytes, &v, 4);
			payload[static_cast<size_t>(i) * 4u + 0] = bytes[0];
			payload[static_cast<size_t>(i) * 4u + 1] = bytes[1];
			payload[static_cast<size_t>(i) * 4u + 2] = bytes[2];
			payload[static_cast<size_t>(i) * 4u + 3] = bytes[3];
		}
	}

	// --- Pack blob --------------------------------------------------------
	const auto payloadSize = static_cast<std::uint32_t>(payload.size());
	std::vector<std::uint8_t> blob;
	blob.reserve(6 + payload.size());
	blob.push_back(0x01);                                     // version
	blob.push_back(typeFlag);                                 // type
	blob.push_back(static_cast<std::uint8_t>(payloadSize & 0xFF));
	blob.push_back(static_cast<std::uint8_t>((payloadSize >> 8)  & 0xFF));
	blob.push_back(static_cast<std::uint8_t>((payloadSize >> 16) & 0xFF));
	blob.push_back(static_cast<std::uint8_t>((payloadSize >> 24) & 0xFF));
	blob.insert(blob.end(), payload.begin(), payload.end());

	return blob;
#else
	return {};
#endif
}

void Vst2Plugin::SetState(const std::vector<std::uint8_t>& blob)
{
#ifdef JAMMA_VST2_ENABLED
	if (!_isLoaded || !_effect)
		return;

	const std::vector<std::uint8_t>* stateBlob = &blob;

	if (stateBlob->size() < 6)
		return;

	auto readBe32 = [](const std::uint8_t* bytes) -> std::uint32_t
	{
		return (static_cast<std::uint32_t>(bytes[0]) << 24)
			| (static_cast<std::uint32_t>(bytes[1]) << 16)
			| (static_cast<std::uint32_t>(bytes[2]) << 8)
			| static_cast<std::uint32_t>(bytes[3]);
	};

	auto readLe32 = [](const std::uint8_t* bytes) -> std::uint32_t
	{
		return static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	};

	auto matchesMagic = [](const std::uint8_t* bytes, const char* magic) noexcept
	{
		return bytes[0] == static_cast<std::uint8_t>(magic[0])
			&& bytes[1] == static_cast<std::uint8_t>(magic[1])
			&& bytes[2] == static_cast<std::uint8_t>(magic[2])
			&& bytes[3] == static_cast<std::uint8_t>(magic[3]);
	};

	if (stateBlob->size() >= 160u && matchesMagic(stateBlob->data(), "CcnK"))
	{
		const auto* fxMagic = stateBlob->data() + 8u;
		const bool isBankChunk = matchesMagic(fxMagic, "FBCh");
		const bool isPresetChunk = matchesMagic(fxMagic, "FPCh");
		if (isBankChunk || isPresetChunk)
		{
			const auto chunkSizeOffset = isBankChunk ? 156u : 56u;
			const auto chunkOffset = chunkSizeOffset + 4u;
			const auto chunkSize = readBe32(stateBlob->data() + chunkSizeOffset);
			if (chunkSize > 0u && stateBlob->size() >= chunkOffset + static_cast<size_t>(chunkSize))
			{
				_effect->dispatcher(_effect, effSetChunk, isPresetChunk ? 1 : 0,
					static_cast<VstIntPtr>(chunkSize),
					const_cast<std::uint8_t*>(stateBlob->data() + chunkOffset),
					0.0f);

				if (!isPresetChunk)
				{
					const auto programCount = (std::max)(0, static_cast<int>(_effect->numPrograms));
					for (int programIndex = 0; programIndex < programCount; ++programIndex)
					{
						char indexedProgramName[kVstMaxProgNameLen + 1] = {};
						_effect->dispatcher(_effect, effGetProgramNameIndexed, programIndex, -1,
							indexedProgramName, 0.0f);
					}
				}

				char programName[kVstMaxProgNameLen + 1] = {};
				_effect->dispatcher(_effect, effGetProgramNameIndexed, 0, -1, programName, 0.0f);
				return;
			}
		}
	}

	// Unpack header
	const std::uint8_t version = (*stateBlob)[0];
	const std::uint8_t typeFlag = (*stateBlob)[1];
	const std::uint32_t payloadSize = readLe32(stateBlob->data() + 2u);

	if (version != 0x01)
	{
		std::cerr << "[Vst2Plugin] SetState: unknown blob version " << (int)version << std::endl;
		return;
	}

	if (stateBlob->size() < 6 + static_cast<size_t>(payloadSize))
	{
		std::cerr << "[Vst2Plugin] SetState: blob truncated" << std::endl;
		return;
	}

	const std::uint8_t* payload = stateBlob->data() + 6;

	if (typeFlag == 1)
	{
		// Chunk path
		_effect->dispatcher(_effect, effSetChunk, /*bank=*/0,
			static_cast<VstIntPtr>(payloadSize),
			const_cast<void*>(static_cast<const void*>(payload)),
			0.0f);

		const auto programCount = (std::max)(0, static_cast<int>(_effect->numPrograms));
		for (int programIndex = 0; programIndex < programCount; ++programIndex)
		{
			char indexedProgramName[kVstMaxProgNameLen + 1] = {};
			_effect->dispatcher(_effect, effGetProgramNameIndexed, programIndex, -1,
				indexedProgramName, 0.0f);
		}

		char programName[kVstMaxProgNameLen + 1] = {};
		_effect->dispatcher(_effect, effGetProgramNameIndexed, 0, -1, programName, 0.0f);
	}
	else
	{
		// Param path
		const int numParams = _effect->numParams;
		const auto paramCount = static_cast<int>(payloadSize / 4);
		for (int i = 0; i < (std::min)(numParams, paramCount); ++i)
		{
			float v = 0.0f;
			std::memcpy(&v, payload + static_cast<size_t>(i) * 4u, 4);
			_effect->setParameter(_effect, i, v);
		}
	}
#else
	(void)blob;
#endif
}
