#include "../gui/WorkspaceTestAccess.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <crtdbg.h>

#include "actions/JobAction.h"
#include "audio/PlaybackDelay.h"
#include "actions/TriggerAction.h"
#include "base/AudioSink.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
#include "engine/Scene.h"
#include "midi/MidiLoop.h"
#include "midi/MidiIndexedOutputSink.h"
#include "midi/MidiRouter.h"
#include "vst/Vst3Plugin.h"

using actions::JobAction;
using actions::TriggerAction;
using engine::LoopTake;
using engine::LoopTakeParams;
using midi::MidiEvent;
using engine::Station;
using engine::StationParams;

class StationMidiInstrumentFakePlugin final : public vst::IVstPlugin
{
public:
	bool PreInit(const std::wstring&) override { return true; }

	bool Load(const std::wstring&, float, unsigned int, unsigned int hostedChannels,
		vst::HostedLayoutMode = vst::HostedLayoutMode::Exact) override
	{
		_loaded = true;
		HostedChannels = hostedChannels;
		return true;
	}

	void Unload() override { _loaded = false; }
	void ProcessBlock(float* samples, int32_t count) noexcept override
	{ float* channels[]{samples}; ProcessBlockMulti(channels, 1, count); }
	void ProcessBlockStereo(float*, float*, int32_t) noexcept override {}

	void ProcessBlockMulti(float* const* channels, int32_t numChannels, int32_t numSamples) noexcept override
	{
		ProcessCalls++;
		LastBlockProcessed = !SkipNextProcess && !_bypassed && numChannels >= static_cast<int>(HostedChannels);
		SkipNextProcess = false;
		if (!LastBlockProcessed) return;
		if (ReplaceOutput)
			for (int channel = 0; channel < numChannels; ++channel)
				std::fill_n(channels[channel], numSamples, OutputValue);
		else if (OutputValue != 0.0f)
			for (int channel = 0; channel < numChannels; ++channel)
				for (int sample = 0; sample < numSamples; ++sample) channels[channel][sample] *= OutputValue;
		ProcessedEvents.insert(ProcessedEvents.end(), BlockEvents.begin(), BlockEvents.end());
		if (RenderMidiImpulses && channels && numChannels > 0 && channels[0])
			for (const auto& event : BlockEvents)
				if (event.IsNoteOn() && event.sampleOffset >= BlockStart
					&& event.sampleOffset - BlockStart < static_cast<std::uint32_t>(numSamples))
					channels[0][event.sampleOffset - BlockStart] += 1.0f;
		if (RenderAudioDelay && LatencySamps > 0 && LatencySamps < 512)
			for (auto sample = 0; sample < numSamples; ++sample)
			{
				for (auto channel = 0; channel < numChannels && channel < 2; ++channel)
				{
					if (!channels[channel]) continue;
					const auto input = channels[channel][sample];
					DelayHistory[channel][DelayIndex] = input;
					channels[channel][sample] = DelayHistory[channel][
						(DelayIndex + 512u - static_cast<unsigned int>(LatencySamps)) % 512u];
				}
				DelayIndex = (DelayIndex + 1u) % 512u;
			}
	}

	void BeginMidiBlock(std::uint32_t blockStartSample,
		std::uint32_t numSamples) noexcept override
	{
		BlockStart = blockStartSample;
		BlockSamples = numSamples;
		BeginCalls++;
		LastBlockProcessed = false;
		AcceptedThisBlock = 0u;
		BlockEvents.clear();
	}

	void SendMidiEvent(const MidiEvent& event, bool isRealtime) noexcept override
	{
		Events.push_back(event);
		BlockEvents.push_back(event);
		RealtimeFlags.push_back(isRealtime);
	}

	bool TrySendMidiEvent(const MidiEvent& event, bool isRealtime) noexcept override
	{
		if (AcceptedThisBlock >= MaxAcceptedPerBlock) return false;
		++AcceptedThisBlock;
		SendMidiEvent(event, isRealtime);
		return true;
	}
	bool MidiBlockWasProcessed() const noexcept override { return LastBlockProcessed; }

	void SetParameter(unsigned int index, float value) noexcept override
	{
		ParamSetCalls++;
		LastParamIndex = index;
		LastParamValue = value;
	}

	float GetParameter(unsigned int index) const noexcept override
	{
		(void)index;
		return LastParamValue;
	}

	bool OpenEditor(HWND) override { return false; }
	void CloseEditor() override {}
	utils::Size2d GetEditorSize() const noexcept override { return { 0, 0 }; }
	bool IsLoaded() const noexcept override { return _loaded; }
	const std::string& Name() const noexcept override { return _name; }
	vst::RenderRole Role() const noexcept override { return RenderRole; }
	vst::MidiInputCapabilities MidiInput() const noexcept override { return MidiCapabilities; }
	vst::MidiInputCapabilities MidiCapabilities{true, true, true};
	unsigned int HostedChannelCount() const noexcept override { return HostedChannels; }
	vst::RenderRole RenderRole = vst::RenderRole::Effect;
	bool ReplaceOutput = false;
	float OutputValue = 0.0f;
	void SetBypassed(bool bypass) noexcept override { _bypassed = bypass; }
	bool IsBypassed() const noexcept override { return _bypassed; }
	int GetLatencySamples() const noexcept override { return LatencySamps; }

	std::uint32_t BlockStart = 0u;
	std::uint32_t BlockSamples = 0u;
	unsigned int BeginCalls = 0u;
	unsigned int HostedChannels = 0u;
	unsigned int ProcessCalls = 0u;
	unsigned int ParamSetCalls = 0u;
	unsigned int LastParamIndex = 0u;
	float LastParamValue = 0.0f;
	int LatencySamps = 0;
	bool RenderMidiImpulses = false;
	bool RenderAudioDelay = false;
	std::array<std::array<float, 512u>, 2u> DelayHistory{};
	unsigned int DelayIndex = 0u;
	std::size_t MaxAcceptedPerBlock = static_cast<std::size_t>(-1);
	std::size_t AcceptedThisBlock = 0u;
	bool SkipNextProcess = false;
	bool LastBlockProcessed = true;
	std::vector<MidiEvent> BlockEvents;
	std::vector<MidiEvent> ProcessedEvents;
	std::vector<MidiEvent> Events;
	std::vector<bool> RealtimeFlags;

private:
	bool _loaded = false;
	bool _bypassed = false;
	std::string _name = "fake-midi-plugin";
};

class StationMidiInstrumentCaptureSink : public base::AudioSink
{
public:
	explicit StationMidiInstrumentCaptureSink(unsigned int bufSize) : Samples(bufSize, 0.0f) {}

	void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override
	{
		for (auto sampleIndex = 0u; sampleIndex < request.numSamps; ++sampleIndex)
		{
			auto bufferIndex = _writeIndex + writeOffset + sampleIndex;
			if (bufferIndex < Samples.size())
				Samples[bufferIndex] = request.samples[sampleIndex * request.stride];
		}
	}

	void EndWrite(unsigned int numSamps, bool updateIndex) override
	{
		if (updateIndex)
			_writeIndex += numSamps;
	}

	std::vector<float> Samples;
};

class StationMidiInstrumentCaptureMultiSink : public base::MultiAudioSink
{
public:
	StationMidiInstrumentCaptureMultiSink(unsigned int numChannels, unsigned int bufSize)
	{
		for (auto channel = 0u; channel < numChannels; ++channel)
			_sinks.push_back(std::make_shared<StationMidiInstrumentCaptureSink>(bufSize));
	}

	unsigned int NumInputChannels(base::Audible::AudioSourceType) const override
	{
		return static_cast<unsigned int>(_sinks.size());
	}

protected:
	const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType) override
	{
		return channel < _sinks.size() ? _sinks[channel] : nullptr;
	}

public:
	const std::vector<float>& ChannelSamples(unsigned int channel) const { return _sinks.at(channel)->Samples; }

private:
	std::vector<std::shared_ptr<StationMidiInstrumentCaptureSink>> _sinks;
};

class ChildMidiTestStation final : public Station
{
public:
	using Station::Station;
	auto HoldAudioState() const { return _BorrowAudioState(); }
};

class ChildMidiTestTake final : public LoopTake
{
public:
	using LoopTake::LoopTake;
	auto HoldAudioState() const { return _BorrowAudioState(); }
	auto HoldMidiSources() const { return _BorrowMidiLoopSnapshot(); }
};

static std::shared_ptr<ChildMidiTestStation> MakeStation(const std::string& name)
{
	StationParams params;
	params.Name = name;
	params.Size = { 200, 200 };
	audio::MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	auto station = std::make_shared<ChildMidiTestStation>(params, mixerParams);
	station->SetAllowedMidiChannels({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
	station->SetupBuffers(128u);
	station->SetNumDacChannels(2u);
	station->CommitChanges();
	return station;
}

static std::shared_ptr<StationMidiInstrumentFakePlugin> AddPlugin(const std::shared_ptr<Station>& station,
	const std::wstring& path)
{
	auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
	JobAction job;
	job.JobActionType = JobAction::JOB_LOADVST;
	job.VstPath = path;
	job.PreInitPlugin = plugin;
	if(!job.VstManaged) EXPECT_TRUE(station->StageVstJob(job)); station->OnAction(job);
	station->CommitChanges();
	return plugin;
}

static void AllowAllMidiChannels(const std::shared_ptr<Station>& station)
{
	station->SetAllowedMidiChannels({
		1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11, 12, 13, 14, 15, 16
	});
}

static void RenderStationBlock(const std::shared_ptr<Station>& station,
	std::uint32_t blockStart,
	unsigned int numSamps = 128u)
{
	auto sink = std::make_shared<StationMidiInstrumentCaptureMultiSink>(2u, numSamps);
	station->Zero(numSamps, base::Audible::AUDIOSOURCE_LOOPS);
	station->WriteBlock(sink, nullptr, 0, numSamps, blockStart);
	station->EndMultiPlay(numSamps);
}

static std::vector<float> RenderStationAudioBlock(const std::shared_ptr<Station>& station,
	std::uint32_t blockStart, unsigned int numSamps)
{
	auto sink = std::make_shared<StationMidiInstrumentCaptureMultiSink>(2u, numSamps);
	station->Zero(numSamps, base::Audible::AUDIOSOURCE_LOOPS);
	station->WriteBlock(sink, nullptr, 0, numSamps, blockStart);
	station->EndMultiPlay(numSamps);
	return sink->ChannelSamples(0u);
}

static std::shared_ptr<ChildMidiTestTake> MakeMidiTake(const std::string& id)
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<ChildMidiTestTake>(params, mixerParams);
}

TEST(StationMidiInstrument, LiveMidiIsDeliveredToStationVstPlugin)
{
	auto station = MakeStation("station-live");
	auto plugin = AddPlugin(station, L"fake-live.dll");
	AllowAllMidiChannels(station);

	auto event = MidiEvent::MakeNoteOn(72u, 0u, 60u, 100u);
	station->TryEnqueueSyntheticLiveMidi(event);

	RenderStationBlock(station, 64u);

	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_EQ(event.status, plugin->Events[0].status);
	EXPECT_EQ(event.data1, plugin->Events[0].data1);
	EXPECT_TRUE(plugin->RealtimeFlags[0]);
	EXPECT_EQ(64u, plugin->BlockStart);
	EXPECT_EQ(128u, plugin->BlockSamples);
}

TEST(StationMidiInstrument, AdditiveRackPreservesGeneratorsEffectOrderAndDestinationLatency)
{
	vst::VstChain chain;
	auto a = std::make_shared<StationMidiInstrumentFakePlugin>();
	auto b = std::make_shared<StationMidiInstrumentFakePlugin>();
	auto effect = std::make_shared<StationMidiInstrumentFakePlugin>();
	for (const auto& plugin : {a, b, effect}) plugin->Load(L"fake", 48000, 128, 1);
	a->RenderRole = b->RenderRole = vst::RenderRole::Instrument;
	a->ReplaceOutput = b->ReplaceOutput = true;
	a->OutputValue = 2.0f; b->OutputValue = 3.0f; effect->OutputValue = 4.0f;
	a->LatencySamps = 11; b->LatencySamps = 23; effect->LatencySamps = 7;
	chain.AddSlot(a); chain.AddSlot(effect); chain.AddSlot(b);
	std::array<float, 4> audio{1, 1, 1, 1}, scratch{};
	float* channels[]{audio.data()}; float* instruments[]{scratch.data()};
	chain.ProcessRack(channels, instruments, 1, 4);
	for (const auto sample : audio) EXPECT_FLOAT_EQ(15.0f, sample); // (input + A) * effect + B
	EXPECT_EQ(7, chain.GetLatencySamples());
	EXPECT_EQ(18, chain.GetLatencySamplesFrom(0));
	EXPECT_EQ(7, chain.GetLatencySamplesFrom(1));
	EXPECT_EQ(23, chain.GetLatencySamplesFrom(2));
}

TEST(StationMidiInstrument, ChannelRoutesAreManyToManyAndEmptyTargetsBroadcast)
{
	auto station = MakeStation("channel-routes");
	auto a = AddPlugin(station, L"a.dll");
	auto b = AddPlugin(station, L"b.dll");
	station->SetAllowedMidiChannels({1});
	ASSERT_TRUE(station->SetMidiChannelRoute(0, {{a->StateId(), a->StateId(), b->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(1, {{b->StateId()}}, station->RackRevision()));
	EXPECT_EQ((std::vector<int>{1, 2}), station->AllowedMidiChannels());
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(0, 0, 60, 100));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(0, 1, 61, 100));
	RenderStationBlock(station, 0);
	EXPECT_EQ(1u, a->Events.size());
	EXPECT_EQ(2u, b->Events.size());
	auto routes = station->MidiChannelRoutes();
	routes[0] = {{}};
	routes[2] = {{a->StateId()}};
	const auto revision = station->RackRevision();
	ASSERT_TRUE(station->SetMidiChannelRoutes(routes, revision));
	EXPECT_EQ(revision + 1u, station->RackRevision());
	EXPECT_FALSE(station->SetMidiChannelRoutes(routes, revision));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(128, 0, 62, 100));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(128, 2, 63, 100));
	RenderStationBlock(station, 128);
	EXPECT_EQ(3u, std::count_if(a->Events.begin(), a->Events.end(), [](const auto& event) { return event.IsNoteOn(); }));
	EXPECT_EQ(3u, std::count_if(b->Events.begin(), b->Events.end(), [](const auto& event) { return event.IsNoteOn(); }));
	routes = station->MidiChannelRoutes();
	routes[3] = {{a->StateId()}};
	routes[4] = {{b->StateId()}};
	ASSERT_TRUE(station->SetMidiChannelRoutes(routes, station->RackRevision()));
	EXPECT_TRUE(station->AcceptsLiveMidiChannel(3u));
	EXPECT_TRUE(station->AcceptsLiveMidiChannel(4u));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(256, 3, 64, 100));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(256, 4, 65, 100));
	RenderStationBlock(station, 256);
	EXPECT_TRUE(std::any_of(a->Events.begin(), a->Events.end(), [](const auto& event) { return event.IsNoteOn() && event.Channel() == 3u; }));
	EXPECT_FALSE(std::any_of(a->Events.begin(), a->Events.end(), [](const auto& event) { return event.IsNoteOn() && event.Channel() == 4u; }));
	EXPECT_TRUE(std::any_of(b->Events.begin(), b->Events.end(), [](const auto& event) { return event.IsNoteOn() && event.Channel() == 4u; }));
	EXPECT_FALSE(std::any_of(b->Events.begin(), b->Events.end(), [](const auto& event) { return event.IsNoteOn() && event.Channel() == 3u; }));
}

TEST(StationMidiInstrument, DisablingMidiChannelClearsRoutesAndReenableBroadcasts)
{
	auto station = MakeStation("channel-toggle-routes");
	auto a = AddPlugin(station, L"toggle-a.dll");
	auto b = AddPlugin(station, L"toggle-b.dll");
	station->SetAllowedMidiChannels({1});
	ASSERT_TRUE(station->SetMidiChannelRoute(0, {{a->StateId()}}, station->RackRevision()));
	const auto staleRevision = station->RackRevision();
	const auto staleRoutes = station->MidiChannelRoutes();

	station->SetAllowedMidiChannels({});
	EXPECT_FALSE(station->AcceptsLiveMidiChannel(0u));
	EXPECT_TRUE(station->MidiChannelRoutes()[0].PluginIds.empty());
	EXPECT_EQ(staleRevision + 1u, station->RackRevision());
	EXPECT_FALSE(station->SetMidiChannelRoutes(staleRoutes, staleRevision));

	station->SetAllowedMidiChannels({1});
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(0, 0, 60, 100));
	RenderStationBlock(station, 0);
	EXPECT_EQ(1u, a->Events.size());
	EXPECT_EQ(1u, b->Events.size());
}

TEST(StationMidiInstrument, DisablingUnroutedChannelRejectsPendingCableEdit)
{
	auto station = MakeStation("pending-channel-cable");
	auto plugin = AddPlugin(station, L"pending-cable.dll");
	station->SetAllowedMidiChannels({1});
	auto pendingRoutes = station->MidiChannelRoutes();
	pendingRoutes[0] = {{plugin->StateId()}};
	const auto staleRevision = station->RackRevision();

	station->SetAllowedMidiChannels({});
	EXPECT_EQ(staleRevision + 1u, station->RackRevision());
	EXPECT_FALSE(station->SetMidiChannelRoutes(std::move(pendingRoutes), staleRevision));
	EXPECT_FALSE(station->AcceptsLiveMidiChannel(0u));
}

