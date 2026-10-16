#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <memory>
#include <limits>
#include <vector>

#include "actions/JobAction.h"
#include "audio/ChannelMixer.h"
#include "base/AudioSink.h"
#include "engine/Station.h"
#include "io/UserConfig.h"
#include "midi/MidiEvent.h"
#include "vst/IVstPlugin.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"

class RecordedLatencyCaptureSink final : public base::AudioSink
{
public:
	explicit RecordedLatencyCaptureSink(unsigned int count) : Samples(count, 0.0f) {}

	void OnBlockWrite(const base::AudioWriteRequest& request, int offset) override
	{
		for (auto sample = 0u; sample < request.numSamps; ++sample)
		{
			const auto index = _writeIndex + offset + sample;
			if (index < Samples.size())
				Samples[index] = request.fadeCurrent * Samples[index]
					+ request.fadeNew * request.samples[sample * request.stride];
		}
	}

	void EndWrite(unsigned int count, bool advance) override
	{
		if (advance) _writeIndex += count;
	}

	std::vector<float> Samples;
};

class RecordedLatencyCaptureBus final : public base::MultiAudioSink
{
public:
	explicit RecordedLatencyCaptureBus(unsigned int count)
		: Sink(std::make_shared<RecordedLatencyCaptureSink>(count)) {}

	unsigned int NumInputChannels(base::Audible::AudioSourceType) const override { return 1u; }
	std::shared_ptr<RecordedLatencyCaptureSink> Sink;

protected:
	const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType) override
	{
		return channel == 0u ? Sink : nullptr;
	}
};

static std::shared_ptr<engine::Loop> MakeRecordedLatencyImpulseLoop(unsigned int impulseOffset = 0u)
{
	constexpr auto length = 256u;
	audio::WireMixBehaviourParams wire;
	wire.Channels = { 0u };
	auto mixer = engine::Loop::GetMixerParams({ 80, 80 }, wire);
	engine::LoopParams params;
	params.Wav = "recorded-latency-impulse";
	params.FadeSamps = constants::DefaultFadeSamps;
	auto loop = std::make_shared<engine::Loop>(params, mixer);
	loop->Record();
	std::vector<float> source(constants::MaxLoopFadeSamps + length, 0.0f);
	source[constants::MaxLoopFadeSamps + impulseOffset] = 1.0f;
	base::AudioWriteRequest request;
	request.samples = source.data();
	request.numSamps = static_cast<unsigned int>(source.size());
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(request, 0);
	loop->EndWrite(static_cast<unsigned int>(source.size()), true);
	loop->Play(constants::MaxLoopFadeSamps, length, false);
	return loop;
}

static std::vector<float> ReadRecordedLatencyLoop(const std::shared_ptr<engine::Loop>& loop,
	unsigned int count)
{
	auto sink = std::make_shared<RecordedLatencyCaptureBus>(count);
	loop->WriteBlock(sink, nullptr, 0, count);
	loop->EndMultiPlay(count);
	return sink->Sink->Samples;
}

TEST(RecordedPlaybackLatency, MutedSourceDoesNotLeaveAQueuedImpulseAfterUnmute)
{
	auto loop = MakeRecordedLatencyImpulseLoop();
	ASSERT_TRUE(loop->ConfigureRecordedPlaybackDelay(24u));
	ASSERT_TRUE(loop->Mute());
	ReadRecordedLatencyLoop(loop, 8u);
	ASSERT_TRUE(loop->UnMute());
	std::vector<float> heard;
	for (auto block = 0u; block < 4u; ++block)
	{
		const auto samples = ReadRecordedLatencyLoop(loop, 8u);
		heard.insert(heard.end(), samples.begin(), samples.end());
	}
	// The rack mute may fade the current block. It must not leave
	// a delayed copy that sounds after unmuting.
	EXPECT_TRUE(std::all_of(heard.begin(), heard.end(), [](float sample) { return sample == 0.0f; }));
}

