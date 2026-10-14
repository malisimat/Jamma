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
	// Plugin reporting a narrower natural layout (e.g. stereo on an 8-channel
	// station) is now accepted; the host pass-through routes the extras.
	EXPECT_TRUE(vst::IsHostedLayoutCompatible(layout, 2, 2));
	// Wider than requested is still rejected to avoid surprising fan-out.
	EXPECT_FALSE(vst::IsHostedLayoutCompatible(layout, 8, 8));
	EXPECT_FALSE(vst::IsHostedLayoutCompatible(layout, 0, 2));
}

TEST(VstAudioBuffers, CopyOutputToMultiLeavesExtraDestinationChannelsUntouched)
{
	float plugChan0[] = { 0.1f, 0.2f };
	float plugChan1[] = { 0.3f, 0.4f };
	float* pluginOutputs[] = { plugChan0, plugChan1 };

	float dest0[] = { 9.0f, 9.0f };
	float dest1[] = { 9.0f, 9.0f };
	float dest2[] = { 9.0f, 9.0f };
	float dest3[] = { 9.0f, 9.0f };
	float* dest[] = { dest0, dest1, dest2, dest3 };

	vst::CopyOutputToMulti(pluginOutputs, 2, 2, dest, 4);

	EXPECT_FLOAT_EQ(0.1f, dest0[0]);
	EXPECT_FLOAT_EQ(0.2f, dest0[1]);
	EXPECT_FLOAT_EQ(0.3f, dest1[0]);
	EXPECT_FLOAT_EQ(0.4f, dest1[1]);
	// Channels beyond the plugin's output width are preserved as-is.
	EXPECT_FLOAT_EQ(9.0f, dest2[0]);
	EXPECT_FLOAT_EQ(9.0f, dest2[1]);
	EXPECT_FLOAT_EQ(9.0f, dest3[0]);
	EXPECT_FLOAT_EQ(9.0f, dest3[1]);
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
#include "vst/VstChain.h"
#include <limits>

class LatencyTestPlugin final : public vst::IVstPlugin
{
public:
	int Latency = 0;
	bool Loaded = true;
	bool Bypassed = false;
	mutable unsigned int PollCount = 0u;
	bool PreInit(const std::wstring&) override { return true; }
	bool Load(const std::wstring&, float, unsigned int, unsigned int, vst::HostedLayoutMode) override { return true; }
	void Unload() override { Loaded = false; }
	void ProcessBlock(float*, int32_t) noexcept override {}
	void ProcessBlockStereo(float*, float*, int32_t) noexcept override {}
	void ProcessBlockMulti(float* const*, int32_t, int32_t) noexcept override {}
	void SetParameter(unsigned int, float) noexcept override {}
	float GetParameter(unsigned int) const noexcept override { return 0.0f; }
	void BeginMidiBlock(std::uint32_t, std::uint32_t) noexcept override {}
	void SendMidiEvent(const midi::MidiEvent&, bool) noexcept override {}
	bool OpenEditor(HWND) override { return false; }
	void CloseEditor() override {}
	utils::Size2d GetEditorSize() const noexcept override { return { 0u, 0u }; }
	bool IsLoaded() const noexcept override { return Loaded; }
	const std::string& Name() const noexcept override { return PluginName; }
	void SetBypassed(bool bypassed) noexcept override { Bypassed = bypassed; }
	bool IsBypassed() const noexcept override { return Bypassed; }
	int GetLatencySamples() const noexcept override { return Latency; }
	void PollPendingChanges() const noexcept override { ++PollCount; }
private:
	std::string PluginName = "latency-test";
};

TEST(VstPathLatency, IndexedMidiRouteIncludesOnlyDestinationAndDownstreamDelay)
{
	vst::VstChain chain;
	for (int delay : { 13, 29, 47 })
	{
	 auto plugin = std::make_shared<LatencyTestPlugin>();
	 plugin->Latency = delay;
	 chain.AddPlugin(plugin);
	}
	EXPECT_EQ(89, chain.GetLatencySamples());
	EXPECT_EQ(76, chain.GetLatencySamplesFrom(1u));
	EXPECT_EQ(47, chain.GetLatencySamplesFrom(2u));
	EXPECT_EQ(0, chain.GetLatencySamplesFrom(3u));
}

TEST(VstPathLatency, BypassedUnloadedAndNegativeReportsContributeNoDelay)
{
	vst::VstChain chain;
	auto first = std::make_shared<LatencyTestPlugin>();
	auto second = std::make_shared<LatencyTestPlugin>();
	first->Latency = 13;
	second->Latency = 29;
	chain.AddPlugin(first);
	chain.AddPlugin(second);
	EXPECT_EQ(42, chain.GetLatencySamples());
	first->SetBypassed(true);
	EXPECT_EQ(29, chain.GetLatencySamples());
	second->Loaded = false;
	EXPECT_EQ(0, chain.GetLatencySamples());
	second->Loaded = true;
	second->Latency = -1;
	EXPECT_EQ(0, chain.GetLatencySamples());
	first->SetBypassed(false);
	first->Latency = 17;
	EXPECT_EQ(17, chain.GetLatencySamples());
}

TEST(VstPathLatency, ExcessiveReportsSaturateWithoutSignedOverflow)
{
	vst::VstChain chain;
	for (auto i = 0u; i < 2u; ++i)
	{
	 auto plugin = std::make_shared<LatencyTestPlugin>();
	 plugin->Latency = (std::numeric_limits<int>::max)();
	 chain.AddPlugin(plugin);
	}
	EXPECT_EQ((std::numeric_limits<int>::max)(), chain.GetLatencySamples());
}

TEST(VstPathLatency, NotificationPumpVisitsLoadedPluginsWithClosedEditors)
{
	vst::VstChain chain;
	auto plugin = std::make_shared<LatencyTestPlugin>();
	chain.AddPlugin(plugin);
	chain.PollPendingChanges();
	EXPECT_EQ(1u, plugin->PollCount);
}