TEST(StationMidiInstrument, RackRerouteAndRemovalRetryOwnedNoteAndSustainReleases)
{
	auto station = MakeStation("rack-release");
	auto a = AddPlugin(station, L"a.dll");
	auto b = AddPlugin(station, L"b.dll");
	AllowAllMidiChannels(station);
	ASSERT_TRUE(station->SetMidiChannelRoute(0, {{a->StateId()}}, station->RackRevision()));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(0, 0, 60, 100));
	station->TryEnqueueSyntheticLiveMidi({0, 0xb0, 64, 127});
	RenderStationBlock(station, 0);
	a->MaxAcceptedPerBlock = 0;
	ASSERT_TRUE(station->SetMidiChannelRoute(0, {{b->StateId()}}, station->RackRevision()));
	RenderStationBlock(station, 128);
	a->MaxAcceptedPerBlock = 100;
	RenderStationBlock(station, 256);
	EXPECT_TRUE(std::any_of(a->ProcessedEvents.begin(), a->ProcessedEvents.end(), [](const auto& event) { return event.IsNoteOff(); }));
	EXPECT_TRUE(std::any_of(a->ProcessedEvents.begin(), a->ProcessedEvents.end(), [](const auto& event) { return event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 == 0u; }));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(384, 0, 61, 100));
	station->TryEnqueueSyntheticLiveMidi({384, 0xb0, 64, 127});
	RenderStationBlock(station, 384);
	ASSERT_TRUE(station->SetMidiChannelRoute(1, {{a->StateId(), b->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(2, {{}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(15, {{b->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->UnloadManagedVstPlugin(b->StateId()));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	const auto& liveRoutes = station->MidiChannelRoutes();
	EXPECT_TRUE(liveRoutes[0].PluginIds.empty());
	EXPECT_EQ((std::vector<std::string>{a->StateId()}), liveRoutes[1].PluginIds);
	EXPECT_TRUE(liveRoutes[2].PluginIds.empty());
	EXPECT_TRUE(liveRoutes[15].PluginIds.empty());
	for (const auto& route : liveRoutes)
		EXPECT_EQ(route.PluginIds.end(), std::find(route.PluginIds.begin(), route.PluginIds.end(), b->StateId()));
	EXPECT_EQ(1u, station->GetVstRackSlots().size());
	b->MaxAcceptedPerBlock = 0;
	RenderStationBlock(station, 512);
	b->MaxAcceptedPerBlock = 100;
	RenderStationBlock(station, 640);
	EXPECT_TRUE(std::any_of(b->ProcessedEvents.begin(), b->ProcessedEvents.end(), [](const auto& event) { return event.IsNoteOff() && event.data1 == 61; }));
	EXPECT_TRUE(std::any_of(b->ProcessedEvents.begin(), b->ProcessedEvents.end(), [](const auto& event) { return event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 == 0u; }));

}

TEST(StationMidiInstrument, QueuedRackRemovalsClearOnlyDeletedCableIdentities)
{
	auto station = MakeStation("queued-rack-removals");
	auto a = AddPlugin(station, L"queued-a.dll");
	auto b = AddPlugin(station, L"queued-b.dll");
	auto c = AddPlugin(station, L"queued-c.dll");
	ASSERT_TRUE(station->SetMidiChannelRoute(0,
		{{a->StateId(), b->StateId(), c->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(15,
		{{a->StateId(), b->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->UnloadManagedVstPlugin(a->StateId()));
	ASSERT_TRUE(station->UnloadManagedVstPlugin(b->StateId()));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	ASSERT_EQ(1u, station->GetVstRackSlots().size());
	EXPECT_EQ(c->StateId(), station->GetVstRackSlots()[0].Identity);
	const auto& routes = station->MidiChannelRoutes();
	EXPECT_EQ((std::vector<std::string>{c->StateId()}), routes[0].PluginIds);
	EXPECT_TRUE(routes[15].PluginIds.empty());
}

TEST(StationMidiInstrument, AutomationSurvivesCompactionAndReleasesRemovedProcessor)
{
	auto station = MakeStation("automation-compaction");
	auto a = AddPlugin(station, L"fake-auto-a.dll");
	auto b = AddPlugin(station, L"fake-auto-b.dll");
	auto c = AddPlugin(station, L"fake-auto-c.dll");
	const auto removedId = b->StateId();
	const auto survivingId = c->StateId();
	auto take = MakeMidiTake("automation-take");
	LoopTake::MidiExportState state;
	state.LoopLengthSamps = 1024u;
	LoopTake::MidiStreamExport stream;
	stream.Loop.LoopLengthSamps = 1024u;
	state.Streams.push_back(std::move(stream));
	ASSERT_TRUE(take->RestoreMidiFromExport(state));
	auto loop = take->GetMidiLoopSnapshot().front();
	ASSERT_TRUE(loop->WireEditorAutomationLane(0u, b.get(), 1u, removedId));
	ASSERT_TRUE(loop->WireEditorAutomationLane(1u, c.get(), 2u, survivingId));
	loop->SetAutomationValueAtFrac(0u, 0.0, 0.25f);
	loop->SetAutomationValueAtFrac(1u, 0.0, 0.75f);
	station->AddTake(take);
	station->CommitChanges();
	RenderStationBlock(station, 0u);
	ASSERT_GT(b->ParamSetCalls, 0u);
	ASSERT_GT(c->ParamSetCalls, 0u);
	const auto removedCalls = b->ParamSetCalls;
	const auto survivingCalls = c->ParamSetCalls;
	ASSERT_TRUE(station->UnloadManagedVstPlugin(removedId));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	RenderStationBlock(station, 128u);
	EXPECT_EQ(removedCalls, b->ParamSetCalls);
	EXPECT_GT(c->ParamSetCalls, survivingCalls);
	EXPECT_EQ(0u, a->ParamSetCalls);
	LoopTake::MidiExportState captured;
	ASSERT_TRUE(take->SnapshotMidiForExport(captured));
	const auto& lanes = captured.Streams.front().Loop.AutomationLanes;
	EXPECT_EQ(removedId, lanes[0].SavedTarget.PluginId);
	EXPECT_EQ(survivingId, lanes[1].SavedTarget.PluginId);
	EXPECT_EQ(1u, lanes[1].SavedTarget.PluginIndex);
	std::weak_ptr<vst::IVstPlugin> lifetime = b;
	b.reset();
	for (auto block = 2u; block < 6u; ++block)
	{
		RenderStationBlock(station, block * 128u);
		station->AcknowledgeAudioBoundary();
		station->ReleaseRetiredAudioStates();
		vst::DrainUiThreadDestroyQueue();
	}
	EXPECT_TRUE(lifetime.expired());
	// A later rebuild must not dereference or republish the lane's old observer.
	station->RebuildAutomationDispatch();
	RenderStationBlock(station, 768u);
	EXPECT_EQ(0u, a->ParamSetCalls);
}

TEST(StationMidiInstrument, ChildPluginRemovalSchedulesPinnedAutomationRefresh)
{
	auto station = MakeStation("child-automation");
	auto take = MakeMidiTake("child-automation-take");
	auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
	JobAction load;
	load.JobActionType = JobAction::JOB_LOADVST;
	load.VstPath = L"fake-child-automation.dll";
	load.PreInitPlugin = plugin;
	take->OnAction(load);
	take->CommitChanges();
	LoopTake::MidiExportState state;
	state.LoopLengthSamps = 1024u;
	LoopTake::MidiStreamExport stream;
	stream.Loop.LoopLengthSamps = 1024u;
	state.Streams.push_back(std::move(stream));
	ASSERT_TRUE(take->RestoreMidiFromExport(state));
	auto loop = take->GetMidiLoopSnapshot().front();
	ASSERT_TRUE(loop->WireEditorAutomationLane(0u, plugin.get(), 3u, plugin->StateId()));
	loop->SetAutomationValueAtFrac(0u, 0.0, 0.5f);
	station->AddTake(take);
	station->CommitChanges();
	RenderStationBlock(station, 0u);
	ASSERT_GT(plugin->ParamSetCalls, 0u);
	const auto calls = plugin->ParamSetCalls;
	JobAction unload;
	unload.JobActionType = JobAction::JOB_UNLOADVST;
	unload.VstIdentity = plugin->StateId();
	take->OnAction(unload);
	station->CommitChanges(); // Child commits after parent and schedules refresh.
	station->CommitChanges();
	RenderStationBlock(station, 128u);
	EXPECT_EQ(calls, plugin->ParamSetCalls);
	EXPECT_EQ("take", loop->GetLane(0u).Mapping.SavedTarget.Scope);
	EXPECT_EQ(plugin->StateId(), loop->GetLane(0u).Mapping.SavedTarget.PluginId);
}

TEST(StationMidiInstrument, ShutdownReleasesPluginWithoutAnotherAudioBoundary)
{
	auto station = MakeStation("shutdown");
	auto plugin = AddPlugin(station, L"fake-shutdown.dll");
	std::weak_ptr<vst::IVstPlugin> lifetime = plugin;
	plugin.reset();
	station->ForceUnloadAllVstPlugins();
	vst::DrainUiThreadDestroyQueue();
	EXPECT_TRUE(lifetime.expired());
	// Shutdown is also called again from Scene destruction.
	station->ForceUnloadAllVstPlugins();
	vst::DrainUiThreadDestroyQueue();
}

TEST(StationMidiInstrument, ShutdownReleasesRemovedPluginHeldByPlaybackRoute)
{
	auto station = MakeStation("shutdown-held-route");
	auto plugin = AddPlugin(station, L"fake-held-shutdown.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("held-route");
	take->Record({}, station->Name(), { 0u }, { "source" });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "source", 0u));
	take->Play(0u, 1000u, 0u);
	station->AddTake(take);
	station->CommitChanges();
	RenderStationBlock(station, 0u);
	ASSERT_FALSE(plugin->Events.empty());
	JobAction unload;
	unload.JobActionType = JobAction::JOB_UNLOADVST;
	unload.VstIndex = 0u;
	unload.VstIdentity = plugin->StateId();
	ASSERT_TRUE(station->StageVstJob(unload));
	station->OnAction(unload);
	station->CommitChanges();
	std::weak_ptr<vst::IVstPlugin> lifetime = plugin;
	plugin.reset();
	station->ForceUnloadAllVstPlugins();
	vst::DrainUiThreadDestroyQueue();
	EXPECT_TRUE(lifetime.expired());
}

class ChannelOverrideSettingsScene final : public engine::Scene
{
public:
	ChannelOverrideSettingsScene() : Scene(engine::SceneParams({ "" }, {}, { 800u, 600u }), io::UserConfig{}) {}
	void Attach(const std::shared_ptr<Station>& station) { _AddStation(station); }
	const auto& ChannelControl() const { return _workspace->Settings().Channel(); }
	void RefreshChannel() { _workspace->Settings().RefreshChannelPresentation(ChannelControl()->Channel()); }
};

TEST(StationMidiInstrument, SettingsOverrideChangesFlushHeldNotesAndPresentationDoesNot)
{
	auto scene = std::make_shared<ChannelOverrideSettingsScene>();
	scene->InitReceivers();
	auto station = MakeStation("override-settings");
	auto plugin = AddPlugin(station, L"fake-channel-override.dll");
	AllowAllMidiChannels(station);
	scene->Attach(station);
	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "keyboard");
	auto channel = std::dynamic_pointer_cast<gui::GuiToggle>(scene->ChannelControl()->TryGetChild(6));
	ASSERT_TRUE(channel);
	channel->SetToggleState(gui::GuiToggleParams::TOGGLE_ON, false);
	EXPECT_EQ(7u, scene->ChannelControl()->Channel());
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events.front().IsNoteOff());
	EXPECT_EQ(0u, plugin->Events.front().Channel());
	plugin->Events.clear();
	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 6u, 62u, 100u), "keyboard");
	scene->RefreshChannel();
	RenderStationBlock(station, 128u);
	EXPECT_TRUE(plugin->Events.empty());
	channel->SetToggleState(gui::GuiToggleParams::TOGGLE_OFF, false);
	EXPECT_EQ(0u, scene->ChannelControl()->Channel());
	RenderStationBlock(station, 256u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events.front().IsNoteOff());
	EXPECT_EQ(6u, plugin->Events.front().Channel());
	scene->Shutdown();
}

TEST(StationMidiInstrument, LostDeviceReleasePreservesHealthyAndAnonymousHeldNotes)
{
    auto station = MakeStation("scoped-source-loss");
    auto plugin = AddPlugin(station, L"fake-scoped-loss.dll");
    AllowAllMidiChannels(station);
    station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "lost");
    station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "healthy");
    station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), "lost");
    station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 62u, 100u), "lost");
    station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 62u, 100u), "");
    station->FlushLiveHeldMidiNotesForDevice("lost");
    RenderStationBlock(station, 0u);
    ASSERT_EQ(1u, plugin->Events.size());
    EXPECT_TRUE(plugin->Events.front().IsNoteOff());
    EXPECT_EQ(61u, plugin->Events.front().data1);
    plugin->Events.clear();
    station->FlushLiveHeldMidiNotes();
    RenderStationBlock(station, 128u);
    ASSERT_EQ(2u, plugin->Events.size());
    EXPECT_EQ(60u, plugin->Events[0].data1);
    EXPECT_EQ(62u, plugin->Events[1].data1);
}

TEST(StationMidiInstrument, LostDeviceReleaseRetriesAfterSyntheticQueueSaturation)
{
    for (const bool acceptHealthy : { false, true })
    {
        SCOPED_TRACE(acceptHealthy ? "accepted live owner" : "dropped live owner");
        auto station = MakeStation("saturated-source-loss");
        auto plugin = AddPlugin(station, L"fake-saturated-loss.dll");
        AllowAllMidiChannels(station);
        station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 65u, 100u), "lost");
        station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 66u, 100u), "lost");
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 65u, 100u)));
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 66u, 100u)));
        RenderStationBlock(station, 0u);
        plugin->Events.clear();
        unsigned int queued = 0u;
        while (queued < 10000u && station->TryEnqueueSyntheticLiveMidi(
            MidiEvent::MakeNoteOn(1u, 0u, 30u, 100u))) ++queued;
        ASSERT_GT(queued, 0u);
        ASSERT_LT(queued, 10000u);
        station->FlushLiveHeldMidiNotesForDevice("lost");
        RenderStationBlock(station, 0u);
        station->FlushPendingLostLiveMidiNotes();
        unsigned int immediateQueued = 0u;
        if (!acceptHealthy)
            while (immediateQueued < 1024u && station->TryEnqueueImmediateLiveMidi(
                MidiEvent::MakeNoteOn(0u, 0u, 30u, 100u))) ++immediateQueued;
        // Earlier than the backlogged synthetic traffic, so an accepted owner is
        // delivered before the queued release. Observation alone cannot claim it.
        const auto healthy = MidiEvent::MakeNoteOn(0u, 0u, 66u, 100u);
        station->ObservePhysicalMidiForRecording(healthy, "healthy");
        ASSERT_EQ(acceptHealthy, station->TryEnqueueImmediateLiveMidi(healthy));
        plugin->Events.clear();
        unsigned int releases = 0u;
        unsigned int sharedReleases = 0u;
        // Production drains a bounded slice per block. Keep that limit intact and
        // retry while the queued traffic ahead of our release is consumed.
        for (unsigned int block = 1u; block <= (queued + immediateQueued + 63u) / 64u + 2u; ++block)
        {
            station->FlushPendingLostLiveMidiNotes();
            RenderStationBlock(station, block * 128u);
            for (const auto& event : plugin->Events)
            {
                if (event.IsNoteOff() && event.data1 == 65u) ++releases;
                if (event.IsNoteOff() && event.data1 == 66u) ++sharedReleases;
            }
            plugin->Events.clear();
        }
        EXPECT_EQ(1u, releases);
        EXPECT_EQ(acceptHealthy ? 0u : 1u, sharedReleases);
        plugin->Events.clear();
        station->FlushPendingLostLiveMidiNotes();
        RenderStationBlock(station, 256u);
        EXPECT_TRUE(plugin->Events.empty());
    }
}

TEST(StationMidiInstrument, SourceLossDitchRetriesRemovedTargetsPlaybackHeldRelease)
{
    auto station = MakeStation("removed-playback-source-loss");
    auto plugin = AddPlugin(station, L"fake-removed-playback-loss.dll");
    AllowAllMidiChannels(station);
    auto take = MakeMidiTake("removed-held-target");
    take->Record({}, station->Name(), { 0u }, { "lost" });
    ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 67u, 100u), "lost", 0u));
    ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "lost", 0u));
    take->Play(0u, 1000u, 0u);
    station->AddTake(take);
    auto healthy = MakeMidiTake("late-healthy-owner");
    healthy->Record({}, station->Name(), { 0u }, { "healthy" });
    ASSERT_TRUE(healthy->RecordMidiEvent(MidiEvent::MakeNoteOn(128u, 0u, 60u, 100u), "healthy", 0u));
    healthy->Play(0u, 10000u, 0u);
    station->AddTake(healthy);
    station->CommitChanges();
    RenderStationBlock(station, 0u);
    ASSERT_FALSE(plugin->Events.empty());
    plugin->Events.clear();
    unsigned int queued = 0u;
    while (queued < 10000u && station->TryEnqueueSyntheticLiveMidi(
        MidiEvent::MakeNoteOn(0u, 0u, 30u, 100u))) ++queued;
    ASSERT_GT(queued, 0u);
    ASSERT_LT(queued, 10000u);
    TriggerAction ditch;
    ditch.ActionType = TriggerAction::TRIGGER_DITCH;
    ditch.TargetId = take->Id();
    ditch.IsSourceLossCancellation = true;
    EXPECT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
    station->CommitChanges();
    ASSERT_EQ(1u, station->GetLoopTakeSnapshot().size());
    unsigned int releases = 0u;
    // The healthy owner starts after cancellation, while the release is queued.
    for (unsigned int block = 1u; block <= (queued + 63u) / 64u + 2u; ++block)
    {
        station->FlushPendingLostLiveMidiNotes();
        RenderStationBlock(station, block * 128u);
        for (const auto& event : plugin->Events)
        {
            if (event.IsNoteOff() && event.data1 == 67u) ++releases;
            EXPECT_FALSE(event.IsNoteOff() && event.data1 == 60u);
        }
        plugin->Events.clear();
    }
    EXPECT_EQ(1u, releases);
}

TEST(StationMidiInstrument, SourceLossPlaybackReleasePreservesHealthyOwnershipAcrossPublication)
{
    for (const bool healthyPlayback : { false, true })
    {
        SCOPED_TRACE(healthyPlayback ? "playback" : "live");
        auto station = MakeStation("overlapping-playback-loss");
        auto plugin = AddPlugin(station, L"fake-overlap.dll");
        AllowAllMidiChannels(station);
        auto cancelled = MakeMidiTake("cancelled");
        cancelled->Record({}, station->Name(), { 0u }, { "lost" });
        ASSERT_TRUE(cancelled->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "lost", 0u));
        ASSERT_TRUE(cancelled->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), "lost", 0u));
        cancelled->Play(0u, 1000u, 0u);
        station->AddTake(cancelled);
        if (healthyPlayback)
        {
            auto healthy = MakeMidiTake("healthy");
            healthy->Record({}, station->Name(), { 0u }, { "healthy" });
            ASSERT_TRUE(healthy->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "healthy", 0u));
            healthy->Play(0u, 1000u, 0u);
            station->AddTake(healthy);
        }
        else
        {
            const auto on = MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
            station->ObservePhysicalMidiForRecording(on, "healthy");
            ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(on));
        }
        const auto lostLive = MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
        station->ObservePhysicalMidiForRecording(lostLive, "lost");
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(lostLive));
        station->CommitChanges();
        RenderStationBlock(station, 0u, 64u);
        plugin->Events.clear();
        TriggerAction ditch;
        ditch.ActionType = TriggerAction::TRIGGER_DITCH;
        ditch.TargetId = cancelled->Id();
        ditch.IsSourceLossCancellation = true;
        ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
        station->FlushLiveHeldMidiNotesForDevice("lost");
        // Exercise the old published take list as well as its replacement.
        RenderStationBlock(station, 64u, 64u);
        station->CommitChanges();
        RenderStationBlock(station, 128u, 64u);
        station->FlushPendingLostLiveMidiNotes();
        RenderStationBlock(station, 192u, 64u);
        ASSERT_EQ(1u, plugin->Events.size());
        EXPECT_TRUE(plugin->Events[0].IsNoteOff());
        EXPECT_EQ(61u, plugin->Events[0].data1);
    }
}

TEST(StationMidiInstrument, SourceLossPlaybackReleaseUsesItsDestination)
{
    for (const bool rerouteWhileHeld : { false, true })
    {
        SCOPED_TRACE(rerouteWhileHeld ? "rerouted held note" : "original route");
        auto station = MakeStation("routed-playback-loss");
        auto livePlugin = AddPlugin(station, L"fake-live-destination.dll");
        auto playbackPlugin = AddPlugin(station, L"fake-playback-destination.dll");
        AllowAllMidiChannels(station);
        ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{playbackPlugin->StateId()}}, station->RackRevision()));
        ASSERT_TRUE(station->SetMidiChannelRoute(1u, {{livePlugin->StateId()}}, station->RackRevision()));
        auto take = MakeMidiTake("routed-cancelled");
        take->Record({}, station->Name(), { 0u }, { "lost" });
        ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "lost", 0u));
        take->Play(0u, 1000u, 0u);
        station->AddTake(take);
        station->CommitChanges();
        const auto on = MidiEvent::MakeNoteOn(0u, rerouteWhileHeld ? 0u : 1u, 60u, 100u);
        station->ObservePhysicalMidiForRecording(on, "healthy");
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(on));
        RenderStationBlock(station, 0u, 64u);
        ASSERT_EQ(rerouteWhileHeld ? 0u : 1u, livePlugin->Events.size());
        ASSERT_EQ(rerouteWhileHeld ? 2u : 1u, playbackPlugin->Events.size());
        livePlugin->Events.clear();
        playbackPlugin->Events.clear();
        if (rerouteWhileHeld)
        {
            ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{livePlugin->StateId()}}, station->RackRevision()));
            const auto off = MidiEvent::MakeNoteOff(64u, 0u, 60u);
            station->ObservePhysicalMidiForRecording(off, "healthy");
            ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(off));
            RenderStationBlock(station, 64u, 64u);
            ASSERT_EQ(1u, playbackPlugin->Events.size());
            EXPECT_TRUE(playbackPlugin->Events[0].IsNoteOff());
            EXPECT_EQ(60u, playbackPlugin->Events[0].data1);
            station->AcknowledgeAudioBoundary();
            station->ReleaseRetiredAudioStates();
            livePlugin->Events.clear();
            playbackPlugin->Events.clear();
        }
        TriggerAction ditch;
        ditch.ActionType = TriggerAction::TRIGGER_DITCH;
        ditch.TargetId = take->Id();
        ditch.IsSourceLossCancellation = true;
        ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
        RenderStationBlock(station, 64u, 64u);
        station->CommitChanges();
        RenderStationBlock(station, 128u, 64u);
        EXPECT_TRUE(livePlugin->Events.empty());
        if (rerouteWhileHeld)
            EXPECT_TRUE(playbackPlugin->Events.empty()); // Old voice was released at reroute.
        else
        {
            ASSERT_EQ(1u, playbackPlugin->Events.size());
            EXPECT_TRUE(playbackPlugin->Events[0].IsNoteOff());
            EXPECT_EQ(60u, playbackPlugin->Events[0].data1);
        }
    }
}

TEST(StationMidiInstrument, LiveMidiAllowedChannelIsDelivered)
{
	auto station = MakeStation("station-live-allow");
	auto plugin = AddPlugin(station, L"fake-live-allow.dll");
	station->SetAllowedMidiChannels({ 2 });

	auto event = MidiEvent::MakeNoteOn(32u, 1u, 61u, 100u);
	station->TryEnqueueSyntheticLiveMidi(event);

	RenderStationBlock(station, 0u);

	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_EQ(event.status, plugin->Events[0].status);
	EXPECT_EQ(event.data1, plugin->Events[0].data1);
}

TEST(StationMidiInstrument, RemovingAllowedChannelFlushesHeldNoteAndIgnoresLaterExternalRelease)
{
	auto station = MakeStation("station-live-disable");
	auto plugin = AddPlugin(station, L"fake-live-disable.dll");
	station->SetAllowedMidiChannels({ 1 });

	const auto noteOn = MidiEvent::MakeNoteOn(0u, 0u, 62u, 100u);
	station->TryEnqueueImmediateLiveMidi(noteOn);
	station->ObservePhysicalMidiForRecording(noteOn, "Keys");
	RenderStationBlock(station, 0u);
	station->SetAllowedMidiChannels({});
	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOff(16u, 0u, 62u), "Keys");

	RenderStationBlock(station, 128u);

	ASSERT_EQ(2u, plugin->Events.size()) << testing::PrintToString(plugin->Events);
	EXPECT_TRUE(plugin->Events[0].IsNoteOn());
	EXPECT_TRUE(plugin->Events[1].IsNoteOff());
	EXPECT_EQ(62u, plugin->Events[0].data1);
	EXPECT_EQ(62u, plugin->Events[1].data1);
}

TEST(StationMidiInstrument, SameLiveMidiInputCanPlayMultipleStations)
{
	auto stationA = MakeStation("station-a");
	auto stationB = MakeStation("station-b");
	auto pluginA = AddPlugin(stationA, L"fake-a.dll");
	auto pluginB = AddPlugin(stationB, L"fake-b.dll");
	AllowAllMidiChannels(stationA);
	AllowAllMidiChannels(stationB);

	auto event = MidiEvent::MakeNoteOn(10u, 1u, 64u, 96u);
	stationA->TryEnqueueSyntheticLiveMidi(event);
	stationB->TryEnqueueSyntheticLiveMidi(event);

	RenderStationBlock(stationA, 0u);
	RenderStationBlock(stationB, 0u);

	ASSERT_EQ(1u, pluginA->Events.size());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(64u, pluginA->Events[0].data1);
	EXPECT_EQ(64u, pluginB->Events[0].data1);
}

TEST(StationMidiInstrument, LiveMidiCanRouteToSpecificStationPlugin)
{
	auto station = MakeStation("station-route-live");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");
	AllowAllMidiChannels(station);
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{pluginB->StateId()}}, station->RackRevision()));

	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(16u, 0u, 36u, 120u));
	RenderStationBlock(station, 0u);

	EXPECT_TRUE(pluginA->Events.empty());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(36u, pluginB->Events[0].data1);
}