TEST(RecordedPlaybackLatency, DeliberateRelocationClearsQueuedImpulse)
{
	engine::LoopTakeParams params;
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto mixer = engine::LoopTake::GetMixerParams(params.Size, merge);
	auto take = std::make_shared<engine::LoopTake>(params, mixer);
	auto loop = MakeRecordedLatencyImpulseLoop();
	take->AddLoop(loop);
	take->CommitChanges();
	ASSERT_TRUE(loop->ConfigureRecordedPlaybackDelay(24u));
	const auto queued = ReadRecordedLatencyLoop(loop, 8u);
	EXPECT_TRUE(std::all_of(queued.begin(), queued.end(), [](float sample) { return sample == 0.0f; }));
	take->ApplyAcceptedTimingCorrection(1, 1u, true);
	std::vector<float> heard;
	for (auto block = 0u; block < 4u; ++block)
	{
		const auto samples = ReadRecordedLatencyLoop(loop, 8u);
		heard.insert(heard.end(), samples.begin(), samples.end());
	}
	EXPECT_TRUE(std::all_of(heard.begin(), heard.end(), [](float sample) { return sample == 0.0f; }));
}

TEST(RecordedPlaybackLatency, RoutineDisciplineCorrectionPreservesQueuedImpulse)
{
	engine::LoopTakeParams params;
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto mixer = engine::LoopTake::GetMixerParams(params.Size, merge);
	auto take = std::make_shared<engine::LoopTake>(params, mixer);
	auto loop = MakeRecordedLatencyImpulseLoop();
	take->AddLoop(loop);
	take->CommitChanges();
	ASSERT_TRUE(loop->ConfigureRecordedPlaybackDelay(24u));
	ReadRecordedLatencyLoop(loop, 8u);
	take->ApplyAcceptedTimingCorrection(1, 1u, false);
	std::vector<float> heard;
	for (auto block = 0u; block < 4u; ++block)
	{
		const auto samples = ReadRecordedLatencyLoop(loop, 8u);
		heard.insert(heard.end(), samples.begin(), samples.end());
	}
	EXPECT_TRUE(std::any_of(heard.begin(), heard.end(), [](float sample) { return sample > 0.0f; }));
}

class RecordedLatencyImpulsePlugin final : public vst::IVstPlugin
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
	void ProcessBlock(float* mono, int32_t count) noexcept override
	{
		float* channels[] = { mono };
		ProcessBlockMulti(channels, 1, count);
	}
	void ProcessBlockStereo(float* left, float* right, int32_t count) noexcept override
	{
		float* channels[] = { left, right };
		ProcessBlockMulti(channels, 2, count);
	}
	void ProcessBlockMulti(float* const* channels, int32_t numChannels,
		int32_t numSamples) noexcept override
	{
		_processed = false;
		if (!_loaded || _bypassed || !channels || numChannels < 1 || !channels[0]) return;
		for (auto index = 0; index < numSamples; ++index)
		{
			for (auto channel = 0; channel < numChannels && channel < 2; ++channel)
			{
				if (!channels[channel]) continue;
				float sample = channels[channel][index];
				if (channel == 0)
					for (auto eventIndex = 0u; eventIndex < _eventCount; ++eventIndex)
						if (_events[eventIndex].IsNoteOn()
							&& _events[eventIndex].sampleOffset == _blockStart + static_cast<std::uint32_t>(index))
						{
							sample += 1.0f;
							LastAttackSample = _events[eventIndex].sampleOffset;
							++AttackCount;
						}
				_ring[channel][_writeIndex] = sample;
				channels[channel][index] = _ring[channel][
					(_writeIndex + _ring[channel].size() - LatencySamps) % _ring[channel].size()];
			}
			_writeIndex = (_writeIndex + 1u) % _ring[0].size();
		}
		_processed = true;
	}
	void BeginMidiBlock(std::uint32_t blockStart, std::uint32_t) noexcept override
	{
		_blockStart = blockStart;
		_eventCount = 0u;
		_processed = false;
	}
	void SendMidiEvent(const midi::MidiEvent& event, bool realtime) noexcept override
	{
		(void)TrySendMidiEvent(event, realtime);
	}
	bool TrySendMidiEvent(const midi::MidiEvent& event, bool) noexcept override
	{
		if (_eventCount >= _events.size()) return false;
		_events[_eventCount++] = event;
		return true;
	}
	bool MidiBlockWasProcessed() const noexcept override { return _processed; }
	void SetParameter(unsigned int, float) noexcept override {}
	float GetParameter(unsigned int) const noexcept override { return 0.0f; }
	bool OpenEditor(HWND) override { return false; }
	void CloseEditor() override {}
	utils::Size2d GetEditorSize() const noexcept override { return { 0u, 0u }; }
	bool IsLoaded() const noexcept override { return _loaded; }
	const std::string& Name() const noexcept override { return _name; }
	void SetBypassed(bool value) noexcept override { _bypassed = value; }
	bool IsBypassed() const noexcept override { return _bypassed; }
	int GetLatencySamples() const noexcept override { return ReportedLatencySamps; }

	static constexpr unsigned int LatencySamps = 29u;
	int ReportedLatencySamps = static_cast<int>(LatencySamps);
	std::uint32_t LastAttackSample = 0u;
	unsigned int AttackCount = 0u;
