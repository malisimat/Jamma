#pragma once

#include <vector>
#include <memory>
#include <iostream>
#include "../include/Constants.h"
#include "AudioSource.h"

namespace audio
{
	class AudioBuffer :
		public virtual base::AudioSink,
		public virtual base::AudioSource
	{
	public:
		AudioBuffer();
		AudioBuffer(unsigned int size);
		~AudioBuffer();

	public:
		virtual AudioPlugType AudioPlug() const override { return AUDIOPLUG_BOTH; }
		virtual void OnPlay(const std::shared_ptr<base::AudioSink> dest,
			int indexOffset,
			unsigned int numSamps) override;
		virtual void EndPlay(unsigned int numSamps) override;
		inline virtual int OnMixWrite(float samp,
			float fadeCurrent,
			float fadeNew,
			int indexOffset,
			Audible::AudioSourceType source) override;
		virtual void EndWrite(unsigned int numSamps, bool updateIndex) override;

		void SetSize(unsigned int size);
		unsigned int SampsRecorded() const;
		unsigned int BufSize() const;

		std::vector<float>::iterator Start();
		std::vector<float>::iterator End();
		std::vector<float>::iterator Delay(unsigned int sampsDelay);

	protected:
		void _SetWriteIndex(unsigned int index);

	protected:
		unsigned int _sampsRecorded;
		unsigned long _playIndex;
		std::vector<float> _buffer;
	};
}
