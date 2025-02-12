#pragma once

#include <vector>
#include <optional>
#include "Audible.h"
#include "AudioSink.h"
#include "MultiAudible.h"

namespace base
{
	class MultiAudioSource;

	class MultiAudioSink :
		public virtual MultiAudible
	{
	public:
		virtual MultiAudioPlugType MultiAudioPlug() const
		{
			return MULTIAUDIOPLUG_SINK;
		}

		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source)
		{
			for (auto chan = 0u; chan < NumInputChannels(); chan++)
			{
				auto channel = InputChannel(chan, source);
				channel->Zero(numSamps);
			}
		}
		virtual bool IsArmed() const { return true; }
		virtual void EndMultiWrite(unsigned int numSamps,
			Audible::AudioSourceType source) { return EndMultiWrite(numSamps, false, source); }
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source)
		{
			for (auto chan = 0u; chan < NumInputChannels(); chan++)
			{
				auto channel = InputChannel(chan, source);
				channel->EndWrite(numSamps, updateIndex);
			}
		}
		virtual void OnWriteChannel(unsigned int channel,
			const std::shared_ptr<base::AudioSource> src,
			int indexOffset,
			unsigned int numSamps,
			Audible::AudioSourceType source)
		{
			auto chan = InputChannel(channel, source);
			if (chan && src)
				src->OnPlay(chan, indexOffset, numSamps);
		}
		virtual void OnMixWriteChannel(unsigned int channel,
			float samp,
			float fadeCurrent,
			float fadeNew,
			int indexOffset,
			Audible::AudioSourceType source)
		{
			auto chan = InputChannel(channel, source);
			if (chan)
				chan->OnMixWrite(samp, fadeCurrent, fadeNew, indexOffset, source);
		}
		virtual unsigned int NumInputChannels() const { return 0; };

		std::shared_ptr<MultiAudioSink> shared_from_this()
		{
			return std::dynamic_pointer_cast<MultiAudioSink>(
				MultiAudible::shared_from_this());
		}

	protected:
		virtual const std::shared_ptr<AudioSink> InputChannel(unsigned int channel,
			Audible::AudioSourceType source) {
			return nullptr;
		}
	};
}