private:
	bool _loaded = false;
	bool _bypassed = false;
	bool _processed = false;
	std::string _name = "recorded-latency-impulse";
	std::uint32_t _blockStart = 0u;
	std::array<midi::MidiEvent, 16u> _events{};
	unsigned int _eventCount = 0u;
	std::array<std::array<float, 128u>, 2u> _ring{};
	unsigned int _writeIndex = 0u;
};

TEST(RecordedPlaybackLatency, CapturedAudioAndMidiImpulsesMeetAtTheDac)
{
	constexpr auto blockSize = 128u;
	constexpr auto loopLength = 256u;
	constexpr auto musicalSample = 37u;
	constexpr auto referenceLatency = 47u;
	engine::StationParams stationParams;
	stationParams.Name = "capture-alignment";
	stationParams.Size = { 200, 200 };
	audio::MergeMixBehaviourParams merge;
	auto station = std::make_shared<engine::Station>(stationParams,
		engine::Station::GetMixerParams(stationParams.Size, merge));
	station->SetupBuffers(blockSize);
	station->SetNumBusChannels(1u);
	station->SetNumDacChannels(1u);
	auto plugin = std::make_shared<RecordedLatencyImpulsePlugin>();
	actions::JobAction load;
	load.JobActionType = actions::JobAction::JOB_LOADVST;
	load.VstPath = L"fake-recorded-latency.dll";
	load.PreInitPlugin = plugin;
	station->OnAction(load);
	engine::LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	takeParams.FadeSamps = constants::DefaultFadeSamps;
	auto take = std::make_shared<engine::LoopTake>(takeParams,
		engine::LoopTake::GetMixerParams(takeParams.Size, merge));
	station->AddTake(take);
	take->Record({ 0u }, station->Name(), { 0u }, { "source" }, {}, 0u, 0u);
	station->CommitChanges();
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(musicalSample,
		0u, 60u, 100u), "source", musicalSample));
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(musicalSample + 1u,
		0u, 60u), "source", musicalSample + 1u));
	audio::ChannelMixerParams mixerParams;
	mixerParams.InputBufferSize = blockSize;
	mixerParams.OutputBufferSize = blockSize;
	mixerParams.NumInputChannels = 1u;
	mixerParams.NumOutputChannels = 1u;
	audio::ChannelMixer adc(mixerParams);
	const auto totalRecord = constants::MaxLoopFadeSamps + loopLength;
	std::array<float, blockSize> input{};
	for (auto blockStart = 0u; blockStart < totalRecord; blockStart += blockSize)
	{
		input.fill(0.0f);
		const auto impulse = constants::MaxLoopFadeSamps + musicalSample;
		if (impulse >= blockStart && impulse < blockStart + blockSize)
			input[impulse - blockStart] = 0.5f;
		adc.FromAdc(input.data(), 1u, blockSize);
		adc.InitPlay(0u, blockSize);
		adc.WriteToSink(station, blockSize);
		adc.Source()->EndMultiPlay(blockSize);
		station->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
	}
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0u);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(referenceLatency));
	ASSERT_TRUE(station->ConfigureRecordedMidiCompensation(referenceLatency));
	std::vector<float> rendered;
	for (auto block = 0u; block < 4u; ++block)
	{
		auto sink = std::make_shared<RecordedLatencyCaptureBus>(blockSize);
		station->Zero(blockSize, base::Audible::AUDIOSOURCE_LOOPS);
		station->WriteBlock(sink, nullptr, 0, blockSize, block * blockSize);
		station->EndMultiPlay(blockSize);
		rendered.insert(rendered.end(), sink->Sink->Samples.begin(), sink->Sink->Samples.end());
	}
	const auto target = musicalSample + referenceLatency;
	const auto repeated = target + loopLength;
	ASSERT_LT(repeated, rendered.size());
	EXPECT_GT(rendered[target], 1.0f);
	EXPECT_GT(rendered[repeated], 1.0f);
	for (auto sample = 0u; sample < rendered.size(); ++sample)
		if (sample != target && sample != repeated)
			EXPECT_NEAR(0.0f, rendered[sample], 1.0e-4f) << "unexpected transient at " << sample;
}