TEST(StationMidiInstrument, RecordedMidiLoopPlaybackCanRouteToSpecificStationPlugin)
{
	auto station = MakeStation("station-route-loop");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");
	auto take = MakeMidiTake("midi-take");
	station->AddTake(take);
	station->CommitChanges();

	take->Record({}, station->Name(), { 0u }, { "Keys" }, {}, 0u, 0u);
	take->RecordMidiEvent(MidiEvent::MakeNoteOn(24u, 0u, 48u, 100u), "Keys", 24u);
	take->Play(0u, 128u, 0u);
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{pluginB->StateId()}}, station->RackRevision()));

	RenderStationBlock(station, 0u);

	RenderStationBlock(station, 128u, 1u); // Artificial closure releases exactly at the seam.

	EXPECT_TRUE(pluginA->Events.empty());
	ASSERT_EQ(2u, pluginB->Events.size());
	EXPECT_TRUE(pluginB->Events[0].IsNoteOn());
	EXPECT_EQ(48u, pluginB->Events[0].data1);
	EXPECT_TRUE(pluginB->Events[1].IsNoteOff());
	EXPECT_EQ(48u, pluginB->Events[1].data1);
	EXPECT_FALSE(pluginB->RealtimeFlags[0]);
	EXPECT_FALSE(pluginB->RealtimeFlags[1]);
}

TEST(StationMidiInstrument, RecordedMidiFollowsRackChannelCables)
{
	auto station = MakeStation("recorded-channel-cables");
	auto pluginA = AddPlugin(station, L"fake-channel-four.dll");
	auto pluginB = AddPlugin(station, L"fake-channel-five.dll");
	auto take = MakeMidiTake("recorded-channel-take");
	station->AddTake(take);
	station->CommitChanges();
	take->Record({}, station->Name(), {3u, 4u}, {"Keys"});
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(24u, 3u, 48u, 100u), "Keys", 24u));
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(24u, 4u, 52u, 100u), "Keys", 24u));
	take->Play(0u, 128u, 0u);
	ASSERT_TRUE(station->SetMidiChannelRoute(3u, {{pluginA->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(4u, {{pluginB->StateId()}}, station->RackRevision()));
	RenderStationBlock(station, 0u);
	ASSERT_FALSE(pluginA->Events.empty());
	ASSERT_FALSE(pluginB->Events.empty());
	for (const auto& event : pluginA->Events) EXPECT_EQ(3u, event.Channel());
	for (const auto& event : pluginB->Events) EXPECT_EQ(4u, event.Channel());
}

TEST(StationMidiInstrument, RegularRecordFinalizationClosesHeldNotesAtLoopEnd)
{
	auto take = MakeMidiTake("regular-record-held-boundary");
	take->Record({}, "station-held-boundary", { 0u }, { "Keys" });

	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(24u, 0u, 48u, 100u), "Keys", 24u));
	take->Play(0u, 128u, 0u);

	ASSERT_EQ(1u, take->GetMidiLoops().size());
	auto midiLoop = take->GetMidiLoops()[0];
	ASSERT_NE(nullptr, midiLoop);
	ASSERT_EQ(2u, midiLoop->EventCount());

	MidiEvent event{};
	ASSERT_TRUE(midiLoop->TryGetEvent(0u, event));
	EXPECT_TRUE(event.IsNoteOn());
	EXPECT_EQ(0u, event.sampleOffset);
	EXPECT_EQ(48u, event.data1);
	EXPECT_EQ(100u, event.data2);

	ASSERT_TRUE(midiLoop->TryGetEvent(1u, event));
	EXPECT_TRUE(event.IsNoteOff());
	EXPECT_EQ(127u, event.sampleOffset);
	EXPECT_EQ(48u, event.data1);
}

TEST(StationMidiInstrument, RecStartSeedsHeldChordIntoRecordingAndPlayback)
{
	auto station = MakeStation("station-recstart-held-seed");
	station->SetAllowedMidiChannels({ 1 });

	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "Keys");
	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 0u, 64u, 110u), "Keys");

	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_REC_START;
	start.MidiSample = 0u;
	start.InputChannels = {};
	start.MidiInputDevices = { "Keys" };
	auto startRes = station->OnAction(start);

	ASSERT_TRUE(startRes.IsEaten);
	ASSERT_FALSE(startRes.TargetId.empty());
	station->CommitChanges();

	std::shared_ptr<LoopTake> take;
	for (const auto& candidate : station->GetLoopTakes())
	{
		if (candidate && candidate->Id() == startRes.TargetId)
		{
			take = candidate;
			break;
		}
	}

	ASSERT_NE(nullptr, take);
	ASSERT_EQ(16u, take->GetMidiLoops().size());
	for (auto channel=1u;channel<16u;++channel) EXPECT_EQ(0u,take->GetMidiLoops()[channel]->EventCount());
	auto midiLoop = take->GetMidiLoops()[0];
	ASSERT_NE(nullptr, midiLoop);

	// Seeded notes must be present while still recording, so model updates can show them immediately.
	EXPECT_EQ(2u, midiLoop->EventCount());

	take->Play(0u, 128u, 0u);
	EXPECT_TRUE(take->MidiTriggerCapturePending());
	station->CompleteMidiIngressPump(128u + 4410u);
	EXPECT_FALSE(take->MidiTriggerCapturePending());
	ASSERT_EQ(4u, midiLoop->EventCount());

	std::array<bool, 2u> foundOn{ false, false };
	std::array<bool, 2u> foundOff{ false, false };
	for (std::size_t i = 0u; i < midiLoop->EventCount(); ++i)
	{
		MidiEvent event{};
		ASSERT_TRUE(midiLoop->TryGetEvent(i, event));
		if (event.data1 == 60u)
		{
			if (event.IsNoteOn() && event.sampleOffset == 0u) foundOn[0] = true;
			if (event.IsNoteOff() && event.sampleOffset == 127u) foundOff[0] = true;
		}
		else if (event.data1 == 64u)
		{
			if (event.IsNoteOn() && event.sampleOffset == 0u) foundOn[1] = true;
			if (event.IsNoteOff() && event.sampleOffset == 127u) foundOff[1] = true;
		}
	}

	EXPECT_TRUE(foundOn[0]);
	EXPECT_TRUE(foundOn[1]);
	EXPECT_TRUE(foundOff[0]);
	EXPECT_TRUE(foundOff[1]);
}

TEST(StationMidiInstrument, RecStartCapturesAllWiredChannelsIndependentlyOfRackMask)
{
	auto station = MakeStation("station-recstart-station-midi-channels");
	station->SetAllowedMidiChannels({ 2 });

	station->ObservePhysicalMidiForRecording(MidiEvent::MakeNoteOn(0u, 1u, 62u, 90u), "Keys");

	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_REC_START;
	start.InputChannels = {};
	start.MidiInputDevices = { "Keys" };
	auto startRes = station->OnAction(start);

	ASSERT_TRUE(startRes.IsEaten);
	ASSERT_FALSE(startRes.TargetId.empty());
	station->CommitChanges();

	std::shared_ptr<LoopTake> take;
	for (const auto& candidate : station->GetLoopTakes())
	{
		if (candidate && candidate->Id() == startRes.TargetId)
		{
			take = candidate;
			break;
		}
	}

	ASSERT_NE(nullptr, take);
	ASSERT_EQ(16u, take->GetMidiLoops().size());
	ASSERT_EQ(1u, take->MidiLoopChannels()[1]);
	ASSERT_EQ("Keys", take->MidiLoopDevices()[1]);

	auto midiLoop = take->GetMidiLoops()[1];
	ASSERT_NE(nullptr, midiLoop);
	ASSERT_EQ(1u, midiLoop->EventCount());

	MidiEvent event{};
	ASSERT_TRUE(midiLoop->TryGetEvent(0u, event));
	EXPECT_TRUE(event.IsNoteOn());
	EXPECT_EQ(1u, event.Channel());
	EXPECT_EQ(62u, event.data1);
	EXPECT_EQ(90u, event.data2);
}

TEST(StationMidiInstrument, ChannelCableReplacementSelectsNewDestination)
{
	auto station = MakeStation("station-route-replace");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");
	auto pluginC = AddPlugin(station, L"fake-c.dll");
	AllowAllMidiChannels(station);

	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{pluginB->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{pluginC->StateId()}}, station->RackRevision()));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(8u, 0u, 40u, 100u));

	RenderStationBlock(station, 0u);

	EXPECT_TRUE(pluginA->Events.empty());
	EXPECT_TRUE(pluginB->Events.empty());
	ASSERT_EQ(1u, pluginC->Events.size());
	EXPECT_EQ(40u, pluginC->Events[0].data1);
}

TEST(StationMidiInstrument, RemovingLastChannelCableRestoresBroadcast)
{
	auto station = MakeStation("station-route-clear");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");
	AllowAllMidiChannels(station);

	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{pluginB->StateId()}}, station->RackRevision()));
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {}, station->RackRevision()));
	station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(8u, 0u, 41u, 100u));

	RenderStationBlock(station, 0u);

	ASSERT_EQ(1u, pluginA->Events.size());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(41u, pluginA->Events[0].data1);
	EXPECT_EQ(41u, pluginB->Events[0].data1);
}

TEST(StationMidiInstrument, OverdubStartPassesSourceMidiToTargetTake)
{
	auto station = MakeStation("station-overdub-source");
	station->SetAllowedMidiChannels({ 4 });
	auto sourceTake = MakeMidiTake("source-midi-take");
	station->AddTake(sourceTake);
	station->CommitChanges();

	sourceTake->Record({}, station->Name(), { 3u }, { "Keys" });
	sourceTake->EndMultiWrite(100u, true, base::Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "Keys", 100u));
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "Keys", 100u));
	sourceTake->Play(0u, 100u, 0u);
	sourceTake->BeginCaptureAudioBoundary(0u, 0u, 0u);

	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_OVERDUB_START;
	start.MidiSample = 0u;
	start.InputChannels = {};
	start.MidiInputDevices = { "Keys" };
	auto result = station->OnAction(start);

	ASSERT_TRUE(result.IsEaten);
	ASSERT_EQ(sourceTake->Id(), result.SourceId);
	ASSERT_FALSE(result.TargetId.empty());

	station->CommitChanges();

	std::shared_ptr<LoopTake> targetTake;
	for (const auto& take : station->GetLoopTakes())
	{
		if (take && take->Id() == result.TargetId)
		{
			targetTake = take;
			break;
		}
	}

	ASSERT_NE(nullptr, targetTake);
	ASSERT_EQ(16u, targetTake->GetMidiLoops().size());
	ASSERT_EQ(3u, targetTake->MidiLoopChannels()[3]);
	ASSERT_EQ("Keys", targetTake->MidiLoopDevices()[3]);

	targetTake->BeginCaptureAudioBoundary(0u, 0u, 0u);
	targetTake->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);
	targetTake->BeginCaptureAudioBoundary(20u, 20u, 20u);
	targetTake->PunchIn();
	targetTake->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);
	targetTake->BeginCaptureAudioBoundary(40u, 40u, 40u);
	targetTake->PunchOut();
	targetTake->Play(0u, 100u, 0u);
	station->CompleteMidiIngressPump(100u + 4410u);

	ASSERT_EQ(4u, targetTake->GetMidiLoops()[3]->EventCount());
	MidiEvent event{};
	ASSERT_TRUE(targetTake->GetMidiLoops()[3]->TryGetEvent(0u, event));
	EXPECT_EQ(10u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[3]->TryGetEvent(1u, event));
	EXPECT_EQ(20u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[3]->TryGetEvent(2u, event));
	EXPECT_EQ(40u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[3]->TryGetEvent(3u, event));
	EXPECT_EQ(90u, event.sampleOffset);
}

TEST(StationMidiInstrument, PunchBoundariesEmitLiveMidiTransitionsForSourceAndLiveHeldNotes)
{
	auto station = MakeStation("station-punch-live-boundary");
	auto plugin = AddPlugin(station, L"fake-punch-live.dll");
	AllowAllMidiChannels(station);
	auto sourceTake = MakeMidiTake("source-punch-live");
	station->AddTake(sourceTake);
	station->CommitChanges();

	sourceTake->Record({}, station->Name(), { 3u }, { "Keys" });
	sourceTake->EndMultiWrite(100u, true, base::Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "Keys", 100u));
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "Keys", 100u));
	sourceTake->Play(0u, 100u, 0u);

	auto targetTake = MakeMidiTake("target-punch-live");
	targetTake->Overdub({}, station->Name(), { 3u }, { "Keys" }, sourceTake);
	station->AddTake(targetTake);
	station->CommitChanges();

	targetTake->EndMultiWrite(15u, true, base::Audible::AUDIOSOURCE_ADC);
	EXPECT_FALSE(targetTake->RecordMidiEvent(MidiEvent::MakeNoteOn(15u, 3u, 62u, 110u), "Keys", 15u));
	targetTake->EndMultiWrite(5u, true, base::Audible::AUDIOSOURCE_ADC);

	TriggerAction punchIn;
	punchIn.ActionType = TriggerAction::TRIGGER_PUNCHIN_START;
	punchIn.TargetId = targetTake->Id();
	punchIn.SourceId = sourceTake->Id();
	punchIn.SampleCount = 20u;
	punchIn.ApplyToTargetTake = true;
	punchIn.ApplyToSourceTake = true;
	punchIn.ApplyToTargetAudio = false;
	punchIn.ApplyToTargetMidi = true;
	station->OnAction(punchIn);

	RenderStationBlock(station, 20u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events[0].IsNoteOff());
	EXPECT_EQ(60u, plugin->Events[0].data1);
	EXPECT_TRUE(plugin->RealtimeFlags[0]);

	plugin->Events.clear();
	plugin->RealtimeFlags.clear();
	targetTake->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);

	TriggerAction punchOut;
	punchOut.ActionType = TriggerAction::TRIGGER_PUNCHIN_END;
	punchOut.TargetId = targetTake->Id();
	punchOut.SourceId = sourceTake->Id();
	punchOut.SampleCount = 40u;
	punchOut.ApplyToTargetTake = true;
	punchOut.ApplyToSourceTake = true;
	punchOut.ApplyToTargetAudio = false;
	punchOut.ApplyToTargetMidi = true;
	station->OnAction(punchOut);

	RenderStationBlock(station, 40u);
	ASSERT_GE(plugin->Events.size(), 2u);
	EXPECT_TRUE(plugin->Events[0].IsNoteOff());
	EXPECT_EQ(62u, plugin->Events[0].data1);
	EXPECT_TRUE(plugin->Events[1].IsNoteOn());
	EXPECT_EQ(60u, plugin->Events[1].data1);
	EXPECT_EQ(100u, plugin->Events[1].data2);
	EXPECT_TRUE(plugin->RealtimeFlags[0]);
	EXPECT_TRUE(plugin->RealtimeFlags[1]);
}

TEST(StationMidiInstrument, AudioOnlyTriggerDoesNotCreateMidiStreams)
{
	for (const auto actionType : { TriggerAction::TRIGGER_REC_START,
		TriggerAction::TRIGGER_OVERDUB_START })
	{
		SCOPED_TRACE(static_cast<int>(actionType));
		auto station = MakeStation("station-audio-only-trigger");
		station->SetAllowedMidiChannels({ 1, 4 });

		// Even a MIDI source take must not turn audio-only overdub into MIDI capture.
		if (actionType == TriggerAction::TRIGGER_OVERDUB_START)
		{
			auto sourceTake = MakeMidiTake("source-midi-take");
			sourceTake->Record({}, station->Name(), { 3u }, { "Keys" });
			sourceTake->Play(0u, 100u, 0u);
			station->AddTake(sourceTake);
			station->CommitChanges();
		}

		TriggerAction start;
		start.ActionType = actionType;
		start.InputChannels = { 0u };
		start.MidiInputDevices = {};
		const auto result = station->OnAction(start);
		ASSERT_TRUE(result.IsEaten);
		station->CommitChanges();

		std::shared_ptr<LoopTake> targetTake;
		for (const auto& take : station->GetLoopTakes())
			if (take && take->Id() == result.TargetId)
				targetTake = take;
		ASSERT_NE(nullptr, targetTake);
		EXPECT_EQ(1u, targetTake->GetLoops().size());
		EXPECT_TRUE(targetTake->GetMidiLoops().empty());
		EXPECT_TRUE(targetTake->MidiLoopChannels().empty());
		EXPECT_TRUE(targetTake->MidiLoopDevices().empty());
	}
}

TEST(StationMidiInstrument, ManyHeldReleasesRetryAcrossBoundedPluginBlocks)
{
	auto station = MakeStation("many-release-recovery");
	auto plugin = AddPlugin(station, L"fake-many-release.dll");
	plugin->MaxAcceptedPerBlock = 256u;
	auto take = MakeMidiTake("many-release-take");
	take->Record({}, station->Name(), { 0u }, { "Keys" });
	take->Play(0u, 1024u, 0u);
	std::vector<MidiEvent> attacks;
	for (unsigned int i = 0u; i < 300u; ++i)
		attacks.push_back(MidiEvent::MakeNoteOn(i < 150u ? 0u : 128u,
			static_cast<std::uint8_t>(i / 128u), static_cast<std::uint8_t>(i % 128u), 100u));
	auto loop = take->GetMidiLoops().at(0u);
	loop->ReplaceRecordedEvents(attacks.data(), attacks.size(), 1024u);
	station->AddTake(take);
	station->CommitChanges();
	RenderStationBlock(station, 0u);
	RenderStationBlock(station, 128u);
	ASSERT_EQ(300u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOn(); }));

	take->Mute();
	RenderStationBlock(station, 256u);
	EXPECT_TRUE(loop->HeldNotes().none());
	EXPECT_EQ(256u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOff(); }));
	RenderStationBlock(station, 384u);
	EXPECT_EQ(300u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOff(); }));
}

TEST(StationMidiInstrument, PartialFanoutAcceptanceRetainsTheAudibleLoopNote)
{
	auto station = MakeStation("partial-acceptance");
	auto rejected = AddPlugin(station, L"fake-rejected.dll");
	auto accepted = AddPlugin(station, L"fake-accepted.dll");
	rejected->MaxAcceptedPerBlock = 0u;
	auto take = MakeMidiTake("partial-acceptance-take");
	station->AddTake(take);
	station->CommitChanges();
	take->Record({}, station->Name(), { 0u }, { "Keys" });
	take->Play(0u, 256u, 0u);
	const auto attack = MidiEvent::MakeNoteOn(10u, 0u, 60u, 113u);
	auto loop = take->GetMidiLoops().at(0u);
	loop->ReplaceRecordedEvents(&attack, 1u, 256u);
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {}, station->RackRevision()));
	RenderStationBlock(station, 0u);
	EXPECT_TRUE(rejected->ProcessedEvents.empty());
	ASSERT_EQ(1u, accepted->ProcessedEvents.size());
	EXPECT_TRUE(accepted->ProcessedEvents[0].IsNoteOn());
	EXPECT_TRUE(loop->HeldNotes().test(midi::MidiLoop::NoteSlot(0u, 60u)));
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u);
	EXPECT_EQ(2u, std::count_if(accepted->ProcessedEvents.begin(), accepted->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOn(); }));
	ASSERT_EQ(3u, accepted->ProcessedEvents.size());
	EXPECT_TRUE(accepted->ProcessedEvents[1].IsNoteOff());
	EXPECT_EQ(256u, accepted->ProcessedEvents[1].sampleOffset);
	EXPECT_TRUE(accepted->ProcessedEvents[2].IsNoteOn());
	EXPECT_EQ(266u, accepted->ProcessedEvents[2].sampleOffset);
}

TEST(StationMidiInstrument, SkippedPluginBlockFlushesContinuationAndDropsStaleController)
{
	auto station = MakeStation("skipped-editor-block");
	auto plugin = AddPlugin(station, L"fake-skipped-editor.dll");
	auto take = MakeMidiTake("skipped-editor-take");
	take->Record({}, station->Name(), { 0u }, { "Keys" });
	take->Play(0u, 256u, 0u);
	const std::array source = { MidiEvent::MakeNoteOn(10u, 0u, 60u, 113u),
		MidiEvent{ 20u, 0xB0u, 74u, 99u, 0u } };
	auto loop = take->GetMidiLoops().at(0u);
	loop->ReplaceRecordedEvents(source.data(), source.size(), 256u);
	station->AddTake(take);
	station->CommitChanges();
	plugin->SkipNextProcess = true;
	RenderStationBlock(station, 0u);
	EXPECT_TRUE(plugin->ProcessedEvents.empty());
	RenderStationBlock(station, 128u);
	EXPECT_TRUE(loop->HeldNotes().none());
	EXPECT_EQ(0u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.MessageType() == 0xB0u; }));
	RenderStationBlock(station, 256u);
	const auto on = std::find_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOn(); });
	ASSERT_NE(plugin->ProcessedEvents.end(), on);
	EXPECT_EQ(266u, on->sampleOffset);
	const auto cc = std::find_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.MessageType() == 0xB0u; });
	ASSERT_NE(plugin->ProcessedEvents.end(), cc);
	EXPECT_EQ(276u, cc->sampleOffset);
}

TEST(StationMidiInstrument, RecordedMidiIsDelayedWhileLiveInputIsImmediate)
{
	auto station = MakeStation("recorded-delay-live-immediate");
	auto plugin = AddPlugin(station, L"fake-recorded-delay.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("recorded-delay");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const std::array events{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(64u, 0u, 60u) };
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(events.data(), events.size(), 1000u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(8u, 0u, 62u, 100u)));
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->ProcessedEvents.size());
	EXPECT_EQ(62u, plugin->ProcessedEvents[0].data1);
	EXPECT_EQ(8u, plugin->ProcessedEvents[0].sampleOffset);
	ASSERT_EQ(1u, plugin->RealtimeFlags.size());
	EXPECT_TRUE(plugin->RealtimeFlags[0]);
	RenderStationBlock(station, 128u);
	EXPECT_EQ(1u, plugin->ProcessedEvents.size());
	RenderStationBlock(station, 256u);
	ASSERT_EQ(3u, plugin->ProcessedEvents.size());
	EXPECT_EQ(60u, plugin->ProcessedEvents[1].data1);
	EXPECT_EQ(256u, plugin->ProcessedEvents[1].sampleOffset);
	EXPECT_TRUE(plugin->ProcessedEvents[2].IsNoteOff());
	EXPECT_EQ(320u, plugin->ProcessedEvents[2].sampleOffset);
	EXPECT_FALSE(plugin->RealtimeFlags[1]);
	EXPECT_FALSE(plugin->RealtimeFlags[2]);
}

