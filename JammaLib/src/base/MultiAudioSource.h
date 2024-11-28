#pragma once

#include <vector>
#include "AudioSource.h"
#include "AudioSink.h"
#include "MultiAudible.h"
#include "MultiAudioSink.h"

namespace engine
{
	class Trigger;
}

namespace base
{
	class MultiAudioSource :
		public virtual MultiAudible
	{
	public:
		MultiAudioSource() :
			_sourceParams(AudioSourceParams())
		{
		}
		MultiAudioSource(AudioSourceParams params) :
			_sourceParams(params)
		{
		}
		~MultiAudioSource() {}

	public:
		virtual MultiAudioDirection MultiAudibleDirection() const override { return MULTIAUDIO_SOURCE; }
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<engine::Trigger> trigger,
			int indexOffset,
			unsigned int numSamps)
		{
			for (unsigned int chan = 0; chan < NumOutputChannels(); chan++)
			{
				auto channel = OutputChannel(chan);
				dest->OnWriteChannel(chan, channel, indexOffset, numSamps);
			}
		}
		virtual void EndMultiPlay(unsigned int numSamps)
		{
			for (auto chan = 0u; chan < NumOutputChannels(); chan++)
			{
				auto channel = OutputChannel(chan);
				channel->EndPlay(numSamps);
			}
		}
		virtual void OnPlayChannel(unsigned int channel,
			const std::shared_ptr<base::AudioSink> dest,
			int indexOffset,
			unsigned int numSamps)
		{
			auto chan = OutputChannel(channel);
			if (chan)
				chan->OnPlay(dest, indexOffset, numSamps);
		}
		virtual unsigned int NumOutputChannels() const { return 0; };

		Audible::AudioSourceType SourceType() const { return _sourceParams.SourceType; }
		void SetSourceType(Audible::AudioSourceType source) { _sourceParams.SourceType = source; }

		std::shared_ptr<MultiAudioSource> shared_from_this()
		{
			return std::dynamic_pointer_cast<MultiAudioSource>(
				Sharable::shared_from_this());
		}

	protected:
		virtual const std::shared_ptr<AudioSource> OutputChannel(unsigned int channel)
		{
			auto chan = std::shared_ptr<AudioSource>();
			chan->SetSourceType(SourceType());
			return chan;
		}

	protected:
		AudioSourceParams _sourceParams;
	};
}