TEST(RecordedPlaybackLatency, RapidReferenceChangeWaitsForAudioTransitionBeforeMovingMidi)
{
	engine::StationParams stationParams;
	stationParams.Name = "latency-transition";
	stationParams.Size = { 200, 200 };
	audio::MergeMixBehaviourParams merge;
	auto station = std::make_shared<engine::Station>(stationParams,
		engine::Station::GetMixerParams(stationParams.Size, merge));
	station->SetupBuffers(128u);
	station->SetNumBusChannels(1u);
	station->SetNumDacChannels(1u);
	auto plugin = std::make_shared<RecordedLatencyImpulsePlugin>();
	actions::JobAction load;
	load.JobActionType = actions::JobAction::JOB_LOADVST;
	load.VstPath = L"fake-transition.dll";
	load.PreInitPlugin = plugin;
	station->OnAction(load);
	engine::LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	auto take = std::make_shared<engine::LoopTake>(takeParams,
		engine::LoopTake::GetMixerParams(takeParams.Size, merge));
	take->Record({}, station->Name(), { 0u }, { "source" }, {}, 0u, 0u);
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(37u, 0u, 60u, 100u),
		"source", 37u));
	take->Play(0u, 1000u, 0u);
	take->AddLoop(MakeRecordedLatencyImpulseLoop());
	take->CommitChanges();
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(47u));
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(63u));
	EXPECT_FALSE(station->CanConfigureRecordedPlaybackCompensation(79u));
	EXPECT_FALSE(station->ConfigureRecordedPlaybackCompensation(79u));
	// A rejected audio plan must leave MIDI on the last accepted reference.
	auto sink = std::make_shared<RecordedLatencyCaptureBus>(128u);
	station->Zero(128u, base::Audible::AUDIOSOURCE_LOOPS);
	station->WriteBlock(sink, nullptr, 0, 128u, 0u);
	station->EndMultiPlay(128u);
	ASSERT_EQ(1u, plugin->AttackCount);
	EXPECT_EQ(37u + 63u - RecordedLatencyImpulsePlugin::LatencySamps,
		plugin->LastAttackSample);
	EXPECT_TRUE(station->CanConfigureRecordedPlaybackCompensation(79u));
	EXPECT_TRUE(station->ConfigureRecordedPlaybackCompensation(79u));
}