TEST(StationMidiInstrument, DisconnectingChannelCableCancelsDelayedMidiAndPreservesOtherOwners)
{
	for (const auto disconnectSample : {128u, 384u})
		for (const bool survivingOwners : {false, true})
		{
			SCOPED_TRACE(disconnectSample);
			SCOPED_TRACE(survivingOwners);
			auto station = MakeStation("disconnect-delayed-channel");
			auto pluginA = AddPlugin(station, L"fake-disconnected-channel.dll");
			auto pluginB = AddPlugin(station, L"fake-surviving-channel.dll");
			AllowAllMidiChannels(station);
			auto take = MakeMidiTake("disconnected-channel-take");
			take->Record({}, station->Name(), {0u}, {"source"});
			take->Play(0u, 1000u, 0u);
			const std::array events{
				MidiEvent{0u, 0xb0u, 64u, 127u}, MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
				MidiEvent{0u, 0xb1u, 64u, 127u}, MidiEvent::MakeNoteOn(0u, 1u, 62u, 100u),
				MidiEvent{128u, 0xb0u, 64u, 0u}, MidiEvent::MakeNoteOff(512u, 0u, 60u),
				MidiEvent{512u, 0xb1u, 64u, 0u}, MidiEvent::MakeNoteOff(512u, 1u, 62u)};
			take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(events.data(), events.size(), 1000u);
			station->AddTake(take);
			if (survivingOwners)
			{
				auto explicitTake = MakeMidiTake("surviving-channel-owner");
				explicitTake->Record({}, station->Name(), {0u}, {"source"});
				explicitTake->Play(0u, 1000u, 0u);
				const std::array explicitEvents{
					MidiEvent{0u, 0xb0u, 64u, 127u}, MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
					MidiEvent{640u, 0xb0u, 64u, 0u}, MidiEvent::MakeNoteOff(640u, 0u, 60u)};
				explicitTake->GetMidiLoops().at(0u)->ReplaceRecordedEvents(
					explicitEvents.data(), explicitEvents.size(), 1000u);
				station->AddTake(explicitTake);
			}
			station->CommitChanges();
			std::vector<std::string> targets{pluginA->StateId()};
			if (survivingOwners) targets.push_back(pluginB->StateId());
			ASSERT_TRUE(station->SetMidiChannelRoute(0u, {targets}, station->RackRevision()));
			ASSERT_TRUE(station->SetMidiChannelRoute(1u, {{pluginA->StateId()}}, station->RackRevision()));
			ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
			ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 1u, 64u, 100u)));
			ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi({0u, 0xb1u, 64u, 127u}));
			for (auto sample = 0u; sample < disconnectSample; sample += 128u)
				RenderStationBlock(station, sample);
			// At 128 the channel-zero pedal-down is still queued; at 384 it is audible.
			targets = {pluginB->StateId()};
			ASSERT_TRUE(station->SetMidiChannelRoute(0u, {targets}, station->RackRevision()));
			for (auto sample = disconnectSample; sample <= 640u; sample += 128u)
				RenderStationBlock(station, sample);
			auto count = [](const auto& plugin, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
				return std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
					[=](const MidiEvent& event) { return event.status == status && event.data1 == data1 && event.data2 == data2; });
			};
			const auto attacks = (disconnectSample == 384u ? (survivingOwners ? 2 : 1) : 0);
			EXPECT_EQ(attacks, count(pluginA, 0x90u, 60u, 100u));
			EXPECT_EQ(attacks, count(pluginA, 0xb0u, 64u, 127u));
			const auto releases = disconnectSample == 384u ? 1 : 0;
			EXPECT_EQ(releases, count(pluginA, 0x80u, 60u, 0u));
			EXPECT_EQ(releases, count(pluginA, 0xb0u, 64u, 0u));
			EXPECT_EQ(1, count(pluginA, 0x91u, 62u, 100u));
			EXPECT_EQ(1, count(pluginA, 0x91u, 64u, 100u));
			EXPECT_EQ(0, count(pluginA, 0x81u, 62u, 0u));
			EXPECT_EQ(0, count(pluginA, 0x81u, 64u, 0u));
			EXPECT_EQ(0, count(pluginA, 0xb1u, 64u, 0u));
			EXPECT_EQ(survivingOwners ? 2 : 0, count(pluginB, 0x90u, 60u, 100u));
			EXPECT_EQ(0, count(pluginB, 0x80u, 60u, 0u));
			EXPECT_EQ(0, count(pluginB, 0xb0u, 64u, 0u));
			ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(768u, 1u, 64u)));
			ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi({768u, 0xb1u, 64u, 0u}));
			RenderStationBlock(station, 768u);
			RenderStationBlock(station, 896u);
			EXPECT_EQ(1, count(pluginA, 0x81u, 62u, 0u));
			EXPECT_EQ(1, count(pluginA, 0x81u, 64u, 0u));
			EXPECT_EQ(1, count(pluginA, 0xb1u, 64u, 0u));
			EXPECT_EQ(releases, count(pluginA, 0x80u, 60u, 0u));
			EXPECT_EQ(releases, count(pluginA, 0xb0u, 64u, 0u));
			EXPECT_EQ(survivingOwners ? 1 : 0, count(pluginB, 0x80u, 60u, 0u));
			EXPECT_EQ(survivingOwners ? 1 : 0, count(pluginB, 0xb0u, 64u, 0u));
		}
}

TEST(StationMidiInstrument, MutingCancelsFutureRecordedAttackBeforeDelivery)
{
	auto station = MakeStation("cancel-future-recorded");
	auto plugin = AddPlugin(station, L"fake-future-cancel.dll");
	auto take = MakeMidiTake("future-cancel");
	take->Record({}, station->Name(), { 0u }, { "source" });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "source", 0u));
	take->Play(0u, 1000u, 0u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	EXPECT_TRUE(plugin->ProcessedEvents.empty());
	ASSERT_TRUE(take->Mute());
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u);
	EXPECT_TRUE(std::none_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOn() && event.data1 == 60u; }));
}

TEST(PlaybackDelay, PreservesImpulseOffsetAcrossBlockSplitsAndClear)
{
	audio::PlaybackDelay delay;
	ASSERT_TRUE(delay.Configure(5u));
	std::array<float, 3u> first{ 1.0f, 0.0f, 0.0f };
	std::array<float, 4u> second{};
	delay.Process(first.data(), static_cast<unsigned int>(first.size()));
	delay.Process(second.data(), static_cast<unsigned int>(second.size()));
	EXPECT_EQ(0.0f, first[0]);
	EXPECT_EQ(0.0f, first[1]);
	EXPECT_EQ(1.0f, second[2]);
	std::array<float, 3u> beforeClear{ 2.0f, 0.0f, 0.0f };
	delay.Process(beforeClear.data(), static_cast<unsigned int>(beforeClear.size()));
	delay.Clear();
	std::array<float, 6u> afterClear{};
	delay.Process(afterClear.data(), static_cast<unsigned int>(afterClear.size()));
	EXPECT_TRUE(std::all_of(afterClear.begin(), afterClear.end(),
		[](float value) { return value == 0.0f; }));
}

TEST(StationMidiInstrument, RecordedRouteDelayUsesDestinationSuffixLatency)
{
	for (const auto routeIndex : { 0u, 1u })
	{
		auto station = MakeStation("suffix-route-" + std::to_string(routeIndex));
		auto first = AddPlugin(station, L"fake-first-suffix.dll");
		auto second = AddPlugin(station, L"fake-second-suffix.dll");
		first->LatencySamps = 64;
		second->LatencySamps = 0;
		auto take = MakeMidiTake("suffix-take");
		take->Record({}, station->Name(), { 0u }, { "source" });
		ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "source", 0u));
		take->Play(0u, 1000u, 0u);
		station->AddTake(take);
		ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{(routeIndex == 0u ? first : second)->StateId()}}, station->RackRevision()));
		station->CommitChanges();
		ASSERT_EQ(64u, station->RecordedPlaybackLatencySamps());
		ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(128u));
		RenderStationBlock(station, 0u, 64u);
		EXPECT_TRUE(first->ProcessedEvents.empty());
		EXPECT_TRUE(second->ProcessedEvents.empty());
		RenderStationBlock(station, 64u, 64u);
		if (routeIndex == 0u)
		{
			ASSERT_EQ(1u, first->ProcessedEvents.size());
			EXPECT_EQ(64u, first->ProcessedEvents[0].sampleOffset);
			EXPECT_TRUE(second->ProcessedEvents.empty());
		}
		else
		{
			EXPECT_TRUE(first->ProcessedEvents.empty());
			EXPECT_TRUE(second->ProcessedEvents.empty());
		}
		RenderStationBlock(station, 128u, 64u);
		if (routeIndex == 1u)
		{
			ASSERT_EQ(1u, second->ProcessedEvents.size());
			EXPECT_EQ(128u, second->ProcessedEvents[0].sampleOffset);
			EXPECT_TRUE(first->ProcessedEvents.empty());
		}
	}
}

TEST(PlaybackDelay, RepeatedLatencyChangesRetainAtMostTwoProcessors)
{
	audio::PlaybackDelay delay;
	for (auto latency = 1u; latency <= 64u; ++latency)
	{
		ASSERT_TRUE(delay.Configure(latency));
		std::array<float, audio::PlaybackDelay::TransitionSamps> silence{};
		delay.Process(silence.data(), static_cast<unsigned int>(silence.size()));
		EXPECT_LE(delay.PublishedProcessorCount(), 2u);
	}
	ASSERT_TRUE(delay.Configure(64u));
	EXPECT_EQ(1u, delay.PublishedProcessorCount());
	EXPECT_EQ(64u, delay.ConfiguredDelaySamps());
}

TEST(StationMidiInstrument, RenderedRecordedImpulsesAlignAcrossUnequalPluginLatency)
{
	std::array<std::vector<float>, 2u> rendered;
	for (auto path = 0u; path < 2u; ++path)
	{
		auto station = MakeStation("rendered-f2-" + std::to_string(path));
		auto plugin = AddPlugin(station, L"fake-rendered-latency.dll");
		plugin->RenderMidiImpulses = true;
		plugin->RenderAudioDelay = path == 0u;
		plugin->LatencySamps = path == 0u ? 64 : 0;
		auto take = MakeMidiTake("rendered-note");
		take->Record({}, station->Name(), { 0u }, { "source" });
		ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "source", 0u));
		take->Play(0u, 1000u, 0u);
		station->AddTake(take);
		station->CommitChanges();
		ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(64u));
		for (auto block = 0u; block < 3u; ++block)
		{
			const auto samples = RenderStationAudioBlock(station, block * 64u, 64u);
			rendered[path].insert(rendered[path].end(), samples.begin(), samples.end());
		}
	}
	ASSERT_EQ(192u, rendered[0].size());
	ASSERT_EQ(rendered[0].size(), rendered[1].size());
	for (auto sample = 0u; sample < rendered[0].size(); ++sample)
	{
		if (sample == 64u)
		{
			EXPECT_GT(rendered[0][sample], 0.0f);
			EXPECT_GT(rendered[1][sample], 0.0f);
		}
		else
		{
			EXPECT_NEAR(0.0f, rendered[0][sample], 1.0e-5f);
			EXPECT_NEAR(0.0f, rendered[1][sample], 1.0e-5f);
		}
	}
}

TEST(StationMidiInstrument, LatencyChangeCancelsOldFutureAttackAndAllowsNextIteration)
{
	auto station = MakeStation("latency-epoch-cancel");
	auto plugin = AddPlugin(station, L"fake-latency-epoch.dll");
	auto take = MakeMidiTake("latency-epoch-take");
	take->Record({}, station->Name(), { 0u }, { "source" });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "source", 0u));
	take->Play(0u, 1000u, 0u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	EXPECT_TRUE(plugin->ProcessedEvents.empty());
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(0u));
	for (auto block = 128u; block <= 896u; block += 128u)
		RenderStationBlock(station, block);
	unsigned int attacks = 0u;
	for (const auto& event : plugin->ProcessedEvents)
		if (event.IsNoteOn() && event.data1 == 60u)
		{
			++attacks;
			EXPECT_EQ(1000u, event.sampleOffset);
		}
	EXPECT_EQ(1u, attacks);
}

TEST(StationMidiInstrument, FullyQueuedShortNoteIsCancelledBeforeDelivery)
{
	enum class Change { Mute, Cancel, Replace, Reset, StartRecord, Relocate };
	for (const auto change : { Change::Mute, Change::Cancel, Change::Replace,
		Change::Reset, Change::StartRecord, Change::Relocate })
	{
		SCOPED_TRACE(static_cast<int>(change));
		auto station = MakeStation("short-queue-cancel");
		auto plugin = AddPlugin(station, L"fake-short-queue.dll");
		auto take = MakeMidiTake("short-queue");
		take->Record({}, station->Name(), { 0u }, { "source" });
		take->Play(0u, 1000u, 0u);
		const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
			MidiEvent::MakeNoteOff(64u, 0u, 60u) };
		auto loop = take->GetMidiLoops().at(0u);
		loop->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
		station->AddTake(take);
		station->CommitChanges();
		ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
		RenderStationBlock(station, 0u);
		ASSERT_TRUE(plugin->ProcessedEvents.empty());
		switch (change)
		{
		case Change::Mute: ASSERT_TRUE(take->Mute()); break;
		case Change::Cancel: take->CancelCapture(false); break;
		case Change::Replace:
		{
			const auto replacement = MidiEvent::MakeNoteOn(500u, 0u, 62u, 100u);
			loop->ReplaceRecordedEvents(&replacement, 1u, 1000u);
			break;
		}
		case Change::Reset: loop->Reset(); break;
		case Change::StartRecord: loop->StartRecord(); break;
		case Change::Relocate: loop->RequestRelocationChase(); break;
		}
		RenderStationBlock(station, 128u);
		RenderStationBlock(station, 256u);
		EXPECT_TRUE(std::none_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
			[](const MidiEvent& event) { return event.IsNoteOn() && event.data1 == 60u; }));
	}
}

TEST(StationMidiInstrument, RerouteAndBypassCancelFullyQueuedShortNote)
{
	for (const bool bypass : { false, true })
	{
		SCOPED_TRACE(bypass);
		auto station = MakeStation("queue-route-bypass");
		auto oldPlugin = AddPlugin(station, L"fake-old-route.dll");
		auto newPlugin = AddPlugin(station, L"fake-new-route.dll");
		auto take = MakeMidiTake("route-queued");
		take->Record({}, station->Name(), { 0u }, { "source" });
		take->Play(0u, 1000u, 0u);
		const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
			MidiEvent::MakeNoteOff(64u, 0u, 60u) };
		take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
		station->AddTake(take);
		ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{oldPlugin->StateId()}}, station->RackRevision()));
		station->CommitChanges();
		ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
		RenderStationBlock(station, 0u);
		ASSERT_TRUE(oldPlugin->ProcessedEvents.empty());
		if (bypass) oldPlugin->SetBypassed(true);
		else
		{
			ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{newPlugin->StateId()}}, station->RackRevision()));
			station->CommitChanges();
		}
		RenderStationBlock(station, 128u);
		if (bypass) oldPlugin->SetBypassed(false);
		RenderStationBlock(station, 256u);
		EXPECT_TRUE(std::none_of(oldPlugin->ProcessedEvents.begin(), oldPlugin->ProcessedEvents.end(),
			[](const MidiEvent& event) { return event.IsNoteOn() && event.data1 == 60u; }));
		EXPECT_TRUE(newPlugin->ProcessedEvents.empty());
	}
}

TEST(StationMidiInstrument, RecordedLatencyChangePreservesHeldLiveVoice)
{
	auto station = MakeStation("live-epoch");
	auto plugin = AddPlugin(station, L"fake-live-epoch.dll");
	AllowAllMidiChannels(station);
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 67u, 100u)));
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->ProcessedEvents.size());
	ASSERT_TRUE(plugin->ProcessedEvents[0].IsNoteOn());
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(0u));
	RenderStationBlock(station, 128u);
	ASSERT_EQ(1u, plugin->ProcessedEvents.size());
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(256u, 0u, 67u)));
	RenderStationBlock(station, 256u);
	ASSERT_EQ(2u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[1].IsNoteOff());
}

TEST(StationMidiInstrument, RejectedDelayedAttackDoesNotFlushAnotherTake)
{
	auto station = MakeStation("targeted-delayed-rejection");
	auto plugin = AddPlugin(station, L"fake-targeted-rejection.dll");
	for (const auto note : { 60u, 62u })
	{
		auto take = MakeMidiTake("targeted-" + std::to_string(note));
		take->Record({}, station->Name(), { 0u }, { "source" });
		take->Play(0u, 1000u, 0u);
		const auto attack = MidiEvent::MakeNoteOn(note == 60u ? 0u : 128u,
			0u, static_cast<std::uint8_t>(note), 100u);
		take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(&attack, 1u, 1000u);
		station->AddTake(take);
	}
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u);
	ASSERT_EQ(1u, plugin->ProcessedEvents.size());
	ASSERT_TRUE(plugin->ProcessedEvents[0].IsNoteOn());
	EXPECT_EQ(60u, plugin->ProcessedEvents[0].data1);
	plugin->MaxAcceptedPerBlock = 0u;
	RenderStationBlock(station, 384u); // The second take's delayed attack is rejected.
	plugin->MaxAcceptedPerBlock = static_cast<std::size_t>(-1);
	RenderStationBlock(station, 512u);
	EXPECT_EQ(0u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOff() && event.data1 == 60u; }));
}

TEST(StationMidiInstrument, RemovedDestinationCancelsQueuedShortNoteAndReclaimsPlugin)
{
	auto station = MakeStation("retired-delayed-plugin");
	auto plugin = AddPlugin(station, L"fake-retired-delayed.dll");
	auto take = MakeMidiTake("retired-delayed-take");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(64u, 0u, 60u) };
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	ASSERT_TRUE(plugin->ProcessedEvents.empty());
	JobAction unload;
	unload.JobActionType = JobAction::JOB_UNLOADVST;
	unload.VstIndex = 0u;
	unload.VstIdentity = plugin->StateId();
	ASSERT_TRUE(station->StageVstJob(unload));
	station->OnAction(unload);
	station->CommitChanges();
	RenderStationBlock(station, 128u); // Retired destination queue is discarded even with no active chain.
	station->AcknowledgeAudioBoundary();
	station->ReleaseRetiredAudioStates();
	RenderStationBlock(station, 256u);
	station->AcknowledgeAudioBoundary();
	station->ReleaseRetiredAudioStates();
	EXPECT_TRUE(plugin->ProcessedEvents.empty());
	std::weak_ptr<vst::IVstPlugin> lifetime = plugin;
	plugin.reset();
	station->ForceUnloadAllVstPlugins();
	vst::DrainUiThreadDestroyQueue();
	EXPECT_TRUE(lifetime.expired());
}

TEST(PlaybackDelay, ChangedTargetWaitsForCurrentTransition)
{
	audio::PlaybackDelay delay;
	ASSERT_TRUE(delay.Configure(0u));
	ASSERT_TRUE(delay.Configure(32u));
	EXPECT_FALSE(delay.CanConfigure(64u));
	EXPECT_FALSE(delay.Configure(64u));
	EXPECT_EQ(32u, delay.ConfiguredDelaySamps());
	std::array<float, audio::PlaybackDelay::TransitionSamps> silence{};
	delay.Process(silence.data(), static_cast<unsigned int>(silence.size()));
	EXPECT_TRUE(delay.CanConfigure(64u));
	EXPECT_TRUE(delay.Configure(64u));
	EXPECT_EQ(64u, delay.ConfiguredDelaySamps());
}

TEST(StationMidiInstrument, LiveHeldKeyPreservesRecordedAttackAndDelaysRecordedRelease)
{
	auto station = MakeStation("live-over-recorded-delay");
	auto plugin = AddPlugin(station, L"fake-live-over-recorded.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("live-over-recorded");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(256u, 0u, 60u) };
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(128u, 0u, 60u, 100u)));
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u); // Recorded attack remains audible while live is held.
	RenderStationBlock(station, 384u);
	RenderStationBlock(station, 512u); // Recorded release cannot cut the live key.
	ASSERT_EQ(2u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[0].IsNoteOn());
	EXPECT_TRUE(plugin->ProcessedEvents[1].IsNoteOn());
	EXPECT_EQ(128u, plugin->ProcessedEvents[0].sampleOffset);
	EXPECT_EQ(256u, plugin->ProcessedEvents[1].sampleOffset);
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(640u, 0u, 60u)));
	RenderStationBlock(station, 640u);
	ASSERT_EQ(3u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[2].IsNoteOff());
}

TEST(StationMidiInstrument, LiveReleaseWaitsForAudibleDelayedRecordedOwner)
{
	auto station = MakeStation("recorded-over-live-release");
	auto plugin = AddPlugin(station, L"fake-recorded-over-live.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("recorded-over-live");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(256u, 0u, 60u) };
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u); // Recorded voice is now audible.
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(384u, 0u, 60u, 100u)));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(448u, 0u, 60u)));
	RenderStationBlock(station, 384u);
	EXPECT_EQ(0u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOff() && event.data1 == 60u; }));
	RenderStationBlock(station, 512u);
	EXPECT_EQ(1u, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
		[](const MidiEvent& event) { return event.IsNoteOff() && event.data1 == 60u; }));
}

TEST(StationMidiInstrument, FutureRecordedOwnerDoesNotDelayLiveRelease)
{
	auto station = MakeStation("live-before-future-recorded");
	auto plugin = AddPlugin(station, L"fake-live-before-future.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("future-owner");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const auto attack = MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(&attack, 1u, 1000u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u); // Recorded attack is queued for 256.
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(128u, 0u, 60u, 100u)));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(192u, 0u, 60u)));
	RenderStationBlock(station, 128u);
	ASSERT_EQ(2u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[0].IsNoteOn());
	EXPECT_TRUE(plugin->ProcessedEvents[1].IsNoteOff());
	RenderStationBlock(station, 256u);
	EXPECT_EQ(3u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[2].IsNoteOn());
}

