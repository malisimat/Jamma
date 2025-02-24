#pragma once

#include "MultiAudioSource.h"
#include "MultiAudioSink.h"
#include "AudioBuffer.h"

namespace audio
{
	class ChannelMixerParams
	{
	public:
		unsigned int InputBufferSize;
		unsigned int OutputBufferSize;
		unsigned int NumInputChannels;
		unsigned int NumOutputChannels;
	};

	class ChannelMixer
	{
	private:
		class BufferMixer
		{
		public:
			void SetNumChannels(unsigned int numChans, unsigned int bufSize);
			const std::shared_ptr<audio::AudioBuffer> Channel(unsigned int channel);

		protected:
			std::vector<std::shared_ptr<AudioBuffer>> _buffers;
		};

		class AdcChannelMixer :
			public BufferMixer,
			public base::MultiAudioSource
		{
		public:
			virtual void EndMultiPlay(unsigned int numSamps) override;
			virtual unsigned int NumOutputChannels() const override;

		protected:
			virtual const std::shared_ptr<base::AudioSource> OutputChannel(unsigned int channel) override;
		};

		class DacChannelMixer :
			public BufferMixer,
			public base::MultiAudioSink
		{
		public:
			virtual void EndMultiWrite(unsigned int numSamps,
				base::Audible::AudioSourceType source) override;
			virtual void EndMultiWrite(unsigned int numSamps, bool updateIndex,
				base::Audible::AudioSourceType source) override;
			virtual unsigned int NumInputChannels() const override;

		protected:
			virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
				base::Audible::AudioSourceType source) override;
		};

	public:
		ChannelMixer(ChannelMixerParams chanMixParams);
		~ChannelMixer();

	public:
		void SetParams(ChannelMixerParams chanMixParams);
		void FromAdc(float* inBuf, unsigned int numChannels, unsigned int numSamps);
		void ToDac(float* outBuf, unsigned int numChannels, unsigned int numSamps);
		const std::shared_ptr<base::MultiAudioSource> Source();
		const std::shared_ptr<base::MultiAudioSink> Sink();
		void InitPlay(unsigned int delaySamps, unsigned int blockSize);

	public:
		static const unsigned int DefaultBufferSize = 3000;

	protected:
		std::shared_ptr<AdcChannelMixer> _adcMixer;
		std::shared_ptr<DacChannelMixer> _dacMixer;
	};
}