static void VerifyRecordedCaptureOriginWithHardwareRuler(bool returningOutput,
	int quantisationError = 0, int acceptedCorrection = 0)
{
	constexpr auto blockSize = 128u;
	constexpr auto loopLength = 256u;
	constexpr auto musicalSample = 37u;
	constexpr auto inputLatency = 11u;
	constexpr auto outputLatency = 13u;
	constexpr auto preDelay = 17u;
	constexpr auto referenceLatency = 47u;
	io::UserConfig cfg;
	cfg.Trigger.PreDelaySamps = preDelay;
	engine::StationParams stationParams;
	stationParams.Name = returningOutput ? "overdub-capture-ruler" : "record-capture-ruler";
	stationParams.Size = { 200, 200 };
	audio::MergeMixBehaviourParams merge;
	auto station = std::make_shared<engine::Station>(stationParams,
		engine::Station::GetMixerParams(stationParams.Size, merge));
	station->SetupBuffers(blockSize);
	station->SetNumBusChannels(1u);
	station->SetNumDacChannels(1u);
	auto plugin = std::make_shared<RecordedLatencyImpulsePlugin>();
	actions::JobAction load;
	load.JobActionType = actions::JobAction::JOB_LOADVST;
	load.VstPath = L"fake-capture-ruler.dll";
	load.PreInitPlugin = plugin;
	station->OnAction(load);
	engine::LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	takeParams.FadeSamps = constants::DefaultFadeSamps;
	auto take = std::make_shared<engine::LoopTake>(takeParams,
		engine::LoopTake::GetMixerParams(takeParams.Size, merge));
	station->AddTake(take);
	take->Record({ 0u }, station->Name(), { 0u }, { "source" }, {}, 0u, 0u);
	station->CommitChanges();
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(musicalSample,
		0u, 60u, 100u), "source", musicalSample));
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(musicalSample + 1u,
		0u, 60u), "source", musicalSample + 1u));
	// A returning DAC source reaches ADC after both device latencies. Ordinary
	// first-record capture has no output trip. MIDI keeps its trigger ruler.
	const auto physicalAdcImpulse = musicalSample + inputLatency
		+ (returningOutput ? outputLatency : 0u);
	audio::ChannelMixerParams mixerParams;
	mixerParams.InputBufferSize = constants::MaxLoopFadeSamps + preDelay + 2u * blockSize;
	mixerParams.OutputBufferSize = blockSize;
	mixerParams.NumInputChannels = 1u;
	mixerParams.NumOutputChannels = 1u;
	audio::ChannelMixer adc(mixerParams);
	const auto totalRecord = constants::MaxLoopFadeSamps + loopLength
		+ preDelay + outputLatency + blockSize;
	std::array<float, blockSize> input{};
	for (auto blockStart = 0u; blockStart < totalRecord; blockStart += blockSize)
	{
		input.fill(0.0f);
		if (physicalAdcImpulse >= blockStart
			&& physicalAdcImpulse < blockStart + blockSize)
			input[physicalAdcImpulse - blockStart] = 0.5f;
		adc.FromAdc(input.data(), 1u, blockSize);
		adc.InitPlay(cfg.AdcBufferDelay(inputLatency), blockSize);
		adc.WriteToSink(station, blockSize);
		adc.Source()->EndMultiPlay(blockSize);
		station->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
	}
	const auto audioPlayIndex = returningOutput
		? cfg.LoopPlayPos(quantisationError, loopLength, outputLatency)
		: cfg.OverdubPlayPos(quantisationError, loopLength);
	// The audio index includes the fade prefix and pre-delay. The recorded
	// MIDI offset remains the musical sample inside the loop.
	EXPECT_EQ(static_cast<unsigned int>(static_cast<int>(constants::MaxLoopFadeSamps
		+ preDelay + (returningOutput ? outputLatency : 0u)) + quantisationError),
		audioPlayIndex + loopLength);
	take->Play(audioPlayIndex, loopLength, 0u, quantisationError);
	if (acceptedCorrection != 0)
		take->ApplyAcceptedTimingCorrection(acceptedCorrection, 1u, false);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(referenceLatency));
	std::vector<float> rendered;
	for (auto block = 0u; block < 4u; ++block)
	{
		auto sink = std::make_shared<RecordedLatencyCaptureBus>(blockSize);
		station->Zero(blockSize, base::Audible::AUDIOSOURCE_LOOPS);
		station->WriteBlock(sink, nullptr, 0, blockSize, block * blockSize);
		station->EndMultiPlay(blockSize);
		rendered.insert(rendered.end(), sink->Sink->Samples.begin(), sink->Sink->Samples.end());
	}
	const auto expected = static_cast<unsigned int>(
		static_cast<int>(musicalSample + referenceLatency)
		- quantisationError - acceptedCorrection);
	ASSERT_LT(expected + loopLength, rendered.size());
	// LoopPlayPos returns a cursor one body early; Loop::ReadBlock maps
	// that cursor into the playable body before rendering. Both captured
	// transients must therefore meet on the first iteration too.
	EXPECT_GT(rendered[expected], 1.0f);
	EXPECT_GT(rendered[expected + loopLength], 1.0f);
	for (auto sample = 0u; sample < rendered.size(); ++sample)
		if (sample != expected && sample != expected + loopLength)
			EXPECT_NEAR(0.0f, rendered[sample], 1.0e-4f)
				<< "unexpected rendered sample " << sample << " returning=" << returningOutput
				<< " error=" << quantisationError << " correction=" << acceptedCorrection;
}

