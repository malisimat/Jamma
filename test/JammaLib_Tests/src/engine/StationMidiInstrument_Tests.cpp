#include "gtest/gtest.h"

#include "actions/JobAction.h"
#include "actions/TriggerAction.h"
#include "base/AudioSink.h"
#include "engine/Station.h"

using actions::JobAction;
using actions::TriggerAction;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::MidiEvent;
using engine::Station;
using engine::StationParams;

namespace
{
	class FakeMidiPlugin final : public vst::IVstPlugin
	{
	public:
		bool PreInit(const std::wstring&) override { return true; }

		bool Load(const std::wstring&, float, unsigned int, unsigned int,
			vst::HostedLayoutMode = vst::HostedLayoutMode::Exact) override
		{
			_loaded = true;
			return true;
		}

		void Unload() override { _loaded = false; }
		void ProcessBlock(float*, int32_t) noexcept override {}
		void ProcessBlockStereo(float*, float*, int32_t) noexcept override {}

		void ProcessBlockMulti(float* const*, int32_t, int32_t) noexcept override
		{
			ProcessCalls++;
		}

		void BeginMidiBlock(std::uint32_t blockStartSample,
			std::uint32_t numSamples) noexcept override
		{
			BlockStart = blockStartSample;
			BlockSamples = numSamples;
			BeginCalls++;
		}

		void SendMidiEvent(const MidiEvent& event, bool isRealtime) noexcept override
		{
			Events.push_back(event);
			RealtimeFlags.push_back(isRealtime);
		}

		bool OpenEditor(HWND) override { return false; }
		void CloseEditor() override {}
		utils::Size2d GetEditorSize() const noexcept override { return { 0, 0 }; }
		bool IsLoaded() const noexcept override { return _loaded; }
		const std::string& Name() const noexcept override { return _name; }
		void SetBypassed(bool bypass) noexcept override { _bypassed = bypass; }
		bool IsBypassed() const noexcept override { return _bypassed; }

		std::uint32_t BlockStart = 0u;
		std::uint32_t BlockSamples = 0u;
		unsigned int BeginCalls = 0u;
		unsigned int ProcessCalls = 0u;
		std::vector<MidiEvent> Events;
		std::vector<bool> RealtimeFlags;

	private:
		bool _loaded = false;
		bool _bypassed = false;
		std::string _name = "fake-midi-plugin";
	};

	class CaptureSink : public base::AudioSink
	{
	public:
		explicit CaptureSink(unsigned int bufSize) : Samples(bufSize, 0.0f) {}

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

	class CaptureMultiSink : public base::MultiAudioSink
	{
	public:
		CaptureMultiSink(unsigned int numChannels, unsigned int bufSize)
		{
			for (auto channel = 0u; channel < numChannels; ++channel)
				_sinks.push_back(std::make_shared<CaptureSink>(bufSize));
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

	private:
		std::vector<std::shared_ptr<CaptureSink>> _sinks;
	};

	std::shared_ptr<Station> MakeStation(const std::string& name)
	{
		StationParams params;
		params.Name = name;
		params.Size = { 200, 200 };
		audio::MergeMixBehaviourParams merge;
		auto mixerParams = Station::GetMixerParams(params.Size, merge);
		auto station = std::make_shared<Station>(params, mixerParams);
		station->SetupBuffers(128u);
		station->SetNumDacChannels(2u);
		station->CommitChanges();
		return station;
	}

	std::shared_ptr<FakeMidiPlugin> AddPlugin(const std::shared_ptr<Station>& station,
		const std::wstring& path)
	{
		auto plugin = std::make_shared<FakeMidiPlugin>();
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.VstPath = path;
		job.PreInitPlugin = plugin;
		station->OnAction(job);
		station->CommitChanges();
		return plugin;
	}

	void RenderStationBlock(const std::shared_ptr<Station>& station,
		std::uint32_t blockStart,
		unsigned int numSamps = 128u)
	{
		auto sink = std::make_shared<CaptureMultiSink>(2u, numSamps);
		station->Zero(numSamps, base::Audible::AUDIOSOURCE_LOOPS);
		station->WriteBlock(sink, nullptr, 0, numSamps, blockStart);
		station->EndMultiPlay(numSamps);
	}

