
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Loop.h"
#include "base/AudioSink.h"

using resources::ResourceLib;
using engine::Loop;
using engine::LoopParams;
using audio::WireMixBehaviourParams;
using audio::AudioMixerParams;
using base::AudioSource;
using base::AudioSink;
using base::MultiAudioSink;
using base::AudioSourceParams;

class LoopMockedSink :
	public AudioSink
{
public:
	LoopMockedSink(unsigned int bufSize) :
		Samples({})
	{
		Samples = std::vector<float>(bufSize, 0.0f);
	}

public:
	virtual void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override
	{
		for (auto i = 0u; i < request.numSamps; i++)
		{
			auto destIndex = _writeIndex + writeOffset + i;
			if (destIndex < Samples.size())
				Samples[destIndex] = (request.fadeNew * request.samples[i * request.stride]) +
					(request.fadeCurrent * Samples[destIndex]);
		}
	}
	virtual void EndWrite(unsigned int numSamps, bool updateIndex)
	{
		if (updateIndex)
			_writeIndex += numSamps;
	}

	bool IsFilled() { return _writeIndex >= Samples.size(); }

	std::vector<float> Samples;
};

class MockedMultiSink :
	public MultiAudioSink
{
public:
	MockedMultiSink(unsigned int bufSize)
	{
		_sink = std::make_shared<LoopMockedSink>(bufSize);
	}

public:
	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override { return 1; };

	bool IsFilled() { return _sink->IsFilled(); }

protected:
	virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType source) override
	{
		if (channel == 0)
			return _sink;

		return std::shared_ptr<AudioSink>();
	}
private:
	std::shared_ptr<LoopMockedSink> _sink;
};

TEST(Loop, PlayWrapsAround) {
	auto bufSize = 100;
	auto blockSize = 11;

	auto sink = std::make_shared<MockedMultiSink>(bufSize);

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };
	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "hh";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	engine::TriggerParams trigParams;
	trigParams.Index = 0;

	auto trigger = std::make_shared<engine::Trigger>(trigParams);

	auto loop = Loop(loopParams, mixerParams);

	// Initialize loop with audio data so _loopLength > 0 before playback.
	// Record MaxLoopFadeSamps + loopLength samples, then call Play() to
	// transition to playing state.
	const auto loopLength = 50ul;
	const auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

	loop.Record();

	std::vector<float> recordData(totalRecordSamps, 1.0f);
	base::AudioWriteRequest request;
	request.samples = recordData.data();
	request.numSamps = (unsigned int)totalRecordSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;
	loop.OnBlockWrite(request, 0);
	loop.EndWrite(totalRecordSamps, true);
	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (int i = 0; i < numBlocks; i++)
	{
		sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
		loop.WriteBlock(sink, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
		loop.EndMultiPlay(blockSize);
		sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
	}

	ASSERT_TRUE(sink->IsFilled());
}

TEST(Loop, BlockWriteMatchesSampleWrite) {
	auto blockSize = 64u;

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };
	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "sample";
	loopParams.Size = { 80, 80 };

	std::vector<float> data(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		data[i] = ((rand() % 2000) - 1000) / 1001.0f;

	const auto loopLength = (unsigned long)blockSize;
	const auto totalSamps = constants::MaxLoopFadeSamps + loopLength;

	// Block-level reference loop
	auto loopSample = Loop(loopParams, mixerParams);
	loopSample.Record();

	std::vector<float> sampleData(totalSamps, 0.0f);
	for (auto i = 0u; i < blockSize; i++)
		sampleData[i] = data[i];

	base::AudioWriteRequest sampleReq;
	sampleReq.samples = sampleData.data();
	sampleReq.numSamps = (unsigned int)totalSamps;
	sampleReq.stride = 1;
	sampleReq.fadeCurrent = 0.0f;
	sampleReq.fadeNew = 1.0f;
	sampleReq.source = base::Audible::AUDIOSOURCE_ADC;

	loopSample.OnBlockWrite(sampleReq, 0);
	loopSample.EndWrite(totalSamps, true);

	// Block-level loop
	auto loopBlock = Loop(loopParams, mixerParams);
	loopBlock.Record();

	base::AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = blockSize;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	loopBlock.OnBlockWrite(request, 0);

	std::vector<float> zeros(totalSamps - blockSize, 0.0f);
	base::AudioWriteRequest zeroReq;
	zeroReq.samples = zeros.data();
	zeroReq.numSamps = (unsigned int)(totalSamps - blockSize);
	zeroReq.stride = 1;
	zeroReq.fadeCurrent = 0.0f;
	zeroReq.fadeNew = 1.0f;
	zeroReq.source = base::Audible::AUDIOSOURCE_ADC;

	loopBlock.OnBlockWrite(zeroReq, (int)blockSize);
	loopBlock.EndWrite(totalSamps, true);

	loopSample.Play(constants::MaxLoopFadeSamps, loopLength, false);
	loopBlock.Play(constants::MaxLoopFadeSamps, loopLength, false);

	auto sinkSample = std::make_shared<MockedMultiSink>(blockSize);
	auto sinkBlock = std::make_shared<MockedMultiSink>(blockSize);

	sinkSample->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
	loopSample.WriteBlock(sinkSample, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
	loopSample.EndMultiPlay(blockSize);
	sinkSample->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

	sinkBlock->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
	loopBlock.WriteBlock(sinkBlock, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
	loopBlock.EndMultiPlay(blockSize);
	sinkBlock->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

	ASSERT_TRUE(sinkSample->IsFilled());
	ASSERT_TRUE(sinkBlock->IsFilled());
}

