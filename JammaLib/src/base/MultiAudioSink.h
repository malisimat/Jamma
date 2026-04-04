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
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_SINK; }

		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source)
		{
			for (auto chan = 0u; chan < NumInputChannels(source); chan++)
			{
				const auto& channel = _InputChannel(chan, source);
				if (channel)
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
			for (auto chan = 0u; chan < NumInputChannels(source); chan++)
			{
				const auto& channel = _InputChannel(chan, source);
				if (channel)
					channel->EndWrite(numSamps, updateIndex);
			}
		}

		// Block-level write to a specific channel.
		// Routes to the channel sink's OnBlockWrite.
		virtual void OnBlockWriteChannel(unsigned int channel,
			const AudioWriteRequest& request,
			int writeOffset)
		{
			const auto& chan = _InputChannel(channel, request.source);

			if (chan)
				chan->OnBlockWrite(request, writeOffset);
		}

		virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const { return 0; };

		std::shared_ptr<MultiAudioSink> shared_from_this()
		{
			return std::dynamic_pointer_cast<MultiAudioSink>(
				MultiAudible::shared_from_this());
		}

	protected:
		virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) {
			return nullptr;
		}
	};
}
