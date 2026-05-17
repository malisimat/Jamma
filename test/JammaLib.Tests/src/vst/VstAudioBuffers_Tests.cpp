#include "gtest/gtest.h"
#include "vst/VstAudioBuffers.h"

TEST(VstAudioBuffers, ResolveHostedBusLayoutPrefersStereoWhenPluginReportsStereoBus)
{
	auto layout = vst::ResolveHostedBusLayout(vst::HostedLayoutMode::MonoFlexible, 1u, 2, 2);

	EXPECT_EQ(2, layout.InputChannels);
	EXPECT_EQ(2, layout.OutputChannels);
	EXPECT_EQ(2, layout.MaxChannels);
}

TEST(VstAudioBuffers, ResolveHostedBusLayoutKeepsExactStationBusWidth)
{
	auto layout = vst::ResolveHostedBusLayout(vst::HostedLayoutMode::Exact, 6u, 2, 2);

	EXPECT_EQ(6, layout.RequestedChannels);
	EXPECT_EQ(6, layout.InputChannels);
	EXPECT_EQ(6, layout.OutputChannels);
	EXPECT_TRUE(vst::IsHostedLayoutCompatible(layout, 6, 6));
	EXPECT_FALSE(vst::IsHostedLayoutCompatible(layout, 2, 2));
}

TEST(VstAudioBuffers, CopyMonoToInputBuffersDuplicatesSignalAcrossStereoInputs)
{
	float monoInput[] = { 0.25f, -0.5f, 1.0f };
	float left[3] = {};
	float right[3] = {};
	float* inputs[] = { left, right };

	vst::CopyMonoToInputBuffers(monoInput, 3, 2, inputs);

	for (auto sampleIndex = 0; sampleIndex < 3; ++sampleIndex)
	{
		EXPECT_FLOAT_EQ(monoInput[sampleIndex], left[sampleIndex]);
		EXPECT_FLOAT_EQ(monoInput[sampleIndex], right[sampleIndex]);
	}
}

TEST(VstAudioBuffers, FoldOutputToMonoAveragesAllHostedChannels)
{
	float left[] = { 1.0f, 0.5f, -0.25f };
	float right[] = { -1.0f, 0.5f, 0.75f };
	float* outputs[] = { left, right };
	float monoOutput[3] = {};

	vst::FoldOutputToMono(outputs, 2, 3, monoOutput);

	EXPECT_FLOAT_EQ(0.0f, monoOutput[0]);
	EXPECT_FLOAT_EQ(0.5f, monoOutput[1]);
	EXPECT_FLOAT_EQ(0.25f, monoOutput[2]);
}

TEST(VstAudioBuffers, CopyMultiToInputBuffersCopiesEveryChannel)
{
	float chan0[] = { 1.0f, 2.0f };
	float chan1[] = { 3.0f, 4.0f };
	float chan2[] = { 5.0f, 6.0f };
	float* source[] = { chan0, chan1, chan2 };
	float dest0[2] = {};
	float dest1[2] = {};
	float dest2[2] = {};
	float* dest[] = { dest0, dest1, dest2 };

	vst::CopyMultiToInputBuffers(source, 3, 2, dest, 3);

	EXPECT_FLOAT_EQ(1.0f, dest0[0]);
	EXPECT_FLOAT_EQ(2.0f, dest0[1]);
	EXPECT_FLOAT_EQ(3.0f, dest1[0]);
	EXPECT_FLOAT_EQ(4.0f, dest1[1]);
	EXPECT_FLOAT_EQ(5.0f, dest2[0]);
	EXPECT_FLOAT_EQ(6.0f, dest2[1]);
}