TEST(RecordedPlaybackLatency, FirstRecordCaptureUsesInputLatencyAndPreDelayRuler)
{
	VerifyRecordedCaptureOriginWithHardwareRuler(false);
}

TEST(RecordedPlaybackLatency, ReturningOutputCaptureAlsoUsesOutputLatencyRuler)
{
	VerifyRecordedCaptureOriginWithHardwareRuler(true);
}

TEST(RecordedPlaybackLatency, CaptureRulerIncludesPositiveErrorAndRoutineCorrection)
{
	VerifyRecordedCaptureOriginWithHardwareRuler(false, 9, 13);
}

TEST(RecordedPlaybackLatency, ReturningCaptureRulerIncludesNegativeErrorAndCorrection)
{
	VerifyRecordedCaptureOriginWithHardwareRuler(true, -9, -7);
}

static void VerifySerialRenderedPath(bool indexedStationDestination)
{
	constexpr auto blockSize = 128u;
	constexpr auto loopLength = 256u;
	constexpr auto musicalSample = 37u;
	constexpr auto referenceLatency = 127u;
	audio::MergeMixBehaviourParams merge;
	engine::StationParams stationParams;
	stationParams.Name = indexedStationDestination ? "serial-indexed" : "serial-default";
	stationParams.Size = { 200, 200 };
	auto station = std::make_shared<engine::Station>(stationParams,
		engine::Station::GetMixerParams(stationParams.Size, merge));
	station->SetupBuffers(blockSize);
	station->SetNumBusChannels(1u);
	station->SetNumDacChannels(1u);
	engine::LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	auto take = std::make_shared<engine::LoopTake>(takeParams,
		engine::LoopTake::GetMixerParams(takeParams.Size, merge));
	take->Record({}, station->Name(), { 0u }, { "source" }, {}, 0u, 0u);
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(musicalSample,
		0u, 60u, 100u), "source", musicalSample));
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(musicalSample + 1u,
		0u, 60u), "source", musicalSample + 1u));
	take->Play(0u, loopLength, 0u);
	auto loop = MakeRecordedLatencyImpulseLoop(musicalSample);
	take->AddLoop(loop);
	const auto loadDelay = [](const auto& target, const wchar_t* path)
	{
		auto plugin = std::make_shared<RecordedLatencyImpulsePlugin>();
		actions::JobAction job;
		job.JobActionType = actions::JobAction::JOB_LOADVST;
		job.VstPath = path;
		job.PreInitPlugin = plugin;
		target->OnAction(job);
		target->CommitChanges();
		return plugin;
	};
	auto loopEffect = loadDelay(loop, L"fake-loop-delay.dll");
	auto takeEffect = loadDelay(take, L"fake-take-delay.dll");
	auto firstStationEffect = loadDelay(station, L"fake-station-delay.dll");
	std::shared_ptr<RecordedLatencyImpulsePlugin> secondStationEffect;
	if (indexedStationDestination)
	{
		secondStationEffect = loadDelay(station, L"fake-station-synth.dll");
		station->SetMidiVstRoute(0u, 1u);
	}
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_EQ(RecordedLatencyImpulsePlugin::LatencySamps,
		static_cast<unsigned int>(loop->CurrentVstLatencySamps()));
	ASSERT_EQ(RecordedLatencyImpulsePlugin::LatencySamps,
		static_cast<unsigned int>(take->CurrentVstLatencySamps()));
	ASSERT_EQ(RecordedLatencyImpulsePlugin::LatencySamps *
		(indexedStationDestination ? 2u : 1u),
		static_cast<unsigned int>(station->CurrentVstLatencySamps()));
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(referenceLatency));
	std::vector<float> rendered;
	for (auto block = 0u; block < 4u; ++block)
	{
		auto sink = std::make_shared<RecordedLatencyCaptureBus>(blockSize);
		station->Zero(blockSize, base::Audible::AUDIOSOURCE_LOOPS);
		station->WriteBlock(sink, nullptr, 0, blockSize, block * blockSize);
		station->EndMultiPlay(blockSize);
		rendered.insert(rendered.end(), sink->Sink->Samples.begin(), sink->Sink->Samples.end());
	}
	const auto& synth = indexedStationDestination ? secondStationEffect : firstStationEffect;
	ASSERT_NE(nullptr, synth);
	EXPECT_EQ(2u, synth->AttackCount);
	if (indexedStationDestination)
		EXPECT_EQ(0u, firstStationEffect->AttackCount);
	const auto expected = musicalSample + referenceLatency;
	ASSERT_LT(expected + loopLength, rendered.size());
	EXPECT_GT(rendered[expected], 1.0f);
	EXPECT_GT(rendered[expected + loopLength], 1.0f);
	for (auto sample = 0u; sample < rendered.size(); ++sample)
		if (sample != expected && sample != expected + loopLength)
			EXPECT_NEAR(0.0f, rendered[sample], 1.0e-4f)
				<< "unexpected serial path sample " << sample
				<< " indexed=" << indexedStationDestination;
}

