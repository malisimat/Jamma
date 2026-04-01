
#include "gtest/gtest.h"
#include "audio/AudioMixer.h"

using audio::WireMixBehaviour;
using audio::WireMixBehaviourParams;
using audio::MergeMixBehaviour;
using audio::MergeMixBehaviourParams;
using audio::BounceMixBehaviour;
using audio::BounceMixBehaviourParams;
using base::MultiAudioSink;

struct MixWriteCall
{
	unsigned int Channel;
	float Samp;
	float FadeCurrent;
	float FadeNew;
	int IndexOffset;
	base::Audible::AudioSourceType Source;
};

class MockMultiSink :
	public MultiAudioSink
{
public:
	MockMultiSink(unsigned int numChannels) :
		_numChannels(numChannels)
	{
	}

public:
	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return _numChannels;
	}

	virtual void OnMixWriteChannel(unsigned int channel,
		float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		base::Audible::AudioSourceType source) override
	{
		Calls.push_back({ channel, samp, fadeCurrent, fadeNew, indexOffset, source });
	}

	std::vector<MixWriteCall> Calls;

private:
	unsigned int _numChannels;
};

// ---- WireMixBehaviour ----

TEST(WireMixBehaviour, ApplyWritesToSpecifiedChannel)
{
	WireMixBehaviourParams params({ 1u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 0.5f, 0.8f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_EQ(1u, sink->Calls[0].Channel);
	EXPECT_FLOAT_EQ(0.5f, sink->Calls[0].Samp);
	EXPECT_FLOAT_EQ(0.8f, sink->Calls[0].FadeNew);
}

TEST(WireMixBehaviour, ApplyFadeCurrentIsAlwaysZero)
{
	WireMixBehaviourParams params({ 0u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.3f, 0.6f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(WireMixBehaviour, ApplyWritesToMultipleChannels)
{
	WireMixBehaviourParams params({ 0u, 2u, 3u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 1.0f, 1.0f, 5);

	ASSERT_EQ(3u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(2u, sink->Calls[1].Channel);
	EXPECT_EQ(3u, sink->Calls[2].Channel);
	for (const auto& call : sink->Calls)
	{
		EXPECT_EQ(5, call.IndexOffset);
		EXPECT_FLOAT_EQ(0.0f, call.FadeCurrent);
		EXPECT_FLOAT_EQ(1.0f, call.FadeNew);
	}
}

TEST(WireMixBehaviour, ApplyNullDestDoesNotCrash)
{
	WireMixBehaviourParams params({ 0u });
	WireMixBehaviour behaviour(params);

	EXPECT_NO_FATAL_FAILURE(behaviour.Apply(nullptr, 1.0f, 1.0f, 0));
}

TEST(WireMixBehaviour, SetParamsUpdatesChannels)
{
	WireMixBehaviourParams initial({ 0u });
	WireMixBehaviour behaviour(initial);

	WireMixBehaviourParams updated({ 1u, 2u });
	behaviour.SetParams(updated);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 0.5f, 1.0f, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(1u, sink->Calls[0].Channel);
	EXPECT_EQ(2u, sink->Calls[1].Channel);
}

TEST(WireMixBehaviour, SetMaxChannelsRemovesOutOfRangeChannels)
{
	WireMixBehaviourParams params({ 0u, 1u, 2u, 3u });
	WireMixBehaviour behaviour(params);

	behaviour.SetMaxChannels(2u);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 1.0f, 1.0f, 0);

	// SetMaxChannels keeps channels with index < chans (count semantics).
	// For chans = 2, channels 0 and 1 remain; channels 2 and 3 are removed.
	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
}

// ---- MergeMixBehaviour ----

TEST(MergeMixBehaviour, ApplyWritesToSpecifiedChannel)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.7f, 0.5f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_FLOAT_EQ(0.7f, sink->Calls[0].Samp);
	EXPECT_FLOAT_EQ(0.5f, sink->Calls[0].FadeNew);
}

TEST(MergeMixBehaviour, ApplyFadeCurrentIsAlwaysOne)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.4f, 0.9f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(MergeMixBehaviour, ApplyIsAdditiveAcrossMultipleChannels)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u, 1u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.5f, 0.5f, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	for (const auto& call : sink->Calls)
	{
		EXPECT_FLOAT_EQ(1.0f, call.FadeCurrent);
		EXPECT_FLOAT_EQ(0.5f, call.FadeNew);
		EXPECT_FLOAT_EQ(0.5f, call.Samp);
	}
}

TEST(MergeMixBehaviour, ApplyNullDestDoesNotCrash)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	EXPECT_NO_FATAL_FAILURE(behaviour.Apply(nullptr, 1.0f, 1.0f, 0));
}

TEST(MergeMixBehaviour, SetParamsUpdatesChannels)
{
	MergeMixBehaviourParams initial;
	initial.Channels = { 0u };
	MergeMixBehaviour behaviour(initial);

	// MergeMixBehaviour inherits SetParams from WireMixBehaviour, which
	// handles WireMixBehaviourParams in the variant.
	WireMixBehaviourParams updated({ 2u, 3u });
	behaviour.SetParams(updated);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 1.0f, 1.0f, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(2u, sink->Calls[0].Channel);
	EXPECT_EQ(3u, sink->Calls[1].Channel);
}

TEST(MergeMixBehaviour, SetMaxChannelsRemovesOutOfRangeChannels)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u, 1u, 3u };
	MergeMixBehaviour behaviour(params);

	behaviour.SetMaxChannels(2u);

	auto sink = std::make_shared<MockMultiSink>(4u);
	behaviour.Apply(sink, 1.0f, 1.0f, 0);

	// SetMaxChannels removes channels strictly greater than chans.
	// Channel 3 (3 > 2) is removed; channels 0 and 1 remain.
	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
}

// ---- BounceMixBehaviour ----

TEST(BounceMixBehaviour, ApplyFadeNewZeroGivesFadeCurrentOne)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.8f, 0.0f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyFadeNewOneGivesFadeCurrentZero)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.8f, 1.0f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyMidFadeInterpolates)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.5f, 0.4f, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.4f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(0.6f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyNullDestDoesNotCrash)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	EXPECT_NO_FATAL_FAILURE(behaviour.Apply(nullptr, 1.0f, 0.5f, 0));
}

TEST(BounceMixBehaviour, ApplyWritesToMultipleChannels)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u, 1u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	behaviour.Apply(sink, 0.5f, 0.3f, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
	for (const auto& call : sink->Calls)
	{
		EXPECT_FLOAT_EQ(0.3f, call.FadeNew);
		EXPECT_FLOAT_EQ(0.7f, call.FadeCurrent);
	}
}
