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
		virtual MultiAudioPlugType MultiAudiblePlug() const { return MULTIAUDIOPLUG_SOURCE; }
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<engine::Trigger> trigger,
			int indexOffset,
			unsigned int numSamps)
		{
			for (unsigned int chan = 0; chan < NumOutputChannels(); chan++)
			{
				auto channel = OutputChannel(chan);
				dest->OnWriteChannel(chan,
					channel,
					indexOffset,
					numSamps,
					_sourceParams.SourceType);
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
		virtual void OnPlayChannel(unsigned int chan,
			const std::shared_ptr<base::AudioSink> dest,
			int indexOffset,
			unsigned int numSamps)
		{
			auto channel = OutputChannel(chan);
			if (channel)
				channel->OnPlay(dest, indexOffset, numSamps);
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
			return nullptr;
		}

	protected:
		AudioSourceParams _sourceParams;
	};
}