TEST(StationMidiInstrument, ReroutingAudibleUnmatchedNoteReleasesOldDestination)
{
	auto station = MakeStation("reroute-audible-recorded");
	auto oldPlugin = AddPlugin(station, L"fake-reroute-old.dll");
	auto newPlugin = AddPlugin(station, L"fake-reroute-new.dll");
	auto take = MakeMidiTake("reroute-audible-take");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const auto attack = MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(&attack, 1u, 1000u);
	station->AddTake(take);
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{oldPlugin->StateId()}}, station->RackRevision()));
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u);
	ASSERT_EQ(1u, oldPlugin->ProcessedEvents.size());
	ASSERT_TRUE(oldPlugin->ProcessedEvents[0].IsNoteOn());
	ASSERT_TRUE(station->SetMidiChannelRoute(0u, {{newPlugin->StateId()}}, station->RackRevision()));
	station->CommitChanges();
	RenderStationBlock(station, 384u);
	ASSERT_EQ(2u, oldPlugin->ProcessedEvents.size());
	EXPECT_TRUE(oldPlugin->ProcessedEvents[1].IsNoteOff());
	EXPECT_EQ(60u, oldPlugin->ProcessedEvents[1].data1);
	EXPECT_TRUE(newPlugin->ProcessedEvents.empty());
}

class PlaybackReviewDelivery : public midi::MidiDelivery
{
public:
	// Direct scheduler fixtures construct the same immutable fanout indices that
	// production Prepare resolves off callback; source/destination state stays explicit.
	static void CompleteFixtureTopology(PreparedState& state)
	{
		state.DestinationLegacyVelocity.assign(state.MidiDestinations.size(), true);
		state.DestinationChannels.resize(state.MidiDestinations.size(), 0xffffu);
		state.DestinationRetirements.resize(state.MidiDestinations.size());
		state.PlaybackDestinationIndices.clear();
		for (const auto& routes : state.MidiPlaybackRoutes)
		{
			std::vector<std::size_t> indices;
			for (const auto& binding : routes)
				indices.push_back(static_cast<std::size_t>(std::find(state.MidiDestinations.begin(),
					state.MidiDestinations.end(), binding->Destination) - state.MidiDestinations.begin()));
			state.PlaybackDestinationIndices.push_back(std::move(indices));
		}
	}

	static void CheckCircularQueuedReleaseExtension()
	{
		PreparedState state; auto take = MakeMidiTake("edit-circular-queue"); auto loop = std::make_shared<midi::MidiLoop>();
		const std::array source{MidiEvent::MakeNoteOff(20u, 0u, 60u, 23u).WithFlags(MidiEvent::TriggerSeamEndpoint),
			MidiEvent::MakeNoteOn(80u, 0u, 60u, 100u).WithFlags(MidiEvent::TriggerSeamEndpoint)};
		loop->ReplaceRecordedEvents(source.data(), source.size(), 100u);
		auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>(); plugin->Load(L"", 44100.f, 128u, 2u);
		auto destination = std::make_shared<MidiDestination>(); destination->Plugin = plugin; destination->ActiveCompensationDelaySamps = 50u;
		auto binding = std::make_shared<MidiPlaybackBinding>(); binding->Take = take; binding->Loop = loop; binding->Destination = destination;
		state.MidiPlaybackRoutes.push_back({binding}); state.MidiDestinations.push_back(destination);
		state.DestinationChannels.push_back(0xffffu); state.DestinationRetirements.emplace_back();
		CompleteFixtureTopology(state);
		_InvalidateScheduledMidiAtBlockStart(state, 10000u);
		MidiPlaybackSink output(state); midi::MidiIndexedOutputSink sink(output, 0u, 80u, 10000u);
		loop->ReadBlock(80u, 41u, sink); ASSERT_FALSE(binding->Held.any()); ASSERT_EQ(2u, destination->ScheduledCount);
		_DrainScheduledMidi(state, 10000u, 61u); ASSERT_TRUE(binding->AudibleHeld.test(60u)); ASSERT_EQ(1u, plugin->Events.size());
		midi::MidiLoop::EditState edit; ASSERT_TRUE(loop->SnapshotForEdit(edit));
		edit.Events[0].sampleOffset = 40u; edit.Events[1].sampleOffset = 70u; ASSERT_TRUE(loop->PublishEdit(edit));
		_InvalidateScheduledMidiAtBlockStart(state, 10061u); _DrainScheduledMidi(state, 10061u, 49u);
		EXPECT_TRUE(binding->AudibleHeld.test(60u)); EXPECT_EQ(1u, plugin->Events.size());
		_DrainScheduledMidi(state, 10110u, 1u); ASSERT_EQ(2u, plugin->Events.size());
		EXPECT_TRUE(plugin->Events.back().IsNoteOff()); EXPECT_EQ(10110u, plugin->Events.back().sampleOffset);
		EXPECT_EQ(23u, plugin->Events.back().data2); EXPECT_FALSE(binding->AudibleHeld.any());
	}

	static void CheckOrdinaryEditPartialFanoutRetry()
	{
		PreparedState state; auto take = MakeMidiTake("edit-partial-fanout");
		auto loop = std::make_shared<midi::MidiLoop>();
		const std::array source{MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u), MidiEvent::MakeNoteOn(10u, 0u, 64u, 90u),
			MidiEvent::MakeNoteOff(20u, 0u, 60u, 23u), MidiEvent::MakeNoteOff(900u, 0u, 64u)};
		loop->ReplaceRecordedEvents(source.data(), source.size(), 1000u); state.MidiPlaybackRoutes.emplace_back();
		std::array<std::shared_ptr<StationMidiInstrumentFakePlugin>, 2> plugins;
		for (auto index = 0u; index < plugins.size(); ++index)
		{
			plugins[index] = std::make_shared<StationMidiInstrumentFakePlugin>(); plugins[index]->Load(L"", 44100.f, 128u, 2u);
			auto destination = std::make_shared<MidiDestination>(); destination->Plugin = plugins[index];
			auto binding = std::make_shared<MidiPlaybackBinding>(); binding->Take = take; binding->Loop = loop; binding->Destination = destination;
			state.MidiDestinations.push_back(destination); state.MidiPlaybackRoutes[0].push_back(binding);
			state.DestinationChannels.push_back(0xffffu); state.DestinationRetirements.emplace_back();
		}
		CompleteFixtureTopology(state);
		_InvalidateScheduledMidiAtBlockStart(state, 0u);
		MidiPlaybackSink output(state); midi::MidiIndexedOutputSink sink(output, 0u, 0u, 0u);
		loop->ReadBlock(0u, 15u, sink);
		auto& slow = *state.MidiDestinations[0]; slow.ActiveCompensationDelaySamps = 256u;
		slow.ScheduledCount = MidiDestination::ScheduledCapacity;
		loop->ReadBlock(15u, 16u, sink);
		ASSERT_TRUE(state.MidiPlaybackRoutes[0][0]->Held.test(60u)); ASSERT_FALSE(state.MidiPlaybackRoutes[0][1]->Held.test(60u));
		midi::MidiLoop::EditState edit; ASSERT_TRUE(loop->SnapshotForEdit(edit));
		edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOn(950u, 0u, 72u, 80u);
		edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOff(960u, 0u, 72u); ASSERT_TRUE(loop->PublishEdit(edit));
		slow.ScheduledCount = 0u; _InvalidateScheduledMidiAtBlockStart(state, 31u); loop->ReadBlock(31u, 1u, sink);
		ASSERT_EQ(1u, slow.ScheduledCount); _DrainScheduledMidi(state, 256u, 128u);
		for (auto index = 0u; index < plugins.size(); ++index)
		{
			EXPECT_TRUE(state.MidiPlaybackRoutes[0][index]->AudibleHeld.test(64u));
			EXPECT_FALSE(state.MidiPlaybackRoutes[0][index]->AudibleHeld.test(60u));
			EXPECT_EQ(2, std::count_if(plugins[index]->Events.begin(), plugins[index]->Events.end(), [](const auto& e) { return e.IsNoteOn(); }));
			EXPECT_EQ(1, std::count_if(plugins[index]->Events.begin(), plugins[index]->Events.end(), [](const auto& e) { return e.IsNoteOff(); }));
			EXPECT_EQ(23u, plugins[index]->Events.back().data2);
		}
	}

	static void CheckOrdinaryEditDelayedQueue()
	{
		for (auto mode = 0u; mode < 3u; ++mode)
		for (const auto deviceStart : {10000u, 0xfffffff0u})
		{
			SCOPED_TRACE(mode);
			SCOPED_TRACE(deviceStart);
			PreparedState state;
			auto take = MakeMidiTake("edit-delayed-queue");
			auto loop = std::make_shared<midi::MidiLoop>();
			const std::array source{MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u), MidiEvent::MakeNoteOn(12u, 0u, 64u, 90u),
				MidiEvent::MakeNoteOff(50u, 0u, 60u, 23u), MidiEvent::MakeNoteOff(52u, 0u, 64u, 31u)};
			loop->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
			state.MidiPlaybackRoutes.emplace_back();
			auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
			plugin->Load(L"", 44100.f, 128u, 2u);
			auto destination = std::make_shared<MidiDestination>();
			destination->Plugin = plugin; destination->ActiveCompensationDelaySamps = 50u;
			auto binding = std::make_shared<MidiPlaybackBinding>();
			binding->Take = take; binding->Loop = loop; binding->Destination = destination;
			state.MidiPlaybackRoutes[0].push_back(binding); state.MidiDestinations.push_back(destination);
			state.DestinationChannels.push_back(0xffffu); state.DestinationRetirements.emplace_back();
			CompleteFixtureTopology(state);
			MidiPlaybackSink output(state);
			auto render = [&](unsigned int offset, unsigned int count) {
				_InvalidateScheduledMidiAtBlockStart(state, deviceStart + offset);
				// The source clock deliberately differs from the device clock, including uint32 wrap.
				midi::MidiIndexedOutputSink sink(output, 0u, 1000u + offset, deviceStart + offset);
				loop->ReadBlock(1000u + offset, count, sink);
				_DrainScheduledMidi(state, deviceStart + offset, count);
			};
			render(0u, 55u);
			ASSERT_EQ(4u, destination->ScheduledCount); ASSERT_FALSE(binding->Held.any());
			if (mode == 2u) { render(55u, 10u); ASSERT_TRUE(binding->AudibleHeld.test(60u)); }
			midi::MidiLoop::EditState edit; ASSERT_TRUE(loop->SnapshotForEdit(edit));
			if (mode == 1u)
			{
				edit.Events[0] = edit.Events[1]; edit.Tokens[0] = edit.Tokens[1];
				edit.Events[1] = edit.Events[3]; edit.Tokens[1] = edit.Tokens[3]; edit.EventCount = 2u;
			}
			else if (mode == 2u) { edit.Events[0].sampleOffset = 5u; edit.Events[2].sampleOffset = 90u; }
			else { edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOn(200u, 0u, 72u, 80u); edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOff(210u, 0u, 72u); }
			midi::MidiNote::SortMidiEvents(edit.Events.data(), edit.EventCount, nullptr, nullptr, edit.Tokens.data());
			ASSERT_TRUE(loop->PublishEdit(edit));
			render(mode == 2u ? 65u : 55u, mode == 2u ? 35u : 45u);
			if (mode == 0u) ASSERT_EQ(2u, plugin->Events.size()); // Both attacks survive a revision after their source ends.
			if (mode == 1u) { ASSERT_EQ(1u, plugin->Events.size()); EXPECT_EQ(64u, plugin->Events[0].data1); }
			if (mode == 2u) { ASSERT_EQ(2u, plugin->Events.size()); EXPECT_TRUE(binding->AudibleHeld.test(60u)); }
			render(100u, 40u);
			if (mode == 2u) { EXPECT_TRUE(binding->AudibleHeld.test(60u)); EXPECT_FALSE(binding->AudibleHeld.test(64u)); }
			render(140u, 1u);
			EXPECT_FALSE(binding->AudibleHeld.any()); EXPECT_EQ(0u, destination->ScheduledCount);
			const auto releases = std::count_if(plugin->Events.begin(), plugin->Events.end(), [](const auto& event) { return event.IsNoteOff(); });
			EXPECT_EQ(mode == 1u ? 1 : 2, releases);
			if (mode == 2u) { EXPECT_EQ(deviceStart + 140u, plugin->Events.back().sampleOffset); EXPECT_EQ(23u, plugin->Events.back().data2); }
		}
	}

	static void CheckFanoutReleaseRetry()
	{
		for (const bool sustain : {false, true})
		{
			SCOPED_TRACE(sustain);
			PreparedState state;
			auto take = MakeMidiTake("review-fanout");
			auto loop = std::make_shared<midi::MidiLoop>();
			state.MidiPlaybackRoutes.emplace_back();
			for (unsigned int index = 0; index < 2u; ++index)
			{
				auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
				plugin->Load(L"", 44100.f, 128u, 2u);
				auto destination = std::make_shared<MidiDestination>();
				destination->Plugin = plugin;
				auto binding = std::make_shared<MidiPlaybackBinding>();
				binding->Take = take; binding->Loop = loop; binding->Destination = destination;
				state.MidiPlaybackRoutes[0].push_back(binding);
				state.MidiDestinations.push_back(destination);
			}
			CompleteFixtureTopology(state);
			MidiPlaybackSink sink(state);
			const auto on = sustain ? MidiEvent{0u, 0xb0u, 64u, 127u} : MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
			ASSERT_TRUE(sink.TryOnEvent(0u, on));
			auto slow = state.MidiPlaybackRoutes[0][0]->Destination;
			slow->ActiveCompensationDelaySamps = 256u;
			slow->ScheduledCount = MidiDestination::ScheduledCapacity;
			const auto off = sustain ? MidiEvent{64u, 0xb0u, 64u, 0u} : MidiEvent::MakeNoteOff(64u, 0u, 60u);
			EXPECT_FALSE(sink.TryOnEvent(0u, off));
			if (sustain)
			{
				EXPECT_TRUE(state.MidiPlaybackRoutes[0][0]->AudibleSustain.test(0u));
				EXPECT_FALSE(state.MidiPlaybackRoutes[0][1]->AudibleSustain.test(0u));
			}
			else
			{
				EXPECT_TRUE(state.MidiPlaybackRoutes[0][0]->Held.test(60u));
				EXPECT_FALSE(state.MidiPlaybackRoutes[0][1]->Held.test(60u));
			}
			slow->ScheduledCount = 0u;
			EXPECT_TRUE(sink.TryOnEvent(0u, off));
			EXPECT_FALSE(state.MidiPlaybackRoutes[0][0]->Held.test(60u));
			EXPECT_EQ(1u, slow->ScheduledCount);
			_DrainScheduledMidi(state, 256u, 128u);
			EXPECT_FALSE(state.MidiPlaybackRoutes[0][0]->AudibleSustain.test(0u));
			EXPECT_FALSE(slow->PluginSustain.test(0u));
		}
	}

	static void CheckSourcePedalReleaseRetry()
	{
		PreparedState state;
		auto take = MakeMidiTake("source-pedal-fanout");
		auto loop = std::make_shared<midi::MidiLoop>();
		const std::array source{MidiEvent{0u, 0xb0u, 64u, 127u}, MidiEvent{10u, 0xb0u, 64u, 0u},
			MidiEvent{20u, 0xb0u, 64u, 127u}, MidiEvent{400u, 0xb0u, 64u, 127u}};
		loop->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
		state.MidiPlaybackRoutes.emplace_back();
		std::array<std::shared_ptr<StationMidiInstrumentFakePlugin>, 2> plugins;
		for (auto index = 0u; index < plugins.size(); ++index)
		{
			plugins[index] = std::make_shared<StationMidiInstrumentFakePlugin>();
			plugins[index]->Load(L"", 44100.f, 128u, 2u);
			auto destination = std::make_shared<MidiDestination>();
			destination->Plugin = plugins[index];
			auto binding = std::make_shared<MidiPlaybackBinding>();
			binding->Take = take; binding->Loop = loop; binding->Destination = destination;
			state.MidiDestinations.push_back(destination);
			state.MidiPlaybackRoutes[0].push_back(binding);
		}
		CompleteFixtureTopology(state);
		MidiPlaybackSink output(state);
		midi::MidiIndexedOutputSink sink(output, 0u, 0u, 0u);
		loop->ReadBlock(0u, 10u, sink);
		for (const auto& destination : state.MidiDestinations) ASSERT_TRUE(destination->PluginSustain.test(0u));
		auto& slow = *state.MidiDestinations[0];
		auto& fast = *state.MidiDestinations[1];
		slow.ActiveCompensationDelaySamps = 256u;
		slow.ScheduledCount = MidiDestination::ScheduledCapacity;
		loop->ReadBlock(10u, 20u, sink);
		EXPECT_TRUE(slow.PluginSustain.test(0u));
		EXPECT_FALSE(fast.PluginSustain.test(0u));
		ASSERT_EQ(2u, plugins[1]->Events.size()); // Down20 must not overtake the source's pending up10.
		EXPECT_EQ(0u, plugins[1]->Events.back().data2);
		slow.ScheduledCount = 0u;
		loop->ReadBlock(30u, 10u, sink);
		_DrainScheduledMidi(state, 256u, 128u);
		EXPECT_FALSE(slow.PluginSustain.test(0u));
		EXPECT_FALSE(fast.PluginSustain.test(0u));
		loop->ReadBlock(400u, 1u, sink);
		loop->ReadBlock(401u, 10u, sink);
		EXPECT_TRUE(fast.PluginSustain.test(0u));
		ASSERT_EQ(3u, plugins[1]->Events.size());
		EXPECT_EQ(127u, plugins[1]->Events.back().data2);
		_DrainScheduledMidi(state, 656u, 128u);
		EXPECT_TRUE(slow.PluginSustain.test(0u));
		loop->FlushPlaybackHeldNotes(700u, sink);
		_DrainScheduledMidi(state, 896u, 128u);
		EXPECT_FALSE(slow.PluginSustain.test(0u));
		EXPECT_FALSE(fast.PluginSustain.test(0u));
	}
	static void CheckReplacementPedalOrdering()
	{
		for (const bool live : {false, true})
		{
			SCOPED_TRACE(live);
			PreparedState state;
			auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
			plugin->Load(L"", 44100.f, 128u, 2u);
			auto destination = std::make_shared<MidiDestination>();
			destination->Plugin = plugin;
			state.MidiDestinations.push_back(destination);
			state.LiveChannelDestinations[0].push_back(destination);
			MidiPlaybackBinding owner;
			const MidiEvent down{0u, 0xb0u, 64u, 127u};
			const MidiEvent up{1u, 0xb0u, 64u, 0u};
			auto deliver = [&](const MidiEvent& event) {
				if (live) _DispatchLiveMidi(state, event, false, event.sampleOffset);
				else _TryDeliverRecordedMidiNow(*destination, event, owner, true);
			};
			deliver(down);
			plugin->MaxAcceptedPerBlock = 0u;
			deliver(up);
			ASSERT_TRUE(destination->PendingSustainRelease.test(0u));
			const auto accepted = plugin->Events.size();
			deliver(down);
			EXPECT_EQ(accepted, plugin->Events.size());
			EXPECT_FALSE(live ? destination->LiveSustain.test(0u) : owner.AudibleSustain.test(0u));
			plugin->MaxAcceptedPerBlock = 100u;
			deliver(down);
			ASSERT_EQ(accepted + 2u, plugin->Events.size());
			EXPECT_EQ(0u, plugin->Events[accepted].data2);
			EXPECT_EQ(127u, plugin->Events[accepted + 1u].data2);
			EXPECT_FALSE(destination->PendingSustainRelease.test(0u));
			_DrainPendingMidiReleases(state, 128u);
			EXPECT_EQ(accepted + 2u, plugin->Events.size());
			EXPECT_TRUE(destination->PluginSustain.test(0u));
			EXPECT_TRUE(live ? destination->LiveSustain.test(0u) : owner.AudibleSustain.test(0u));
			deliver(up);
			EXPECT_FALSE(destination->PluginSustain.test(0u));
		}
	}
};
TEST(StationMidiInstrument, LegacyAmbiguousFifoReleasesSurviveRecordedDeliveryCompensation)
{
	auto station = MakeStation("legacy-fifo-delivery");
	station->SetSampleRate(static_cast<float>(constants::DefaultSampleRate), 0u);
	auto plugin = AddPlugin(station, L"fake-fifo-delivery.dll");
	auto take = MakeMidiTake("legacy-fifo-take");
	take->Record({}, station->Name(), {0u}, {"source"}); take->Play(0u, 1000u, 0u);
	const std::array source{MidiEvent::MakeNoteOn(10u, 0u, 60u, 80u), MidiEvent::MakeNoteOn(20u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOff(30u, 0u, 60u), MidiEvent::MakeNoteOn(40u, 0u, 60u, 100u)};
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take); station->CommitChanges(); ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	for (auto sample = 0u; sample < 1664u; sample += 128u) RenderStationBlock(station, sample);
	const std::array expected{266u, 276u, 286u, 296u, 1256u, 1266u, 1276u, 1286u, 1296u};
	ASSERT_EQ(expected.size(), plugin->ProcessedEvents.size());
	for (auto i = 0u; i < expected.size(); ++i) EXPECT_EQ(expected[i], plugin->ProcessedEvents[i].sampleOffset);
	EXPECT_TRUE(plugin->ProcessedEvents[2].IsNoteOff()); EXPECT_TRUE(plugin->ProcessedEvents[4].IsNoteOff());
	EXPECT_TRUE(plugin->ProcessedEvents[7].IsNoteOff());
}

