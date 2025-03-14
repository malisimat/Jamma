#pragma once

#include <vector>
#include <memory>
#include "Audible.h"
#include "AudioSink.h"

namespace base
{
	class AudioSourceParams
	{
	public:
		AudioSourceParams() : SourceType(Audible::AudioSourceType::AUDIOSOURCE_ADC) {}
	public:
		Audible::AudioSourceType SourceType;
	};

	class AudioSource :
		public virtual Audible
	{
	public:
		AudioSource(AudioSourceParams params) :
			_sourceParams(params)
		{
		}

	public:
		virtual AudioPlugType AudioPlug() const override { return AUDIOPLUG_SOURCE; }
		virtual void OnPlay(const std::shared_ptr<base::AudioSink> dest,
			int indexOffset,
			unsigned int numSamps) = 0;
		virtual void EndPlay(unsigned int numSamps) = 0;

		Audible::AudioSourceType SourceType() const { return _sourceParams.SourceType; }
		void SetSourceType(Audible::AudioSourceType source) { _sourceParams.SourceType = source; }
		
		std::shared_ptr<AudioSource> shared_from_this()
		{
			return std::dynamic_pointer_cast<AudioSource>(
				Audible::shared_from_this());
		}

	protected:
		AudioSourceParams _sourceParams;
	};
}