	std::shared_ptr<LoopTake> MakeMidiTake(const std::string& id)
	{
		LoopTakeParams params;
		params.Id = id;
		params.Size = { 100, 100 };
		audio::MergeMixBehaviourParams merge;
		auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
		return std::make_shared<LoopTake>(params, mixerParams);
	}
}

TEST(StationMidiInstrument, LiveMidiIsDeliveredToStationVstPlugin)
{
	auto station = MakeStation("station-live");
	auto plugin = AddPlugin(station, L"fake-live.dll");

	auto event = MidiEvent::MakeNoteOn(72u, 0u, 60u, 100u);
	station->EnqueueLiveMidiEvent(event);

	RenderStationBlock(station, 64u);

	ASSERT_EQ(1u, plugin->Events.size());
	EXPECT_EQ(event.status, plugin->Events[0].status);
	EXPECT_EQ(event.data1, plugin->Events[0].data1);
	EXPECT_TRUE(plugin->RealtimeFlags[0]);
	EXPECT_EQ(64u, plugin->BlockStart);
	EXPECT_EQ(128u, plugin->BlockSamples);
}

TEST(StationMidiInstrument, SameLiveMidiInputCanPlayMultipleStations)
{
	auto stationA = MakeStation("station-a");
	auto stationB = MakeStation("station-b");
	auto pluginA = AddPlugin(stationA, L"fake-a.dll");
	auto pluginB = AddPlugin(stationB, L"fake-b.dll");

	auto event = MidiEvent::MakeNoteOn(10u, 1u, 64u, 96u);
	stationA->EnqueueLiveMidiEvent(event);
	stationB->EnqueueLiveMidiEvent(event);

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
	station->SetMidiVstRoute(Station::LiveMidiOutputIndex, 1u);

	station->EnqueueLiveMidiEvent(MidiEvent::MakeNoteOn(16u, 0u, 36u, 120u));
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

	take->Record({}, station->Name(), { 0u }, { "Keys" });
	take->RecordMidiEvent(MidiEvent::MakeNoteOn(24u, 0u, 48u, 100u), "Keys", 24u);
	take->Play(0u, 128u, 0u);
	station->SetMidiVstRoute(0u, 1u);

	RenderStationBlock(station, 0u);

	EXPECT_TRUE(pluginA->Events.empty());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(48u, pluginB->Events[0].data1);
	EXPECT_FALSE(pluginB->RealtimeFlags[0]);
}

TEST(StationMidiInstrument, SetMidiVstRouteReplacesPreviousRouteForOutput)
{
	auto station = MakeStation("station-route-replace");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");
	auto pluginC = AddPlugin(station, L"fake-c.dll");

	station->SetMidiVstRoute(Station::LiveMidiOutputIndex, 1u);
	station->SetMidiVstRoute(Station::LiveMidiOutputIndex, 2u);
	station->EnqueueLiveMidiEvent(MidiEvent::MakeNoteOn(8u, 0u, 40u, 100u));

	RenderStationBlock(station, 0u);

	EXPECT_TRUE(pluginA->Events.empty());
	EXPECT_TRUE(pluginB->Events.empty());
	ASSERT_EQ(1u, pluginC->Events.size());
	EXPECT_EQ(40u, pluginC->Events[0].data1);
}

TEST(StationMidiInstrument, ClearMidiVstRoutesRestoresWholeChainDelivery)
{
	auto station = MakeStation("station-route-clear");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");

	station->SetMidiVstRoute(Station::LiveMidiOutputIndex, 1u);
	station->ClearMidiVstRoutes();
	station->EnqueueLiveMidiEvent(MidiEvent::MakeNoteOn(8u, 0u, 41u, 100u));

	RenderStationBlock(station, 0u);

	ASSERT_EQ(1u, pluginA->Events.size());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(41u, pluginA->Events[0].data1);
	EXPECT_EQ(41u, pluginB->Events[0].data1);
}

TEST(StationMidiInstrument, InvalidRouteFallsBackToWholeChainDelivery)
{
	auto station = MakeStation("station-route-invalid");
	auto pluginA = AddPlugin(station, L"fake-a.dll");
	auto pluginB = AddPlugin(station, L"fake-b.dll");

	station->SetMidiVstRoute(Station::LiveMidiOutputIndex, 99u);
	station->EnqueueLiveMidiEvent(MidiEvent::MakeNoteOn(8u, 0u, 42u, 100u));

	RenderStationBlock(station, 0u);

	ASSERT_EQ(1u, pluginA->Events.size());
	ASSERT_EQ(1u, pluginB->Events.size());
	EXPECT_EQ(42u, pluginA->Events[0].data1);
	EXPECT_EQ(42u, pluginB->Events[0].data1);
}

TEST(StationMidiInstrument, OverdubStartPassesSourceMidiToTargetTake)
{
	auto station = MakeStation("station-overdub-source");
	auto sourceTake = MakeMidiTake("source-midi-take");
	station->AddTake(sourceTake);
	station->CommitChanges();

	sourceTake->Record({}, station->Name(), { 3u }, { "Keys" });
	sourceTake->EndMultiWrite(100u, true, base::Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "Keys", 100u));
	EXPECT_TRUE(sourceTake->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "Keys", 100u));
	sourceTake->Play(0u, 100u, 0u);

	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_OVERDUB_START;
	start.InputChannels = {};
	start.MidiInputChannels = { 3u };
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
	ASSERT_EQ(1u, targetTake->GetMidiLoops().size());
	ASSERT_EQ(3u, targetTake->MidiLoopChannels()[0]);
	ASSERT_EQ("Keys", targetTake->MidiLoopDevices()[0]);

	targetTake->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);
	targetTake->PunchIn();
	targetTake->EndMultiWrite(20u, true, base::Audible::AUDIOSOURCE_ADC);
	targetTake->PunchOut();
	targetTake->Play(0u, 100u, 0u);

	ASSERT_EQ(4u, targetTake->GetMidiLoops()[0]->EventCount());
	MidiEvent event{};
	ASSERT_TRUE(targetTake->GetMidiLoops()[0]->TryGetEvent(0u, event));
	EXPECT_EQ(10u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[0]->TryGetEvent(1u, event));
	EXPECT_EQ(20u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[0]->TryGetEvent(2u, event));
	EXPECT_EQ(40u, event.sampleOffset);
	ASSERT_TRUE(targetTake->GetMidiLoops()[0]->TryGetEvent(3u, event));
	EXPECT_EQ(90u, event.sampleOffset);
}

TEST(StationMidiInstrument, PunchBoundariesEmitLiveMidiTransitionsForSourceAndLiveHeldNotes)
{
	auto station = MakeStation("station-punch-live-boundary");
	auto plugin = AddPlugin(station, L"fake-punch-live.dll");
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