TEST(StationMidiInstrument, OrdinaryEditorPublicationPreservesAudibleChordThroughStationDelivery)
{
	auto station = MakeStation("ordinary-edit-chord");
	auto plugin = AddPlugin(station, L"fake-edit-chord.dll");
	auto take = MakeMidiTake("ordinary-edit-chord-take");
	take->Record({}, station->Name(), {0u}, {"source"}); take->Play(0u, 1000u, 0u);
	auto loop = take->GetMidiLoops().at(0u);
	const std::array source{MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), MidiEvent::MakeNoteOn(0u, 0u, 64u, 90u),
		MidiEvent::MakeNoteOff(800u, 0u, 60u), MidiEvent::MakeNoteOff(900u, 0u, 64u)};
	loop->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take); station->CommitChanges(); ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u); RenderStationBlock(station, 128u); RenderStationBlock(station, 256u);
	ASSERT_EQ(2u, plugin->ProcessedEvents.size());
	midi::MidiLoop::EditState edit; ASSERT_TRUE(loop->SnapshotForEdit(edit));
	edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOn(700u, 0u, 72u, 80u);
	edit.Events[edit.EventCount++] = MidiEvent::MakeNoteOff(750u, 0u, 72u);
	midi::MidiNote::SortMidiEvents(edit.Events.data(), edit.EventCount, nullptr, nullptr, edit.Tokens.data());
	ASSERT_TRUE(loop->PublishEdit(edit)); RenderStationBlock(station, 384u);
	ASSERT_EQ(2u, plugin->ProcessedEvents.size()); // No delivery-originated full flush on an ordinary revision.
	ASSERT_TRUE(loop->SnapshotForEdit(edit));
	edit.Events[0].data1 = 61u; // A changes pitch; B remains owned.
	// Find A's paired release by pitch rather than relying on sorted insertion positions.
	for (std::size_t n = 0u; n < edit.EventCount; ++n) if (edit.Events[n].IsNoteOff() && edit.Events[n].data1 == 60u) edit.Events[n].data1 = 61u;
	ASSERT_TRUE(loop->PublishEdit(edit)); RenderStationBlock(station, 512u); RenderStationBlock(station, 640u); RenderStationBlock(station, 768u);
	EXPECT_EQ(1, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), [](const auto& e) { return e.IsNoteOff() && e.data1 == 60u; }));
	EXPECT_EQ(0, std::count_if(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), [](const auto& e) { return e.IsNoteOff() && e.data1 == 64u; }));
}

TEST(StationMidiInstrument, OrdinaryEditReconcilesDelayedArticulationsInMappedDeviceTime)
{
	PlaybackReviewDelivery::CheckOrdinaryEditDelayedQueue();
	PlaybackReviewDelivery::CheckOrdinaryEditPartialFanoutRetry();
	PlaybackReviewDelivery::CheckCircularQueuedReleaseExtension();
}

TEST(StationMidiInstrument, FullDelayedDestinationRetainsReleaseUntilRetry)
{
	PlaybackReviewDelivery::CheckFanoutReleaseRetry();
	PlaybackReviewDelivery::CheckSourcePedalReleaseRetry();
	PlaybackReviewDelivery::CheckReplacementPedalOrdering();
}
TEST(StationMidiInstrument, RejectedPlaybackReleasePreservesVelocity)
{
	auto station = MakeStation("release-velocity");
	auto plugin = AddPlugin(station, L"fake-release-velocity.dll");
	auto take = MakeMidiTake("release-velocity-take");
	take->Record({}, station->Name(), { 0u }, { "source" });
	take->Play(0u, 1000u, 0u);
	const std::array source{ MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(10u, 0u, 60u, 23u) };
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(source.data(), source.size(), 1000u);
	station->AddTake(take);
	station->CommitChanges();
	plugin->MaxAcceptedPerBlock = 1u;
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->ProcessedEvents.size());
	ASSERT_TRUE(plugin->ProcessedEvents[0].IsNoteOn());
	RenderStationBlock(station, 128u);
	ASSERT_EQ(2u, plugin->ProcessedEvents.size());
	EXPECT_TRUE(plugin->ProcessedEvents[1].IsNoteOff());
	EXPECT_EQ(23u, plugin->ProcessedEvents[1].data2);
}

TEST(StationMidiInstrument, OrdinaryDitchReleasesSharedLiveAndPlaybackVoice)
{
	for (const bool recording : { false, true })
	for (const auto delay : { 0u, 256u })
	for (const bool rejectRelease : { false, true })
	{
		SCOPED_TRACE(testing::Message() << "recording=" << recording
			<< " delay=" << delay << " rejected=" << rejectRelease);
		auto station = MakeStation("ordinary-held-ditch");
		auto plugin = AddPlugin(station, L"fake-ordinary-held-ditch.dll");
		AllowAllMidiChannels(station);
		auto take = MakeMidiTake("ordinary-held-ditch-take");
		take->Record({}, station->Name(), { 0u }, { "Keys" });
		if (!recording)
		{
			ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 43u, 100u), "Keys", 0u));
			take->Play(0u, 10000u, 0u);
		}
		station->AddTake(take);
		station->CommitChanges();
		ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(delay));
		const auto liveOn = MidiEvent::MakeNoteOn(0u, 0u, 43u, 90u);
		station->ObservePhysicalMidiForRecording(liveOn, "Keys");
		ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(liveOn));
		RenderStationBlock(station, 0u);
		RenderStationBlock(station, 128u);
		RenderStationBlock(station, 256u);
		ASSERT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
			[](const auto& event) { return event.IsNoteOn() && event.data1 == 43u; }));
		plugin->ProcessedEvents.clear();
		TriggerAction ditch;
		ditch.ActionType = TriggerAction::TRIGGER_DITCH;
		ditch.TargetId = take->Id();
		ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
		station->CommitChanges();
		if (rejectRelease) plugin->MaxAcceptedPerBlock = 0u;
		RenderStationBlock(station, 384u);
		plugin->MaxAcceptedPerBlock = static_cast<std::size_t>(-1);
		RenderStationBlock(station, 512u);
		RenderStationBlock(station, 640u);
		EXPECT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
			[](const auto& event) { return event.IsNoteOff() && event.data1 == 43u; }));
		EXPECT_TRUE(std::none_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(),
			[](const auto& event) { return event.IsNoteOn() && event.data1 == 43u; }));
	}
}


TEST(StationMidiInstrument, QuiescentResetReleasesSoundedNoteBeforeJobObservedIt)
{
	auto station = MakeStation("quiescent-live-reset");
	auto plugin = AddPlugin(station, L"fake-reset.dll");
	AllowAllMidiChannels(station);
	// Live monitoring wins the race with the job-side physical-note observer.
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u)));
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->Events.size());
	ASSERT_TRUE(plugin->Events[0].IsNoteOn());
	plugin->Events.clear();
	station->ResetLiveMidiWhileQuiescent();
	EXPECT_TRUE(plugin->Events.empty()); // Delivered after BeginMidiBlock on resume.
	RenderStationBlock(station, 128u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events[0].IsNoteOff());
	EXPECT_EQ(60u, plugin->Events[0].data1);
	RenderStationBlock(station, 256u);
	EXPECT_EQ(1u, plugin->Events.size());
}

TEST(StationMidiInstrument, QuiescentResetDiscardsFutureNotesAndResetsIngressWatermark)
{
	auto station = MakeStation("quiescent-future-reset");
	auto plugin = AddPlugin(station, L"fake-future-reset.dll");
	AllowAllMidiChannels(station);
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(100000u, 0u, 60u, 100u)));
	ASSERT_TRUE(station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(100000u, 0u, 61u, 100u)));
	station->ResetLiveMidiWhileQuiescent();
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 62u, 100u)));
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events[0].IsNoteOn());
	EXPECT_EQ(62u, plugin->Events[0].data1);
	plugin->Events.clear();
	RenderStationBlock(station, 100000u);
	EXPECT_TRUE(plugin->Events.empty());
}

TEST(StationMidiInstrument, QuiescentLiveResetPreservesOverlappingPlaybackOwnership)
{
	auto station = MakeStation("quiescent-playback-reset");
	auto plugin = AddPlugin(station, L"fake-playback-reset.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("playback-preserved");
	take->Record({}, station->Name(), { 0u }, { "playback" });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "playback", 0u));
	take->Play(0u, 1000u, 0u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u)));
	RenderStationBlock(station, 0u);
	ASSERT_FALSE(plugin->Events.empty());
	plugin->Events.clear();
	station->ResetLiveMidiWhileQuiescent();
	RenderStationBlock(station, 128u);
	EXPECT_TRUE(plugin->Events.empty());
}


TEST(StationMidiInstrument, QuiescentResetRetainsQueuedPlaybackSourceLossRelease)
{
	auto station = MakeStation("quiescent-playback-release");
	auto plugin = AddPlugin(station, L"fake-playback-release.dll");
	AllowAllMidiChannels(station);
	auto take = MakeMidiTake("lost-playback");
	take->Record({}, station->Name(), { 0u }, { "lost" });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 67u, 100u), "lost", 0u));
	take->Play(0u, 1000u, 0u);
	station->AddTake(take);
	station->CommitChanges();
	RenderStationBlock(station, 0u);
	plugin->Events.clear();
	TriggerAction ditch;
	ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = take->Id();
	ditch.IsSourceLossCancellation = true;
	ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
	station->CommitChanges();
	station->ResetLiveMidiWhileQuiescent();
	RenderStationBlock(station, 128u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events[0].IsNoteOff());
	EXPECT_EQ(67u, plugin->Events[0].data1);
}

TEST(StationMidiInstrument, SoundingSustainSurvivesHealthySourceAndRecoversAcrossBypassLatencyAndResize)
{
	auto station = MakeStation("pedal-lifecycle");
	auto plugin = AddPlugin(station, L"pedal.dll");
	AllowAllMidiChannels(station);
	const MidiEvent down{0, 0xb0, 64, 127};
	station->ObservePhysicalMidiForRecording(down, "lost");
	station->ObservePhysicalMidiForRecording(down, "healthy");
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0, 0, 60, 100));
	station->TryEnqueueImmediateLiveMidi(down);
	RenderStationBlock(station, 0);
	plugin->ProcessedEvents.clear();
	station->FlushLiveHeldMidiNotesForDevice("lost");
	RenderStationBlock(station, 128);
	EXPECT_TRUE(plugin->ProcessedEvents.empty());
	plugin->SetBypassed(true);
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(256, 0, 60));
	station->TryEnqueueImmediateLiveMidi({256, 0xb0, 64, 0});
	RenderStationBlock(station, 256);
	plugin->SetBypassed(false);
	RenderStationBlock(station, 384);
	EXPECT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), [](const auto& event) { return event.IsNoteOff(); }));
	EXPECT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), [](const auto& event) { return event.MessageType() == 0xb0 && event.data1 == 64 && event.data2 == 0; }));
	plugin->LatencySamps = 262145;
	station->RefreshRackLatency();
	EXPECT_TRUE(station->RackLatencySuspended());
	EXPECT_FALSE(station->RackDiagnostic().empty());
	station->SetNumBusChannels(1); station->CommitChanges();
	RenderStationBlock(station, 512);
	station->AcknowledgeAudioBoundary(); station->ReleaseRetiredAudioStates();
	RenderStationBlock(station, 640);
	station->AcknowledgeAudioBoundary(); station->ReleaseRetiredAudioStates();
	plugin->LatencySamps = 0;
	station->RefreshRackLatency();
	EXPECT_FALSE(station->RackLatencySuspended());
	// A recreated destination still uses the processor's original two-channel
	// hosted layout after the Station bus shrank while suspended.
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(768, 0, 61, 100));
	RenderStationBlock(station, 768);
	EXPECT_TRUE(plugin->LastBlockProcessed);
	// Pending membership must count toward admission, before CommitChanges.
	auto pending = MakeMidiTake("pending-ceiling");
	auto child = std::make_shared<StationMidiInstrumentFakePlugin>();
	child->LatencySamps = 262144;
	JobAction childLoad; childLoad.JobActionType = JobAction::JOB_LOADVST;
	childLoad.VstPath = L"child-ceiling.dll"; childLoad.PreInitPlugin = child;
	pending->OnAction(childLoad); pending->CommitChanges();
	plugin->LatencySamps = 1;
	station->AddTake(pending);
	station->RefreshRackLatency();
	EXPECT_TRUE(station->RackLatencySuspended());
	child->LatencySamps = 0;
	ASSERT_TRUE(station->UnloadManagedVstPlugin(plugin->StateId()));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	station->SetNumBusChannels(1); station->CommitChanges();
	plugin->ProcessedEvents.clear();
	RenderStationBlock(station, 896);
	EXPECT_TRUE(plugin->LastBlockProcessed); // Retirement kept original hosted width.
	EXPECT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), [](const auto& event) { return event.IsNoteOff() && event.data1 == 61; }));
}

TEST(StationMidiInstrument, RecordedPedalFlushRetriesWithoutLiftingLiveOwner)
{
	for (const bool liveOwner : {false, true})
	{
		SCOPED_TRACE(liveOwner);
		auto station = MakeStation("recorded-pedal");
		auto plugin = AddPlugin(station, L"shared-pedal.dll");
		AllowAllMidiChannels(station);
		auto take = MakeMidiTake("pedal-take");
		take->Record({}, station->Name(), {0u}, {"source"});
		take->Play(0, 1000, 0);
		const std::array events{MidiEvent{0, 0xb0, 64, 127}, MidiEvent::MakeNoteOn(0, 0, 60, 100), MidiEvent::MakeNoteOff(32, 0, 60)};
		auto loop = take->GetMidiLoops().at(0);
		loop->ReplaceRecordedEvents(events.data(), events.size(), 1000);
		station->AddTake(take); station->CommitChanges();
		if (liveOwner) station->TryEnqueueImmediateLiveMidi({0, 0xb0, 64, 127});
		RenderStationBlock(station, 0);
		plugin->ProcessedEvents.clear();
		ASSERT_TRUE(take->Mute());
		plugin->MaxAcceptedPerBlock = 0;
		RenderStationBlock(station, 128);
		plugin->MaxAcceptedPerBlock = 100;
		RenderStationBlock(station, 256);
		const auto pedalUp = [](const auto& event) { return event.MessageType() == 0xb0 && event.data1 == 64 && event.data2 == 0; };
		EXPECT_EQ(!liveOwner, std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), pedalUp));
		if (!liveOwner)
		{
			auto filled = 0u;
			while (filled < 1024u && station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(100000u, 0, 70, 100))) ++filled;
			ASSERT_GT(filled, 0u);
			EXPECT_FALSE(station->TryEnqueueSyntheticLiveMidi(MidiEvent::MakeNoteOn(100000u, 0, 70, 100)));
			plugin->LatencySamps = 262145;
			station->RefreshRackLatency();
			RenderStationBlock(station, 384);
			station->AcknowledgeAudioBoundary(); station->ReleaseRetiredAudioStates();
			RenderStationBlock(station, 512);
			station->AcknowledgeAudioBoundary(); station->ReleaseRetiredAudioStates();
			plugin->LatencySamps = 0;
			station->RefreshRackLatency();
			EXPECT_TRUE(station->RackLatencySuspended()); // DSP cleanup alone cannot consume a full ingress queue's receipt.
			station->ResetLiveMidiWhileQuiescent();
			station->FlushPendingLostLiveMidiNotes();
			RenderStationBlock(station, 640);
			station->AcknowledgeAudioBoundary(); station->ReleaseRetiredAudioStates();
			station->RefreshRackLatency();
			EXPECT_FALSE(station->RackLatencySuspended());
		}
		if (liveOwner)
		{
			station->TryEnqueueImmediateLiveMidi({384, 0xb0, 64, 0});
			RenderStationBlock(station, 384);
			EXPECT_TRUE(std::any_of(plugin->ProcessedEvents.begin(), plugin->ProcessedEvents.end(), pedalUp));
		}
	}
}

TEST(StationMidiInstrument, LiveAndRecordedShareChannelStatesIncludingSixteenAndUnavailableCables)
{
	for (const bool recorded : {false, true})
		for (const unsigned int mode : {0u, 1u, 2u, 3u, 4u, 5u})
		{
			SCOPED_TRACE(recorded);
			SCOPED_TRACE(mode);
			auto station = MakeStation("shared-channel-states");
			auto a = AddPlugin(station, L"state-a.dll");
			auto b = AddPlugin(station, L"state-b.dll");
			a->RenderMidiImpulses = b->RenderMidiImpulses = true;
			station->SetAllowedMidiChannels(mode == 0u ? std::vector<int>{2} : std::vector<int>{1, 2, 16});
			if (mode >= 2u)
			{
				auto routes = station->MidiChannelRoutes();
				std::vector<std::string> targets{a->StateId()};
				if (mode == 3u) targets.push_back(b->StateId());
				if (mode == 4u) targets = {"unavailable-plugin"};
				if (mode == 5u) targets.push_back("unavailable-plugin");
				routes[0].PluginIds = routes[15].PluginIds = targets;
				// Startup restoration retains unavailable cable identities.
				station->RestoreSavedRack({}, routes, false);
				station->SetAllowedMidiChannels(station->AllowedMidiChannels());
			}
			std::vector<MidiEvent> events;
			for (const auto channel : {0u, 1u, 15u})
			{
				events.push_back(MidiEvent::MakeNoteOn(16u, channel, 60u + channel, 100u));
				events.push_back({24u, static_cast<std::uint8_t>(0xb0u | channel), 74u, 99u});
				events.push_back(MidiEvent::MakeNoteOff(32u, channel, 60u + channel));
			}
			std::stable_sort(events.begin(), events.end(), [](const auto& l, const auto& r) { return l.sampleOffset < r.sampleOffset; });
			if (recorded)
			{
				auto take = MakeMidiTake("multi-channel-loop");
				take->Record({}, station->Name(), {0u}, {"source"});
				take->Play(0u, 1024u, 0u);
				take->GetMidiLoops().at(0)->ReplaceRecordedEvents(events.data(), events.size(), 1024u);
				station->AddTake(take); station->CommitChanges();
			}
			else for (const auto& event : events) ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(event));
			const auto audio = RenderStationAudioBlock(station, 0u, 128u);
			const auto selected = mode != 0u && mode != 4u;
			for (const auto& plugin : {a, b})
			{
				const auto routed = selected && ((mode != 2u && mode != 5u) || plugin == a);
				ASSERT_EQ(routed ? 9u : 3u, plugin->Events.size());
				for (const auto& event : plugin->Events)
				{
					EXPECT_TRUE(event.Channel() == 1u || routed);
					EXPECT_EQ(event.IsNoteOn() ? 16u : event.IsNoteOff() ? 32u : 24u, event.sampleOffset);
				}
				for (const auto realtime : plugin->RealtimeFlags) EXPECT_EQ(!recorded, realtime);
			}
			EXPECT_FLOAT_EQ(selected ? ((mode == 2u || mode == 5u) ? 4.0f : 6.0f) : 2.0f, audio[16]);
		}
}

TEST(StationMidiInstrument, MaskOnlyDisableCancelsDelayedAttackControllersAndPedalBeforeReenable)
{
	for (const bool skipDisabledBlock : {false, true})
	{
	SCOPED_TRACE(skipDisabledBlock);
	auto station = MakeStation("disable-delayed-broadcast");
	auto plugin = AddPlugin(station, L"disable-delayed.dll");
	station->SetAllowedMidiChannels({1, 16});
	auto take = MakeMidiTake("disable-material");
	take->Record({}, station->Name(), {0u}, {"source"}); take->Play(0u, 1024u, 0u);
	const std::array events{MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), MidiEvent{0u, 0xb0u, 64u, 127u},
		MidiEvent{0u, 0xb0u, 74u, 99u}, MidiEvent::MakeNoteOff(64u, 0u, 60u),
		MidiEvent::MakeNoteOn(0u, 15u, 62u, 100u)};
	auto loop = take->GetMidiLoops().at(0u);
	loop->ReplaceRecordedEvents(events.data(), events.size(), 1024u);
	station->AddTake(take); station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(256u));
	RenderStationBlock(station, 0u);
	station->SetAllowedMidiChannels({16}); // No cables existed or changed.
	if (!skipDisabledBlock) RenderStationBlock(station, 128u);
	station->SetAllowedMidiChannels({1, 16});
	if (skipDisabledBlock) RenderStationBlock(station, 128u);
	RenderStationBlock(station, 256u); RenderStationBlock(station, 384u);
	ASSERT_EQ(1u, plugin->Events.size()); EXPECT_EQ(15u, plugin->Events[0].Channel());
	midi::MidiLoop::ExportState saved;
	ASSERT_TRUE(loop->SnapshotForExport(saved));
	EXPECT_EQ(events.size(), saved.EventCount);
	// Only the next loop occurrence may attack on the re-enabled channel.
	for (auto sample = 512u; sample <= 1280u; sample += 128u) RenderStationBlock(station, sample);
	EXPECT_TRUE(std::any_of(plugin->Events.begin(), plugin->Events.end(), [](const auto& e) { return e.IsNoteOn() && e.Channel() == 0u && e.sampleOffset == 1280u; }));
	}
}

TEST(StationMidiInstrument, DisablingAudibleRecordedChannelRetriesReleasesAndPreservesOtherChannel)
{
	auto station = MakeStation("disable-audible-recorded");
	auto plugin = AddPlugin(station, L"disable-audible.dll");
	station->SetAllowedMidiChannels({1, 16});
	auto take = MakeMidiTake("audible-material");
	take->Record({}, station->Name(), {0u}, {"source"}); take->Play(0u, 1024u, 0u);
	const std::array events{MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), MidiEvent{0u, 0xb0u, 64u, 127u},
		MidiEvent::MakeNoteOn(0u, 15u, 60u, 100u), MidiEvent{0u, 0xbfu, 64u, 127u}};
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(events.data(), events.size(), 1024u);
	station->AddTake(take); station->CommitChanges(); RenderStationBlock(station, 0u);
	ASSERT_EQ(4u, plugin->Events.size()); plugin->Events.clear();
	plugin->MaxAcceptedPerBlock = 0u;
	station->SetAllowedMidiChannels({16}); RenderStationBlock(station, 128u);
	EXPECT_TRUE(plugin->Events.empty());
	plugin->MaxAcceptedPerBlock = 100u; RenderStationBlock(station, 256u);
	ASSERT_EQ(2u, plugin->Events.size());
	for (const auto& event : plugin->Events) EXPECT_EQ(0u, event.Channel());
	EXPECT_TRUE(std::any_of(plugin->Events.begin(), plugin->Events.end(), [](const auto& e) { return e.IsNoteOff() && e.data1 == 60u; }));
	EXPECT_TRUE(std::any_of(plugin->Events.begin(), plugin->Events.end(), [](const auto& e) { return e.MessageType() == 0xb0u && e.data1 == 64u && e.data2 == 0u; }));
	station->SetAllowedMidiChannels({1, 16}); RenderStationBlock(station, 384u);
	EXPECT_EQ(2u, plugin->Events.size());
}

