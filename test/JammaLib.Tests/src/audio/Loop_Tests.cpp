
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Loop.h"

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
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = 0.0f;
		}
	}

public:
	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		base::Audible::AudioSourceType source) override
	{
		if ((_writeIndex + indexOffset) < Samples.size())
			Samples[_writeIndex + indexOffset] = (fadeNew * samp) + (fadeCurrent * Samples[_writeIndex + indexOffset]);

		return indexOffset + 1;
	};
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
	std::shared_ptr<LoopMockedSink> GetSink() { return _sink; }

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

class LoopMockedSource :
	public AudioSource
{
public:
	LoopMockedSource(unsigned int bufSize,
		AudioSourceParams params) :
		_index(0),
		Samples({}),
		AudioSource(params)
	{
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = ((rand() % 2000) - 1000) / 1001.0f;
		}
	}

public:
	virtual void OnPlay(const std::shared_ptr<base::AudioSink> dest,
		int indexOffset,
		unsigned int numSamps)
	{
		auto index = _index;
		auto source = AUDIOSOURCE_ADC;

		for (auto i = 0u; i < numSamps; i++)
		{
			if (index < Samples.size())
				dest->OnMixWrite(Samples[index], 0.0f, 1.0f, i, source);

			index++;
		}
	}
	virtual void EndPlay(unsigned int numSamps)
	{
		_index += numSamps;
	}
	bool WasPlayed() { return _index >= Samples.size(); }
	bool MatchesSink(const std::shared_ptr<LoopMockedSink> buf)
	{
		auto numSamps = buf->Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= Samples.size())
				return false;

			std::cout << "Comparing index " << samp << ": " << buf->Samples[samp] << " = " << Samples[samp] << "?" << std::endl;

			if (buf->Samples[samp] != Samples[samp])
				return false;
		}

		return true;
	}

private:
	unsigned int _index;
	std::vector<float> Samples;
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
	for (auto i = 0ul; i < totalRecordSamps; i++)
		loop.OnMixWrite(1.0f, 0.0f, 1.0f, (int)i, base::Audible::AUDIOSOURCE_ADC);
	loop.EndWrite(totalRecordSamps, true);
	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (int i = 0; i < numBlocks; i++)
	{
		sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
		loop.OnPlay(sink, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
		loop.EndMultiPlay(blockSize);
		sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
	}

	ASSERT_TRUE(sink->IsFilled());
}

// Verify that a constant-value loop produces no gain bump in the crossfade
// region at the wrap point. With normalized Hanning weights, fadeIn + fadeOut
// == 1.0, so overlapping identical content must pass through unchanged.
TEST(Loop, WrapXfade_ConstantInputNoGainBump) {
	const auto fadeSamps = constants::DefaultFadeSamps;
	const auto loopLength = 50ul;
	const auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;
	const float inputVal = 0.5f;

	// blockSize spans twice the fade window to guarantee the full xfade
	// region is captured (fadeSamps samples before wrap + fadeSamps after).
	const unsigned int blockSize = fadeSamps * 2u;

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };
	auto mixerParams = Loop::GetMixerParams({ 80, 80 }, mixBehaviour);

	LoopParams loopParams;
	loopParams.Wav = "test";
	loopParams.FadeSamps = fadeSamps;

	Loop loop(loopParams, mixerParams);
	loop.Record();

	for (auto i = 0ul; i < totalRecordSamps; i++)
		loop.OnMixWrite(inputVal, 0.0f, 1.0f, (int)i, base::Audible::AUDIOSOURCE_ADC);
	loop.EndWrite(static_cast<unsigned int>(totalRecordSamps), true);

	// Start playback just before the crossfade region so the captured block
	// straddles the entire fade window.
	auto startIndex = constants::MaxLoopFadeSamps + loopLength - fadeSamps;
	loop.Play(startIndex, loopLength, false);

	auto sink = std::make_shared<MockedMultiSink>(blockSize);
	loop.OnPlay(sink, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
	loop.EndMultiPlay(blockSize);
	sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

	const auto& samples = sink->GetSink()->Samples;
	for (unsigned int s = 0; s < blockSize; s++)
	{
		EXPECT_NEAR(samples[s], inputVal, 1e-5f)
			<< "Gain bump at crossfade sample " << s;
	}
}
