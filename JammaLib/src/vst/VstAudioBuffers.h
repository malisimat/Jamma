///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <cstdint>

#ifdef JAMMA_VST3_ENABLED
#include "vst3sdk/pluginterfaces/vst/vstspeaker.h"
#endif

namespace vst
{
	enum class HostedLayoutMode
	{
		Exact,
		MonoFlexible,
	};

	struct HostedBusLayout
	{
		int32_t RequestedChannels = 1;
		int32_t InputChannels = 1;
		int32_t OutputChannels = 1;
		int32_t MaxChannels = 1;
		HostedLayoutMode Mode = HostedLayoutMode::Exact;
	};

	inline HostedBusLayout ResolveHostedBusLayout(HostedLayoutMode mode,
		unsigned int requestedChannels,
		int32_t inputBusChannels,
		int32_t outputBusChannels) noexcept
	{
		const auto requested = (std::max)(int32_t{ 1 }, static_cast<int32_t>(requestedChannels));
		if (mode == HostedLayoutMode::Exact)
			return HostedBusLayout{ requested, requested, requested, requested, mode };

		auto inputChannels = inputBusChannels > 0
			? inputBusChannels
			: requested;
		auto outputChannels = outputBusChannels > 0
			? outputBusChannels
			: inputChannels;
		auto maxChannels = (std::max)(inputChannels, outputChannels);

		return HostedBusLayout{ requested, inputChannels, outputChannels, maxChannels, mode };
	}

	inline bool IsHostedLayoutCompatible(const HostedBusLayout& layout,
		int32_t actualInputChannels,
		int32_t actualOutputChannels) noexcept
	{
		if ((actualInputChannels <= 0) || (actualOutputChannels <= 0))
			return false;

		if (layout.Mode == HostedLayoutMode::Exact)
		{
			// Accept the plugin's natural layout even when it is narrower than
			// what the station requested. ProcessBlockMulti will route only
			// the matching channels through the plugin and pass through the
			// remaining host channels untouched. This lets, for example, a
			// stereo plugin sit on an 8-channel station instead of failing to
			// load outright.
			return (actualInputChannels <= layout.RequestedChannels)
				&& (actualOutputChannels <= layout.RequestedChannels);
		}

		return true;
	}

#ifdef JAMMA_VST3_ENABLED
	inline bool TryGetSpeakerArrangementForChannelCount(int32_t channelCount,
		Steinberg::Vst::SpeakerArrangement& arrangement) noexcept
	{
		using namespace Steinberg::Vst;

		switch (channelCount)
		{
		case 1: arrangement = SpeakerArr::kMono; return true;
		case 2: arrangement = SpeakerArr::kStereo; return true;
		case 3: arrangement = SpeakerArr::k30Music; return true;
		case 4: arrangement = SpeakerArr::k40Music; return true;
		case 5: arrangement = SpeakerArr::k50; return true;
		case 6: arrangement = SpeakerArr::k51; return true;
		case 7: arrangement = SpeakerArr::k70Music; return true;
		case 8: arrangement = SpeakerArr::k71Music; return true;
		default:
			return false;
		}
	}
#endif

	inline void CopyMonoToInputBuffers(const float* monoInput,
		int32_t numSamples,
		int32_t inputChannels,
		float* const* inputChannelBuffers) noexcept
	{
		if (!monoInput || !inputChannelBuffers || numSamples <= 0 || inputChannels <= 0)
			return;

		for (int32_t channel = 0; channel < inputChannels; ++channel)
			std::copy(monoInput, monoInput + numSamples, inputChannelBuffers[channel]);
	}

	inline void CopyMultiToInputBuffers(float* const* sourceChannelBuffers,
		int32_t sourceChannels,
		int32_t numSamples,
		float* const* inputChannelBuffers,
		int32_t inputChannels) noexcept
	{
		if (!sourceChannelBuffers || !inputChannelBuffers || numSamples <= 0
			|| sourceChannels <= 0 || inputChannels <= 0)
			return;

		for (int32_t channel = 0; channel < inputChannels; ++channel)
		{
			if (channel < sourceChannels)
				std::copy(sourceChannelBuffers[channel], sourceChannelBuffers[channel] + numSamples, inputChannelBuffers[channel]);
			else
				std::fill(inputChannelBuffers[channel], inputChannelBuffers[channel] + numSamples, 0.0f);
		}
	}

	inline void FoldOutputToMono(float* const* outputChannelBuffers,
		int32_t outputChannels,
		int32_t numSamples,
		float* monoOutput) noexcept
	{
		if (!outputChannelBuffers || !monoOutput || numSamples <= 0 || outputChannels <= 0)
			return;

		const auto scale = 1.0f / static_cast<float>(outputChannels);
		for (int32_t sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
		{
			auto mixed = 0.0f;
			for (int32_t channel = 0; channel < outputChannels; ++channel)
				mixed += outputChannelBuffers[channel][sampleIndex];
			monoOutput[sampleIndex] = mixed * scale;
		}
	}

	inline void CopyOutputToMulti(float* const* outputChannelBuffers,
		int32_t outputChannels,
		int32_t numSamples,
		float* const* destinationChannelBuffers,
		int32_t destinationChannels) noexcept
	{
		if (!outputChannelBuffers || !destinationChannelBuffers || numSamples <= 0
			|| outputChannels <= 0 || destinationChannels <= 0)
			return;

		// Copy plugin outputs into matching destination channels.
		// Destination channels beyond the plugin's output width are left
		// untouched so the caller can pass them through unchanged (e.g. a
		// stereo plugin on an 8-channel station leaves channels 2..7 alone).
		const auto copyChannels = (std::min)(outputChannels, destinationChannels);
		for (int32_t channel = 0; channel < copyChannels; ++channel)
			std::copy(outputChannelBuffers[channel], outputChannelBuffers[channel] + numSamples, destinationChannelBuffers[channel]);
	}
}