TEST(StationMidiInstrument, SourceLossKeepsHealthyLiveOwnerOfSameChannelNoteAndPedal)
{
	auto station = MakeStation("source-loss-shared-owner");
	auto plugin = AddPlugin(station, L"source-loss-shared.dll");
	station->SetAllowedMidiChannels({1});
	auto take = MakeMidiTake("lost-recorded-owner");
	take->Record({}, station->Name(), {0u}, {"lost"}); take->Play(0u, 1024u, 0u);
	const std::array events{MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), MidiEvent{0u, 0xb0u, 64u, 127u}};
	take->GetMidiLoops().at(0u)->ReplaceRecordedEvents(events.data(), events.size(), 1024u);
	station->AddTake(take); station->CommitChanges();
	for (const auto& event : events)
	{
		station->ObservePhysicalMidiForRecording(event, "healthy");
		ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(event));
	}
	RenderStationBlock(station, 0u); ASSERT_EQ(4u, plugin->Events.size()); plugin->Events.clear();
	TriggerAction ditch; ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = take->Id(); ditch.IsSourceLossCancellation = true;
	ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
	RenderStationBlock(station, 128u); station->CommitChanges(); RenderStationBlock(station, 256u);
	EXPECT_TRUE(plugin->Events.empty());
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(384u, 0u, 60u)));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi({384u, 0xb0u, 64u, 0u}));
	RenderStationBlock(station, 384u); ASSERT_EQ(2u, plugin->Events.size());
	EXPECT_TRUE(plugin->Events[0].IsNoteOff()); EXPECT_EQ(64u, plugin->Events[1].data1); EXPECT_EQ(0u, plugin->Events[1].data2);
}

TEST(StationMidiInstrument, BroadcastUsesMidiCapabilitiesRatherThanAudioRackOrder)
{
	auto station = MakeStation("broadcast-capabilities");
	auto a = AddPlugin(station, L"broadcast-first.dll");
	auto audioOnly = AddPlugin(station, L"audio-only.dll");
	auto b = AddPlugin(station, L"broadcast-last.dll");
	audioOnly->MidiCapabilities = {true, false, false};
	station->SetAllowedMidiChannels({16});
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 15u, 60u, 100u)));
	RenderStationBlock(station, 0u);
	EXPECT_EQ(1u, a->Events.size()); EXPECT_TRUE(audioOnly->Events.empty()); EXPECT_EQ(1u, b->Events.size());
}

TEST(StationMidiInstrument, EmptyRetainedMuteAllowsMidiCaptureAndLiveMonitoringButOnlyFutureRecordedOnsetsAfterUnmute)
{
	auto station=MakeStation("retained-midi"); auto plugin=AddPlugin(station,L"fake-retained-midi.dll"); station->Mute();
	auto take=station->AddTake(); take->Record({},station->Name(),{0u},{"source"}); take->CommitChanges();
	std::uint32_t captured=0u;
	for (const auto event : {MidiEvent::MakeNoteOn(0u,0u,60u,90u),MidiEvent::MakeNoteOff(80u,0u,60u),MidiEvent::MakeNoteOn(400u,0u,67u,95u),MidiEvent::MakeNoteOff(450u,0u,67u)})
	{
		take->EndMultiWrite(event.sampleOffset-captured,true,base::Audible::AUDIOSOURCE_ADC); captured=event.sampleOffset;
		ASSERT_EQ(captured,take->NumRecordedSamps()); ASSERT_TRUE(take->RecordMidiEvent(event,"source",event.sampleOffset));
	}
	ASSERT_EQ(4u,take->GetMidiLoops().front()->EventCount());
	take->EndMultiWrite(1000u-captured,true,base::Audible::AUDIOSOURCE_ADC);
	take->Play(0u,1000u,0u); station->CommitChanges(); take->UnMute();
	const auto savedCount=take->GetMidiLoops().front()->EventCount();
	midi::MidiEvent authored; bool futureStored=false;
	for (auto i=0u; i<savedCount; ++i) if (take->GetMidiLoops().front()->TryGetEvent(i,authored) && authored.IsNoteOn() && authored.data1==67u) { EXPECT_EQ(400u,authored.sampleOffset); futureStored=true; }
	ASSERT_TRUE(futureStored);
	ASSERT_TRUE(take->IsMuted()); EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
	RenderStationBlock(station,0u); ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(128u,0u,72u,100u)));
	ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOff(192u,0u,72u))); RenderStationBlock(station,128u);
	EXPECT_EQ(256u,take->MidiPlayIndex()); EXPECT_EQ(savedCount,take->GetMidiLoops().front()->EventCount());
	ASSERT_EQ(2u,plugin->ProcessedEvents.size()); EXPECT_EQ(72u,plugin->ProcessedEvents[0].data1); EXPECT_EQ(72u,plugin->ProcessedEvents[1].data1);
	station->UnMute(); RenderStationBlock(station,256u);
	EXPECT_EQ(0,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.IsNoteOn() && e.data1==67u;}));
	RenderStationBlock(station,384u);
	EXPECT_EQ(0,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.IsNoteOn() && e.data1==60u;}));
	EXPECT_EQ(1,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.IsNoteOn() && e.data1==67u && e.sampleOffset==400u;}));
}

TEST(StationMidiInstrument, RetainedParentMuteCleansHeldChordAndPedalWithRejectedReleaseRetry)
{
	auto station=MakeStation("parent-held"); auto plugin=AddPlugin(station,L"fake-parent-held.dll"); auto take=MakeMidiTake("parent-held-take");
	take->Record({},station->Name(),{0u},{"source"}); take->Play(0u,1000u,0u);
	const std::array source{MidiEvent{0u,0xb0u,64u,127u},MidiEvent::MakeNoteOn(10u,0u,60u,90u),MidiEvent::MakeNoteOn(10u,0u,64u,80u),
		MidiEvent::MakeNoteOff(800u,0u,60u,23u),MidiEvent::MakeNoteOff(800u,0u,64u),MidiEvent{900u,0xb0u,64u,0u}};
	take->GetMidiLoops().front()->ReplaceRecordedEvents(source.data(),source.size(),1000u); station->AddTake(take); station->CommitChanges(); RenderStationBlock(station,0u);
	ASSERT_EQ(3u,plugin->ProcessedEvents.size()); station->SetRetainedMuted(true); plugin->MaxAcceptedPerBlock=0u;
	RenderStationBlock(station,128u); EXPECT_EQ(3u,plugin->ProcessedEvents.size()); plugin->MaxAcceptedPerBlock=100u; RenderStationBlock(station,256u);
	EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_TRUE(take->IsMuted());
	EXPECT_EQ(2,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.IsNoteOff();}));
	EXPECT_EQ(1,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.MessageType()==0xb0u && e.data1==64u && e.data2==0u;}));
	station->SetRetainedMuted(false); RenderStationBlock(station,384u);
	EXPECT_EQ(2,std::count_if(plugin->ProcessedEvents.begin(),plugin->ProcessedEvents.end(),[](const auto& e){return e.IsNoteOn();}));
}

TEST(StationMidiInstrument, RetainedMuteGatesActualTakePluginOutputFromFirstBlockButNotStationLiveRack)
{
	auto station=MakeStation("muted-take-plugin"); station->SetNumBusChannels(2u); station->SetupBuffers(32u); station->Mute(); auto take=MakeMidiTake("muted-take-plugin-source");
	take->SetNumBusChannels(2u); take->SetupBuffers(32u); take->Record({0u},station->Name()); take->CommitChanges();
	std::vector<float> silence(constants::MaxLoopFadeSamps+128u,0.0f); base::AudioWriteRequest request;
	request.samples=silence.data(); request.numSamps=static_cast<unsigned int>(silence.size()); request.stride=1u; request.fadeNew=1.0f; request.source=base::Audible::AUDIOSOURCE_ADC;
	take->OnBlockWriteChannel(0u,request,0); take->EndMultiWrite(request.numSamps,true,base::Audible::AUDIOSOURCE_ADC); take->Play(constants::MaxLoopFadeSamps,128u,0u);
	auto plugin=std::make_shared<StationMidiInstrumentFakePlugin>(); plugin->RenderRole=vst::RenderRole::Instrument; plugin->ReplaceOutput=true; plugin->OutputValue=0.75f;
	JobAction load; load.JobActionType=JobAction::JOB_LOADVST; load.VstPath=L"fake-retained-take-output.dll"; load.PreInitPlugin=plugin; take->OnAction(load); take->CommitChanges();
	station->AddTake(take); station->CommitChanges();
	const auto muted=RenderStationAudioBlock(station,0u,32u); ASSERT_GT(plugin->ProcessCalls,0u);
	EXPECT_TRUE(std::all_of(muted.begin(),muted.end(),[](float value){return value==0.0f;}));
	for (auto frame=1u; frame<=8u; ++frame)
	{
		const auto processed=plugin->ProcessCalls; const auto quiet=RenderStationAudioBlock(station,frame*32u,32u);
		EXPECT_GT(plugin->ProcessCalls,processed); EXPECT_TRUE(std::all_of(quiet.begin(),quiet.end(),[](float value){return value==0.0f;}));
	}
	station->SetRetainedMuted(false); ASSERT_FALSE(take->IsMuted()); bool audible=false;
	for (auto frame=9u; frame<25u; ++frame)
	{
		const auto signal=RenderStationAudioBlock(station,frame*32u,32u);
		audible |= std::any_of(signal.begin(),signal.end(),[](float value){return std::abs(value)>0.000001f;});
	}
	EXPECT_TRUE(audible);
	auto live=MakeStation("muted-live-rack"); live->Mute(); auto livePlugin=AddPlugin(live,L"fake-retained-live-output.dll");
	livePlugin->RenderRole=vst::RenderRole::Instrument; livePlugin->ReplaceOutput=true; livePlugin->OutputValue=0.25f;
	const auto monitored=RenderStationAudioBlock(live,0u,32u); EXPECT_TRUE(std::any_of(monitored.begin(),monitored.end(),[](float value){return std::abs(value)>0.000001f;}));
}

TEST(StationMidiInstrument, ExplicitEarlierMidiOrMixedSourceRetainsMaterialAndItsAcceptedTimingBaseline)
{
	for (const bool mixed : {false, true})
	for (const bool remote : {false, true})
	{
		SCOPED_TRACE(mixed);
		SCOPED_TRACE(remote);
		auto station = MakeStation("explicit-midi-source");
		station->SetAllowedMidiChannels({4u});
		auto makeSource = [&](const std::string& id, unsigned int pitch, unsigned int length)
		{
			auto take = MakeMidiTake(id);
			take->Record(mixed ? std::vector<unsigned int>{0u, 1u} : std::vector<unsigned int>{},
				station->Name(), {3u}, {"Keys"});
			engine::RemoteTransportGeometry geometry;
			geometry.IntervalLengthSamps = remote ? length * 2u : length;
			geometry.Bpi = remote ? 8u : 4u;
			take->CaptureRecordingBaselineAtAudioBoundary(geometry, 0u);
			take->CommitChanges();
			if (mixed)
				for (unsigned int channel = 0u; channel < 2u; ++channel)
				{
					std::vector<float> samples(constants::MaxLoopFadeSamps + length, 0.0f);
					std::fill(samples.begin() + constants::MaxLoopFadeSamps, samples.end(), channel ? 0.5f : 0.25f);
					base::AudioWriteRequest request;
					request.samples = samples.data(); request.numSamps = static_cast<unsigned int>(samples.size());
					request.stride = 1u; request.fadeCurrent = 0.0f; request.fadeNew = 1.0f;
					request.source = base::Audible::AUDIOSOURCE_ADC;
					take->GetLoops()[channel]->OnBlockWrite(request, 0);
					take->GetLoops()[channel]->EndWrite(request.numSamps, true);
				}
			take->EndMultiWrite(length, true, base::Audible::AUDIOSOURCE_ADC);
			EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, pitch, 100u), "Keys", length));
			EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, pitch), "Keys", length));
			take->Play(mixed ? constants::MaxLoopFadeSamps : 0u, length, 0u);
			station->AddTake(take); station->CommitChanges();
			return take;
		};
		auto a = makeSource("explicit-A", 60u, 100u);
		auto b = makeSource("later-B", 67u, 200u);
		ASSERT_TRUE(a->IsCompletedRecording());
		engine::Trigger trigger(engine::TriggerParams{}); trigger.SetReceiver(station);
		EXPECT_FALSE(trigger.BindNextOverdubSource(station, a)); // No device phase observed yet.
		a->BeginCaptureAudioBoundary(0u, 0u, 0u);
		ASSERT_TRUE(trigger.BindNextOverdubSource(station, a));
		const auto aRevision = a->GetMidiLoops().front()->Revision();
		const auto bRevision = b->GetMidiLoops().front()->Revision();
		std::vector<std::vector<float>> audioBefore;
		for (const auto& loop : a->GetLoops()) audioBefore.push_back(loop->ExportSamples());
		TriggerAction start;
		start.ActionType = TriggerAction::TRIGGER_OVERDUB_START;
		start.SourceId = a->Id(); start.MidiSample = 0u;
		start.InputChannels = mixed ? std::vector<unsigned int>{0u, 1u} : std::vector<unsigned int>{};
		start.MidiInputDevices = {"Keys"};
		auto result = station->OnAction(start);
		ASSERT_TRUE(result.IsEaten);
		EXPECT_EQ(a->Id(), result.SourceId);
		auto target = std::dynamic_pointer_cast<LoopTake>(result.TriggerTargetTake.lock());
		ASSERT_NE(nullptr, target);
		station->CommitChanges();
		EXPECT_EQ(a->RecordingMasterLengthSamps(), target->RecordingMasterLengthSamps());
		EXPECT_EQ(a->RecordingGeometry().Bpi, target->RecordingGeometry().Bpi);
		target->BeginCaptureAudioBoundary(0u, 0u, 0u);
		target->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);
		target->BeginCaptureAudioBoundary(20u, 20u, 20u); target->PunchIn();
		target->EndMultiWrite(5u, true, base::Audible::AUDIOSOURCE_ADC);
		EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(25u, 3u, 62u, 100u), "Keys", 25u));
		target->EndMultiWrite(5u, true, base::Audible::AUDIOSOURCE_ADC);
		EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(30u, 3u, 62u), "Keys", 30u));
		target->EndMultiWrite(10u, true, base::Audible::AUDIOSOURCE_ADC);
		target->BeginCaptureAudioBoundary(40u, 40u, 40u); target->PunchOut();
		if (mixed)
		{
			// This branch verifies admitted mixed construction/preview. The real audio
			// capture history/completion pipeline is exercised by AudioFlow's A/B fixture.
			ASSERT_EQ(2u, target->GetLoops().size());
			JobAction update; update.JobActionType = JobAction::JOB_UPDATELOOPS;
			target->OnAction(update);
		}
		else
		{
			target->Play(0u, 100u, 0u); station->CompleteMidiIngressPump(4510u);
		}
		ASSERT_EQ(16u, target->GetMidiLoops().size());
		bool sourceNote = false, newNote = false, laterNote = false;
		for (std::size_t i = 0u; i < target->GetMidiLoops()[3]->EventCount(); ++i)
		{
			MidiEvent event{}; ASSERT_TRUE(target->GetMidiLoops()[3]->TryGetEvent(i, event));
			if (event.IsNoteOn())
			{ sourceNote |= event.data1 == 60u; newNote |= event.data1 == 62u; laterNote |= event.data1 == 67u; }
		}
		EXPECT_TRUE(sourceNote); EXPECT_TRUE(newNote); EXPECT_FALSE(laterNote);
		EXPECT_EQ(aRevision, a->GetMidiLoops().front()->Revision());
		EXPECT_EQ(bRevision, b->GetMidiLoops().front()->Revision());
		for (std::size_t i = 0u; i < audioBefore.size(); ++i) EXPECT_EQ(audioBefore[i], a->GetLoops()[i]->ExportSamples());
	}
}

TEST(StationMidiInstrument, ResetModesReleaseSoundedNoteAndPedalBeforeRetirementOrThroughExistingResumeRetry)
{
    for(const bool removeVsts:{false,true})
    {
        SCOPED_TRACE(removeVsts);
        auto station=MakeStation("reset-mode-midi"); auto plugin=AddPlugin(station,L"reset-mode-midi.dll");
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u,0u,60u,100u)));
        ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi({1u,0xb0u,64u,127u})); RenderStationBlock(station,0u);
        ASSERT_EQ(2u,plugin->Events.size()); plugin->Events.clear();
        station->ResetLiveMidiWhileQuiescent(); EXPECT_TRUE(plugin->Events.empty());
        if(removeVsts)
        {
            station->ResetVstRackAfterReadersStopped(); EXPECT_EQ(0u,station->RackSlotCount());
            EXPECT_EQ(nullptr,station->GetVstPlugin(0u));
        }
        else
        {
            plugin->MaxAcceptedPerBlock=0u; RenderStationBlock(station,128u); EXPECT_TRUE(plugin->Events.empty());
            plugin->MaxAcceptedPerBlock=64u; RenderStationBlock(station,256u); EXPECT_EQ(plugin,station->GetVstPlugin(0u));
        }
        EXPECT_EQ(1u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event) { return event.IsNoteOff() && event.data1==60u; }));
        EXPECT_EQ(1u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event) { return event.MessageType()==0xb0u && event.data1==64u && event.data2==0u; }));
        const auto count=plugin->Events.size(); RenderStationBlock(station,384u); EXPECT_EQ(count,plugin->Events.size());
    }
}

// These fixtures inject plugins through the production child load job so format,
// chain publication, exact ownership and parent notification are exercised too.
template<class Owner>
static std::shared_ptr<StationMidiInstrumentFakePlugin> AddChildMidiPlugin(
	const std::shared_ptr<Owner>& owner)
{
	auto plugin = std::make_shared<StationMidiInstrumentFakePlugin>();
	JobAction load;
	load.JobActionType = JobAction::JOB_LOADVST;
	load.VstPath = L"fake-child-midi.dll";
	load.PreInitPlugin = plugin;
	owner->OnAction(load);
	owner->CommitChanges();
	return plugin;
}

static void RecordChildChannels(const std::shared_ptr<LoopTake>& take,
	const std::string& stationName, bool withAudioLeaf)
{
	take->Record(withAudioLeaf ? std::vector<unsigned int>{0u} : std::vector<unsigned int>{},
		stationName, {0u,1u,2u,3u,4u,5u}, {"Keys"});
	take->CommitChanges();
	for (auto channel = 0u; channel < 6u; ++channel)
		EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, channel,
			static_cast<std::uint8_t>(60u + channel), 80u), "Keys", 0u));
	take->Play(0u, 1024u, 0u);
}

TEST(StationMidiInstrument, ChildLiveAndRecordedMasksAreIndependentAtAllThreeDepths)
{
	for (const bool recorded : {false, true})
	{
		SCOPED_TRACE(recorded);
		auto station = MakeStation("three-depth-disjoint");
		auto take = MakeMidiTake("three-depth-take");
		RecordChildChannels(take, station->Name(), true);
		ASSERT_EQ(1u, take->GetLoops().size());
		auto leaf = take->GetLoops().front();
		auto stationPlugin = AddPlugin(station, L"station-disjoint.dll");
		auto takePlugin = AddChildMidiPlugin(take);
		auto leafPlugin = AddChildMidiPlugin(leaf);
		station->SetAllowedMidiChannels({4,5});
		take->SetAllowedMidiChannels({2,3});
		leaf->SetAllowedMidiChannels({1,6});
		if (!recorded) take->SetAllowedMidiChannels({2,3});
		station->AddTake(take); station->CommitChanges();
		if (!recorded)
		{
			take->Mute(); // Live monitoring still receives raw wired MIDI.
			for (auto channel = 0u; channel < 6u; ++channel)
				ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, channel,
					static_cast<std::uint8_t>(72u + channel), 100u)));
		}
		RenderStationBlock(station, 0u);
		for (const auto& plugin : {stationPlugin, takePlugin, leafPlugin})
		{
			EXPECT_EQ(1u, plugin->BeginCalls);
			EXPECT_EQ(1u, plugin->ProcessCalls);
			ASSERT_EQ(2u, plugin->Events.size());
		}
		EXPECT_EQ(3u, stationPlugin->Events[0].Channel()); EXPECT_EQ(4u, stationPlugin->Events[1].Channel());
		EXPECT_EQ(1u, takePlugin->Events[0].Channel()); EXPECT_EQ(2u, takePlugin->Events[1].Channel());
		EXPECT_EQ(0u, leafPlugin->Events[0].Channel()); EXPECT_EQ(5u, leafPlugin->Events[1].Channel());
	}
}

TEST(StationMidiInstrument, EmptyAncestorMasksAndAbsentStationRackDoNotGateChildSources)
{
	auto station = MakeStation("empty-ancestors"); station->SetAllowedMidiChannels({});
	auto take = MakeMidiTake("empty-parent-take");
	RecordChildChannels(take, station->Name(), true);
	take->SetAllowedMidiChannels({});
	auto leafPlugin = AddChildMidiPlugin(take->GetLoops().front());
	take->GetLoops().front()->SetAllowedMidiChannels({1,6});
	station->AddTake(take); station->CommitChanges();
	RenderStationBlock(station, 0u);
	ASSERT_EQ(2u, leafPlugin->Events.size());
	EXPECT_EQ(0u, leafPlugin->Events[0].Channel()); EXPECT_EQ(5u, leafPlugin->Events[1].Channel());
	EXPECT_FALSE(leafPlugin->RealtimeFlags.front());
}