TEST(RecordedPlaybackLatency, SerialLoopTakeStationEffectsMeetRecordedMidiAtDac)
{
	VerifySerialRenderedPath(false);
}

TEST(RecordedPlaybackLatency, IndexedStationMidiSkipsUpstreamPluginDelay)
{
	VerifySerialRenderedPath(true);
}

TEST(RecordedPlaybackLatency, ThreeLevelLatencySaturatesWithoutWrapping)
{
	audio::MergeMixBehaviourParams merge;
	engine::StationParams stationParams;
	stationParams.Name = "saturated-latency";
	stationParams.Size = { 200, 200 };
	auto station = std::make_shared<engine::Station>(stationParams,
		engine::Station::GetMixerParams(stationParams.Size, merge));
	station->SetupBuffers(128u);
	station->SetNumBusChannels(1u);
	station->SetNumDacChannels(1u);
	engine::LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	auto take = std::make_shared<engine::LoopTake>(takeParams,
		engine::LoopTake::GetMixerParams(takeParams.Size, merge));
	take->Record({}, station->Name(), { 0u }, { "source" }, {}, 0u, 0u);
	take->Play(0u, 256u, 0u);
	auto loop = MakeRecordedLatencyImpulseLoop();
	take->AddLoop(loop);
	const auto loadMaxLatency = [](const auto& target, const wchar_t* pluginPath)
	{
		auto plugin = std::make_shared<RecordedLatencyImpulsePlugin>();
		plugin->ReportedLatencySamps = (std::numeric_limits<int>::max)();
		actions::JobAction job;
		job.JobActionType = actions::JobAction::JOB_LOADVST;
		job.VstPath = pluginPath;
		job.PreInitPlugin = plugin;
		target->OnAction(job);
		target->CommitChanges();
	};
	loadMaxLatency(loop, L"fake-max-loop.dll");
	loadMaxLatency(take, L"fake-max-take.dll");
	loadMaxLatency(station, L"fake-max-station.dll");
	station->AddTake(take);
	station->CommitChanges();
	EXPECT_EQ((std::numeric_limits<unsigned int>::max)() - 1u,
		take->RecordedPlaybackLatencySamps());
	EXPECT_EQ((std::numeric_limits<unsigned int>::max)(),
		station->RecordedPlaybackLatencySamps());
	EXPECT_FALSE(station->CanConfigureRecordedPlaybackCompensation(
		(std::numeric_limits<unsigned int>::max)()));
	EXPECT_FALSE(station->ConfigureRecordedPlaybackCompensation(
		(std::numeric_limits<unsigned int>::max)()));
}