TEST(StationMidiInstrument, PureMidiTakeRendersSameBlockAndTailWithGainAppliedOnce)
{
	auto station = MakeStation("pure-midi-child");
	auto stationPlugin = AddPlugin(station, L"velocity-observer.dll");
	auto take = MakeMidiTake("pure-midi-render");
	RecordChildChannels(take, station->Name(), false);
	take->SetAllowedMidiChannels({1}); station->SetAllowedMidiChannels({1});
	auto plugin = AddChildMidiPlugin(take);
	plugin->RenderRole = vst::RenderRole::Instrument;
	plugin->RenderMidiImpulses = true;
	station->AddTake(take); station->CommitChanges();
	ASSERT_TRUE(take->RestoreMixerLevels(0.5, {}));
	const auto rendered = RenderStationAudioBlock(station, 0u, 128u);
	ASSERT_TRUE(std::any_of(rendered.begin(), rendered.end(), [](float value) { return value != 0.0f; }));
	ASSERT_EQ(1u, plugin->Events.size()); EXPECT_EQ(80u, plugin->Events.front().data2);
	ASSERT_EQ(1u, stationPlugin->Events.size()); EXPECT_EQ(40u, stationPlugin->Events.front().data2);
	EXPECT_FALSE(plugin->RealtimeFlags.front());
	plugin->RenderMidiImpulses = false;
	plugin->ReplaceOutput = true; plugin->OutputValue = 0.25f;
	std::uint32_t nextBlock=128u;
	auto settledOutput = [&]() {
		std::vector<float> output;
		// Existing mixer gains deliberately fade. Observe the steady result after
		// that same bounded fade for both gain changes and mute.
		for (auto block=0u;block<32u;++block)
		{
			output=RenderStationAudioBlock(station,nextBlock,128u);
			nextBlock+=128u;
		}
		return output;
	};
	const auto half=settledOutput();
	ASSERT_TRUE(take->RestoreMixerLevels(1.0, {}));
	const auto full=settledOutput();
	EXPECT_NE(0.0f,full.back()); EXPECT_NEAR(full.back()*0.5f,half.back(),0.00001f);
	take->Mute();
	const auto muted=settledOutput();
	EXPECT_TRUE(std::all_of(muted.begin(),muted.end(),[](float value){return std::abs(value)<0.000001f;}));
	EXPECT_EQ(plugin->BeginCalls,plugin->ProcessCalls); EXPECT_GT(plugin->ProcessCalls,96u);
	EXPECT_TRUE(take->GetLoops().empty());
}

TEST(StationMidiInstrument, ParentFilterRetirementKeepsChildLiveNotesAndPedalsOwned)
{
	auto station = MakeStation("local-retirement");
	auto stationPlugin = AddPlugin(station, L"parent-retirement.dll");
	auto take = MakeMidiTake("retirement-take");
	auto takePlugin = AddChildMidiPlugin(take);
	station->AddTake(take); station->CommitChanges();
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u));
	station->TryEnqueueImmediateLiveMidi({0u,0xb0u,64u,127u});
	RenderStationBlock(station, 0u);
	ASSERT_EQ(2u, stationPlugin->Events.size()); ASSERT_EQ(2u, takePlugin->Events.size());
	station->SetAllowedMidiChannels({}); RenderStationBlock(station, 128u);
	EXPECT_EQ(4u, stationPlugin->Events.size()); EXPECT_EQ(2u, takePlugin->Events.size());
	takePlugin->MaxAcceptedPerBlock = 0u;
	take->SetAllowedMidiChannels({}); station->CommitChanges(); RenderStationBlock(station, 256u);
	EXPECT_EQ(2u, takePlugin->Events.size());
	takePlugin->MaxAcceptedPerBlock = 64u; RenderStationBlock(station, 384u);
	EXPECT_EQ(1u, std::count_if(takePlugin->Events.begin(), takePlugin->Events.end(), [](const auto& event) { return event.IsNoteOff(); }));
	EXPECT_EQ(1u, std::count_if(takePlugin->Events.begin(), takePlugin->Events.end(), [](const auto& event) { return event.MessageType()==0xb0u && event.data1==64u && event.data2==0u; }));
}

TEST(StationMidiInstrument, ChildCablesRejectStaleGesturesAndUnknownSavedIdentityStaysSilent)
{
	auto station = MakeStation("child-cables");
	auto take = MakeMidiTake("child-cable-take");
	auto a = AddChildMidiPlugin(take), b = AddChildMidiPlugin(take);
	station->AddTake(take); station->CommitChanges();
	take->SetAllowedMidiChannels({});
	auto routes = take->MidiChannelRoutes(); routes[0].PluginIds = {a->StateId()};
	const auto revision = take->RackRevision();
	ASSERT_TRUE(take->SetMidiChannelRoutes(routes, revision));
	EXPECT_EQ((std::vector<int>{1}), take->AllowedMidiChannels());
	EXPECT_FALSE(take->SetMidiChannelRoutes({}, revision));
	station->CommitChanges();
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u)); RenderStationBlock(station, 0u);
	EXPECT_EQ(1u, a->Events.size()); EXPECT_TRUE(b->Events.empty());
	routes = take->MidiChannelRoutes(); routes[0].PluginIds = {"missing-child-destination"};
	ASSERT_TRUE(take->RestoreMidiRouting({1}, routes)); station->CommitChanges();
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(128u, 0u, 61u, 100u)); RenderStationBlock(station, 128u);
	EXPECT_EQ(1u, std::count_if(a->Events.begin(), a->Events.end(), [](const auto& event) { return event.IsNoteOn(); }));
	EXPECT_TRUE(b->Events.empty());
	take->SetAllowedMidiChannels({}); take->SetAllowedMidiChannels({1}); station->CommitChanges();
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(256u, 0u, 62u, 100u)); RenderStationBlock(station, 256u);
	EXPECT_TRUE(std::any_of(b->Events.begin(), b->Events.end(), [](const auto& event) { return event.IsNoteOn() && event.data1==62u; }));
}

TEST(StationMidiInstrument, AcceptedSourceMembershipDoesNotBorrowNewerTakeSnapshots)
{
	auto station = MakeStation("pinned-midi-sources");
	auto take = MakeMidiTake("pinned-midi-take");
	RecordChildChannels(take, station->Name(), false); take->SetAllowedMidiChannels({1});
	auto plugin = AddChildMidiPlugin(take);
	station->AddTake(take); station->CommitChanges();
	std::vector<decltype(take->HoldMidiSources())> sourceLeases;
	std::vector<decltype(take->HoldAudioState())> audioLeases;
	for (auto i=0u;i<8u;++i) { sourceLeases.push_back(take->HoldMidiSources()); audioLeases.push_back(take->HoldAudioState()); }
	RenderStationBlock(station, 0u);
	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_EQ(60u, plugin->Events.front().data1);
	EXPECT_EQ(1u, plugin->BeginCalls); EXPECT_EQ(1u, plugin->ProcessCalls);
}

TEST(StationMidiInstrument, BorrowFailureDropsAllStaleAttacksAcrossBoundedIngressDrains)
{
	auto station = MakeStation("borrow-recovery-pressure");
	auto take = MakeMidiTake("borrow-pressure-take");
	auto plugin = AddChildMidiPlugin(take);
	station->AddTake(take); station->CommitChanges();
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u,0u,60u,100u)); RenderStationBlock(station,0u);
	std::vector<decltype(station->HoldAudioState())> leases;
	for (auto i=0u;i<8u;++i) leases.push_back(station->HoldAudioState());
	for (auto i=0u;i<150u;++i) ASSERT_TRUE(station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(128u,0u,61u,100u)));
	RenderStationBlock(station,128u); RenderStationBlock(station,256u);
	EXPECT_EQ(1u,plugin->Events.size()); leases.clear();
	for (auto block=384u;block<1024u;block+=128u) RenderStationBlock(station,block);
	EXPECT_EQ(0u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.IsNoteOn()&&event.data1==61u;}));
	EXPECT_TRUE(std::any_of(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.IsNoteOff()&&event.data1==60u;}));
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(1024u,0u,62u,100u)); RenderStationBlock(station,1024u);
	EXPECT_TRUE(std::any_of(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.IsNoteOn()&&event.data1==62u;}));
}

TEST(StationMidiInstrument, RecordedFanoutStaysWithinContainingTakeAndPartialRejectionDoesNotRetriggerPeers)
{
	auto station = MakeStation("source-fanout"); station->SetAllowedMidiChannels({1});
	auto stationPlugin = AddPlugin(station, L"source-fanout-station.dll");
	auto a = MakeMidiTake("fanout-A"), b = MakeMidiTake("fanout-B");
	for (const auto& take : {a,b})
	{
		take->Record({0u}, station->Name(), {0u}, {"Keys"}); take->CommitChanges();
		EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u,0u,take==a?60u:72u,100u),"Keys",0u));
		take->Play(0u,1024u,0u); take->SetAllowedMidiChannels({1});
		take->GetLoops().front()->SetAllowedMidiChannels({1});
	}
	auto takeA = AddChildMidiPlugin(a), takeB = AddChildMidiPlugin(b);
	auto leafA = AddChildMidiPlugin(a->GetLoops().front()), leafB = AddChildMidiPlugin(b->GetLoops().front());
	leafB->MaxAcceptedPerBlock = 0u;
	station->AddTake(a); station->AddTake(b); station->CommitChanges();
	RenderStationBlock(station,0u);
	ASSERT_EQ(2u,stationPlugin->Events.size());
	ASSERT_EQ(1u,takeA->Events.size()); EXPECT_EQ(60u,takeA->Events.front().data1);
	ASSERT_EQ(1u,leafA->Events.size()); EXPECT_EQ(60u,leafA->Events.front().data1);
	ASSERT_EQ(1u,takeB->Events.size()); EXPECT_EQ(72u,takeB->Events.front().data1);
	EXPECT_TRUE(leafB->Events.empty());
	leafB->MaxAcceptedPerBlock = 64u; RenderStationBlock(station,128u);
	EXPECT_EQ(2u,stationPlugin->Events.size()); EXPECT_EQ(1u,takeA->Events.size());
	EXPECT_EQ(1u,takeB->Events.size()); EXPECT_EQ(1u,leafA->Events.size()); EXPECT_TRUE(leafB->Events.empty());
}

TEST(StationMidiInstrument, RecordedNoneResumeNoneRetainsDestinationReleaseLifecycle)
{
	auto station = MakeStation("recorded-none-cycle"); station->SetAllowedMidiChannels({});
	auto take = MakeMidiTake("recorded-none-take");
	take->Record({},station->Name(),{0u},{"Keys"});
	take->EndMultiWrite(1024u,true,base::Audible::AUDIOSOURCE_ADC);
	for (const auto& event : {MidiEvent::MakeNoteOn(0u,0u,60u,100u), MidiEvent{8u,0xb0u,64u,127u},
		MidiEvent::MakeNoteOff(300u,0u,60u), MidiEvent{310u,0xb0u,64u,0u},
		MidiEvent::MakeNoteOn(512u,0u,62u,100u), MidiEvent{520u,0xb0u,64u,127u}})
		ASSERT_TRUE(take->RecordMidiEvent(event,"Keys",1024u));
	take->Play(0u,1024u,0u); take->SetAllowedMidiChannels({1});
	auto plugin = AddChildMidiPlugin(take);
	station->AddTake(take); station->CommitChanges();
	RenderStationBlock(station,0u);
	plugin->MaxAcceptedPerBlock=0u; take->SetAllowedMidiChannels({}); station->CommitChanges(); RenderStationBlock(station,128u);
	plugin->MaxAcceptedPerBlock=64u; RenderStationBlock(station,256u);
	take->SetAllowedMidiChannels({1}); station->CommitChanges(); RenderStationBlock(station,384u); RenderStationBlock(station,512u);
	plugin->MaxAcceptedPerBlock=0u; take->SetAllowedMidiChannels({}); station->CommitChanges(); RenderStationBlock(station,640u);
	plugin->MaxAcceptedPerBlock=64u; RenderStationBlock(station,768u); RenderStationBlock(station,896u);
	EXPECT_EQ(2u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.IsNoteOn();}));
	EXPECT_EQ(2u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.IsNoteOff();}));
	EXPECT_EQ(2u,std::count_if(plugin->Events.begin(),plugin->Events.end(),[](const auto& event){return event.MessageType()==0xb0u&&event.data1==64u&&event.data2==0u;}));
}

#ifdef _DEBUG
class ChildMidiAllocationProbe
{
public:
	ChildMidiAllocationProbe()
	{
		Allocations=0u; Frees=0u;
		Previous.store(_CrtSetAllocHook(&Hook),std::memory_order_release);
		Armed=true;
	}
	~ChildMidiAllocationProbe() { Armed=false; _CrtSetAllocHook(Previous.load(std::memory_order_acquire)); }
	static thread_local std::size_t Allocations, Frees;
private:
	static thread_local bool Armed;
	static std::atomic<_CRT_ALLOC_HOOK> Previous;
	static int Hook(int type,void* data,std::size_t size,int block,long request,const unsigned char* file,int line)
	{
		if(Armed) { if(type==_HOOK_ALLOC||type==_HOOK_REALLOC) ++Allocations; if(type==_HOOK_FREE) ++Frees; }
		const auto previous=Previous.load(std::memory_order_acquire);
		return previous?previous(type,data,size,block,request,file,line):TRUE;
	}
};
thread_local bool ChildMidiAllocationProbe::Armed=false;
thread_local std::size_t ChildMidiAllocationProbe::Allocations=0u;
thread_local std::size_t ChildMidiAllocationProbe::Frees=0u;
std::atomic<_CRT_ALLOC_HOOK> ChildMidiAllocationProbe::Previous{nullptr};

TEST(StationMidiInstrument, OccupiedChildDeliveryAndRetirementDoNotAllocateOrDestroyOwnershipOnCallback)
{
	auto station=MakeStation("allocation-child-routing"); auto take=MakeMidiTake("allocation-child-take");
	RecordChildChannels(take,station->Name(),true);
	auto parent=AddPlugin(station,L"allocation-parent.dll"), child=AddChildMidiPlugin(take), leaf=AddChildMidiPlugin(take->GetLoops().front());
	station->AddTake(take); station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(160u));
	for(const auto& plugin:{parent,child,leaf})
	{
		plugin->Events.reserve(4096u); plugin->BlockEvents.reserve(1024u);
		plugin->ProcessedEvents.reserve(4096u); plugin->RealtimeFlags.reserve(4096u);
	}
	auto sink=std::make_shared<StationMidiInstrumentCaptureMultiSink>(2u,128u);
	station->TryEnqueueImmediateLiveMidi(MidiEvent::MakeNoteOn(0u,0u,72u,100u));
	station->TryEnqueueImmediateLiveMidi({1u,0xb0u,64u,127u});
	std::size_t allocations=0u,frees=0u;
	{
		ChildMidiAllocationProbe probe;
		for(auto block:{0u,128u}) { station->Zero(128u,base::Audible::AUDIOSOURCE_LOOPS); station->WriteBlock(sink,nullptr,0,128u,block); station->EndMultiPlay(128u); }
		allocations=ChildMidiAllocationProbe::Allocations; frees=ChildMidiAllocationProbe::Frees;
	}
	EXPECT_EQ(0u,allocations); EXPECT_EQ(0u,frees);
	child->MaxAcceptedPerBlock=0u; take->SetAllowedMidiChannels({}); station->CommitChanges();
	{
		ChildMidiAllocationProbe probe;
		station->Zero(128u,base::Audible::AUDIOSOURCE_LOOPS); station->WriteBlock(sink,nullptr,0,128u,256u); station->EndMultiPlay(128u);
		allocations=ChildMidiAllocationProbe::Allocations; frees=ChildMidiAllocationProbe::Frees;
	}
	EXPECT_EQ(0u,allocations); EXPECT_EQ(0u,frees);
	child->MaxAcceptedPerBlock=64u;
	{
		ChildMidiAllocationProbe probe;
		station->Zero(128u,base::Audible::AUDIOSOURCE_LOOPS); station->WriteBlock(sink,nullptr,0,128u,384u); station->EndMultiPlay(128u);
		allocations=ChildMidiAllocationProbe::Allocations; frees=ChildMidiAllocationProbe::Frees;
	}
	EXPECT_EQ(0u,allocations); EXPECT_EQ(0u,frees);
	EXPECT_TRUE(std::any_of(child->Events.begin(),child->Events.end(),[](const auto& event){return event.IsNoteOff();}));
	EXPECT_FALSE(parent->Events.empty()); EXPECT_FALSE(leaf->Events.empty());
}
#endif

TEST(StationMidiInstrument, PureMidiOutputBusWidthAndLevelsAreIndependentOfCaptureStreamCount)
{
	auto station=MakeStation("four-channel-midi-bus"); station->SetNumBusChannels(4u); station->CommitChanges();
	auto take=MakeMidiTake("four-channel-midi-take");
	ASSERT_TRUE(take->RestoreMixerLevels(1.0,{0.25,0.5,0.75,1.0}));
	EXPECT_EQ((std::vector<double>{0.25,0.5,0.75,1.0}),take->BusLevelsForExport());
	auto plugin=AddChildMidiPlugin(take); EXPECT_EQ(4u,plugin->HostedChannels);
	plugin->RenderRole=vst::RenderRole::Instrument; plugin->ReplaceOutput=true; plugin->OutputValue=0.25f;
	station->AddTake(take); station->CommitChanges();
	const auto output=RenderStationAudioBlock(station,0u,128u);
	EXPECT_TRUE(std::any_of(output.begin(),output.end(),[](float value){return value!=0.0f;}));
	EXPECT_TRUE(take->GetLoops().empty()); EXPECT_EQ(1u,plugin->ProcessCalls);
	station->SetNumBusChannels(2u); station->CommitChanges();
	EXPECT_EQ(2u,take->BusLevelsForExport().size());
	const auto shrunk=RenderStationAudioBlock(station,128u,128u);
	EXPECT_TRUE(std::any_of(shrunk.begin(),shrunk.end(),[](float value){return value!=0.0f;}));
	EXPECT_TRUE(plugin->LastBlockProcessed); EXPECT_EQ(2u,plugin->ProcessCalls);
}

TEST(StationMidiInstrument, PendingChildChainDoesNotChangeAcceptedMaterialCompensation)
{
	auto station=MakeStation("accepted-child-latency");
	auto take=MakeMidiTake("accepted-latency-take");
	take->Record({0u},station->Name()); take->CommitChanges(); take->Play(0u,1024u,0u);
	station->AddTake(take); station->CommitChanges();
	auto plugin=AddChildMidiPlugin(take); plugin->LatencySamps=64;
	// Child chain is newer, but the Station has not accepted its delivery/render publication.
	EXPECT_EQ(64u,take->RecordedPlaybackLatencySamps());
	EXPECT_EQ(0u,station->RecordedPlaybackLatencySamps());
	EXPECT_TRUE(station->CanConfigureRecordedPlaybackCompensation(0u));
	EXPECT_TRUE(station->ConfigureRecordedPlaybackCompensation(0u));
	station->CommitChanges();
	EXPECT_EQ(64u,station->RecordedPlaybackLatencySamps());
	EXPECT_TRUE(station->ConfigureRecordedPlaybackCompensation(64u));
}

TEST(StationMidiInstrument, OneParentCommitPublishesCompletedChildAudioChainAndMidiSources)
{
	auto station=MakeStation("single-commit-child"); auto take=MakeMidiTake("single-commit-take");
	station->AddTake(take);
	take->Record({0u},station->Name(),{0u},{"Keys"});
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u,0u,60u,100u),"Keys",0u));
	take->Play(0u,1024u,0u);
	auto plugin=std::make_shared<StationMidiInstrumentFakePlugin>();
	plugin->RenderRole=vst::RenderRole::Instrument; plugin->RenderMidiImpulses=true;
	JobAction job; job.JobActionType=JobAction::JOB_LOADVST; job.VstPath=L"single-commit-child.dll"; job.PreInitPlugin=plugin;
	take->OnAction(job); // Staged child chain and audio membership are still uncommitted.
	station->CommitChanges();
	const auto output=RenderStationAudioBlock(station,0u,128u);
	ASSERT_EQ(1u,plugin->Events.size()); EXPECT_EQ(60u,plugin->Events.front().data1);
	EXPECT_EQ(1u,plugin->BeginCalls); EXPECT_EQ(1u,plugin->ProcessCalls);
	EXPECT_TRUE(std::any_of(output.begin(),output.end(),[](float value){return value!=0.0f;}));
}

TEST(StationMidiInstrument, RetiredBindingAdmissionIsBoundedAndRejectsBeforeCancellingAcceptedRoutes)
{
	midi::MidiDelivery delivery;
	auto take=MakeMidiTake("retained-binding-budget");
	auto first=std::make_shared<midi::MidiLoop>(), second=std::make_shared<midi::MidiLoop>();
	auto plugin=std::make_shared<StationMidiInstrumentFakePlugin>();
	ASSERT_TRUE(plugin->Load(L"",44100.f,128u,2u));
	auto chain=std::make_shared<vst::VstChain>(); chain->AddSlot(plugin);
	std::vector<std::shared_ptr<LoopTake>> takes{take};
	std::array<midi::MidiChannelRoute,16> routes{};
	std::array<midi::MidiDelivery::SourceGroup,1> sources;
	sources[0].Take=take; sources[0].Audio=take->PrepareRenderState()->Audio;
	std::unique_ptr<midi::MidiDelivery::PreparedState> accepted;
	for (std::size_t replacement=0u;replacement<midi::MidiDelivery::MaxRetainedBindings;++replacement)
	{
		sources[0].Sources={replacement%2u?second:first};
		auto candidate=std::make_unique<midi::MidiDelivery::PreparedState>();
		ASSERT_TRUE(delivery.Prepare(*candidate,accepted.get(),chain,takes,0xffffu,routes,2u,{},sources));
		accepted=std::move(candidate);
		delivery.Collect(); // Unacknowledged synthetic releases retain their existing strong pin.
	}
	const auto binding=accepted->MidiPlaybackRoutes.front().front();
	ASSERT_TRUE(binding->RoutingActive.load()); ASSERT_FALSE(binding->ReleaseQueued);
	sources[0].Sources={first};
	midi::MidiDelivery::PreparedState rejected;
	EXPECT_FALSE(delivery.Prepare(rejected,accepted.get(),chain,takes,0xffffu,routes,2u,{},sources));
	EXPECT_TRUE(binding->RoutingActive.load()); EXPECT_FALSE(binding->ReleaseQueued);
	// Re-publishing the accepted identity needs no new binding and still fits at
	// the cap. Rejection cannot force the surviving processor into retirement.
	sources[0].Sources={second};
	midi::MidiDelivery::PreparedState same;
	ASSERT_TRUE(delivery.Prepare(same,accepted.get(),chain,takes,0xffffu,routes,2u,{},sources));
	EXPECT_EQ(binding,same.MidiPlaybackRoutes.front().front());
	EXPECT_TRUE(same.RetiredMidiDestinations.empty());
	RecordProperty("preparedDestinationBytes",static_cast<int>(sizeof(midi::MidiDelivery::MidiDestination)));
	RecordProperty("preparedBindingBytes",static_cast<int>(sizeof(midi::MidiDelivery::MidiPlaybackBinding)));
	delivery.ClearAfterReadersStopped();
}
