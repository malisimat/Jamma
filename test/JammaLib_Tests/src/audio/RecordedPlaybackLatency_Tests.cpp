#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <memory>
#include <limits>
#include <vector>

#include "actions/JobAction.h"
#include "audio/ChannelMixer.h"
#include "audio/Hanning.h"
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
	explicit RecordedLatencyCaptureBus(unsigned int count, unsigned int channel = 0u)
		: Sink(std::make_shared<RecordedLatencyCaptureSink>(count)), Channel(channel) {}

	unsigned int NumInputChannels(base::Audible::AudioSourceType) const override { return Channel + 1u; }
	std::shared_ptr<RecordedLatencyCaptureSink> Sink;
    unsigned int Channel;

protected:
	const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType) override
	{
		return channel == Channel ? Sink : nullptr;
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
	int quantisationError = 0, int acceptedCorrection = 0, unsigned int inputLatency = 11u)
{
	constexpr auto blockSize = 128u;
	constexpr auto loopLength = 256u;
	constexpr auto musicalSample = 37u;
	constexpr auto outputLatency = 13u;
	constexpr auto preDelay = 0u;
	constexpr auto referenceLatency = 47u;
	io::UserConfig cfg;
	const auto reserve = io::UserConfig::TriggerHistorySamples(constants::DefaultSampleRate).value();
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
	take->ConfigureTriggerCapture({0u, 0, reserve, (std::max)(inputLatency, constants::MaxLoopFadeSamps + reserve), 0u});
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
	mixerParams.InputBufferSize = constants::MaxLoopFadeSamps + reserve + 2u * blockSize;
	mixerParams.OutputBufferSize = blockSize;
	mixerParams.NumInputChannels = 1u;
	mixerParams.NumOutputChannels = 1u;
	audio::ChannelMixer adc(mixerParams);
	const auto totalRecord = take->CaptureTiming().SourceAgeSamps + loopLength
        + outputLatency + blockSize;
	const auto playbackBoundary = ((totalRecord + blockSize - 1u) / blockSize) * blockSize;
	std::array<float, blockSize> input{};
	for (auto blockStart = 0u; blockStart < totalRecord; blockStart += blockSize)
	{
		input.fill(0.0f);
		if (physicalAdcImpulse >= blockStart
			&& physicalAdcImpulse < blockStart + blockSize)
			input[physicalAdcImpulse - blockStart] = 0.5f;
		take->BeginCaptureAudioBoundary(blockStart, blockStart, blockStart);
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
	take->SetTriggerCaptureStop(loopLength + quantisationError);
	take->Play(audioPlayIndex, loopLength, 0u, quantisationError, false, returningOutput ? outputLatency : 0u);
	take->CompleteMidiIngressPump(playbackBoundary);
	take->EndRecording();
	take->BeginCaptureAudioBoundary(playbackBoundary, playbackBoundary, playbackBoundary);
	if (acceptedCorrection != 0)
		take->ApplyAcceptedTimingCorrection(acceptedCorrection, 1u, false);
	station->CommitChanges();
	ASSERT_TRUE(station->ConfigureRecordedPlaybackCompensation(referenceLatency));
	std::vector<float> rendered;
	for (auto block = 0u; block < 6u; ++block)
	{
		auto sink = std::make_shared<RecordedLatencyCaptureBus>(blockSize);
		station->Zero(blockSize, base::Audible::AUDIOSOURCE_LOOPS);
		station->WriteBlock(sink, nullptr, 0, blockSize, block * blockSize);
		station->EndMultiPlay(blockSize);
		rendered.insert(rendered.end(), sink->Sink->Samples.begin(), sink->Sink->Samples.end());
	}
	const auto expected = static_cast<unsigned int>((static_cast<int>(musicalSample)
		- static_cast<int>(playbackBoundary % loopLength) - acceptedCorrection
		+ static_cast<int>(loopLength)) % static_cast<int>(loopLength)) + referenceLatency;
	ASSERT_LT(expected + loopLength, rendered.size());
	// LoopPlayPos returns a cursor one body early; Loop::ReadBlock maps
	// that cursor into the playable body before rendering. Both captured
	// transients must therefore meet on the first iteration too.
	EXPECT_GT(rendered[expected], 1.0f);
	EXPECT_GT(rendered[expected + loopLength], 1.0f);
	for (auto sample = 0u; sample < rendered.size(); ++sample)
		if (sample < expected || (sample - expected) % loopLength != 0u)
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

TEST(RecordedPlaybackLatency, SignedCaptureNormalizesOnlyTheSelectedPhysicalPass)
{
	for (const int delay : { -97, 0, 97 })
	{
		constexpr unsigned long length = 2048u;
		const auto seam = delay > 0 ? delay + 10 : 0;
		const auto begin = static_cast<std::int64_t>(constants::MaxLoopFadeSamps) + delay - seam;
		audio::WireMixBehaviourParams wire;
		wire.Channels = { 0u };
		engine::LoopParams params;
		params.FadeSamps = 0u;
		auto loop = std::make_shared<engine::Loop>(params, engine::Loop::GetMixerParams({80,80}, wire));
		loop->Record();
		loop->ConfigureTriggerCapture(begin, true, false);
		loop->SetTriggerCaptureCursor(begin);
		ASSERT_TRUE(loop->SetTriggerCaptureLength(length));
		std::vector<float> input(length + 31u, 0.0f);
		input[seam] = 1.0f; // E=T, independent of signed content origin
		input[0] = 0.25f; // eligible first sample
		input[length] = 9.0f; // next pass must be excluded
		base::AudioWriteRequest request;
		request.samples = input.data(); request.numSamps = static_cast<unsigned int>(input.size());
		request.stride = 1; request.fadeNew = 1.0f; request.fadeCurrent = 0.0f;
		request.source = base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0);
		loop->EndWrite(request.numSamps, true);
		ASSERT_TRUE(loop->TriggerCaptureComplete());
		loop->Play(constants::MaxLoopFadeSamps, length, true);
		loop->EndRecording();
		const auto exported = loop->ExportSamples();
		ASSERT_EQ(length, exported.size());
		const auto onset = static_cast<unsigned long>((delay + static_cast<int>(length)) % length);
		EXPECT_FLOAT_EQ(seam == 0 ? 0.25f : 1.0f, exported[onset]);
		EXPECT_EQ(exported.end(), std::find(exported.begin(), exported.end(), 9.0f));
		std::vector<float> rendered(length);
		ASSERT_EQ(length, loop->ReadBlock(rendered.data(), 0, length));
		EXPECT_EQ(exported, rendered);
	}
}

TEST(RecordedPlaybackLatency, MonitorCannotCompleteCaptureOrClampUnavailablePlayback)
{
	audio::WireMixBehaviourParams wire; wire.Channels = {0u};
	engine::LoopParams params; params.FadeSamps = 0u;
	auto loop = std::make_shared<engine::Loop>(params, engine::Loop::GetMixerParams({80,80}, wire));
	loop->Record();
	loop->ConfigureTriggerCapture(constants::MaxLoopFadeSamps, true, false);
	loop->SetTriggerCaptureCursor(constants::MaxLoopFadeSamps);
	ASSERT_TRUE(loop->SetTriggerCaptureLength(128u));
	std::array<float, 128> input{};
	base::AudioWriteRequest request;
	request.samples=input.data(); request.numSamps=128u; request.stride=1;
	request.fadeCurrent=0.0f; request.fadeNew=1.0f;
	request.source=base::Audible::AUDIOSOURCE_MONITOR;
	loop->OnBlockWrite(request,0); loop->EndWrite(128u,true);
	EXPECT_FALSE(loop->TriggerCaptureComplete());
	loop->Play(constants::MaxLoopFadeSamps+97u,128u,true);
	EXPECT_EQ(97u,loop->BodyPlayIndex());
	std::array<float, 16> output{};
	EXPECT_EQ(0u,loop->ReadBlock(output.data(),0,16u));
	loop->EndMultiPlay(16u);
	EXPECT_EQ(113u,loop->BodyPlayIndex());
}

TEST(RecordedPlaybackLatency, ProductionCaptureCompletesAtExclusivePhysicalDeadline)
{
	for (const unsigned int sampleRate : {44100u, 48000u, 192000u})
	for (const int sign : {-1, 0, 1})
	for (const int error : {-13, 13})
	{
		SCOPED_TRACE(::testing::Message() << sampleRate << " delay sign=" << sign << " error=" << error);
		const auto reserve = io::UserConfig::TriggerHistorySamples(sampleRate).value();
		const auto delay = io::UserConfig::TriggerPreDelaySamples(sign * 86.0, sampleRate).value();
		const auto guard = (10u * sampleRate + 999u) / 1000u;
		const auto seam = delay > 0 ? (std::min)(reserve, static_cast<unsigned int>(delay) + guard) : 0u;
		const auto age = constants::MaxLoopFadeSamps + reserve;
		const auto length = sampleRate / 2u;
		constexpr std::uint32_t trigger = 1000000u;
		const auto stop = trigger + length + error;
		const auto sourceEnd = trigger + length - seam;
		const auto deadline = sourceEnd + age;
		engine::LoopTakeParams params; params.Size = {100,100}; params.FadeSamps = 0u;
		audio::MergeMixBehaviourParams merge;
		auto take = std::make_shared<engine::LoopTake>(params, engine::LoopTake::GetMixerParams(params.Size, merge));
		take->ConfigureTriggerCapture({trigger, delay, reserve, age, seam});
		take->Record({0u}, "deadline");
		take->CommitChanges();
		ASSERT_EQ(1u, take->GetLoops().size());
		auto loop = take->GetLoops().front();
		auto current = trigger;
		auto advance = [&](std::uint32_t end)
		{
			while (current < end)
			{
				const auto count = (std::min)(128u, end-current);
				take->BeginCaptureAudioBoundary(current, current, current);
				std::array<float,128> input{};
				for (auto i=0u;i<count;++i)
				{
					const auto physical = static_cast<std::int64_t>(current) + i - age;
					if (physical == trigger) input[i]=1.0f;
					if (physical == sourceEnd-1u) input[i]=0.5f;
					if (physical == sourceEnd) input[i]=9.0f;
				}
				base::AudioWriteRequest request;
				request.samples=input.data(); request.numSamps=count; request.stride=1;
				request.fadeCurrent=0.0f; request.fadeNew=1.0f;
				request.source=base::Audible::AUDIOSOURCE_ADC;
				take->OnBlockWriteChannel(0u,request,0);
				take->EndMultiWrite(count,true,base::Audible::AUDIOSOURCE_ADC);
				current+=count;
			}
		};
		advance(stop);
		take->BeginCaptureAudioBoundary(stop,stop,stop);
		take->SetTriggerCaptureStop(stop);
		take->Play(constants::MaxLoopFadeSamps,length,0u,error);
		advance(deadline-1u);
		EXPECT_FALSE(loop->TriggerCaptureComplete());
		const auto early = take->CommitChanges();
		EXPECT_EQ(early.end(),std::find_if(early.begin(),early.end(),[](const auto& job){return job.JobActionType==actions::JobAction::JOB_ENDRECORDING;}));
		advance(deadline);
		EXPECT_TRUE(loop->TriggerCaptureComplete());
		advance(deadline+9u); // late whole-block notification must exclude U and later
		const auto completed = take->CommitChanges();
		const auto completion = std::find_if(completed.begin(),completed.end(),[](const auto& job){return job.JobActionType==actions::JobAction::JOB_ENDRECORDING;});
		ASSERT_NE(completed.end(),completion);
		take->OnAction(*completion);
		const auto exported=loop->ExportSamples();
		ASSERT_EQ(length,exported.size());
		const auto onset=static_cast<unsigned long>((static_cast<std::int64_t>(delay)+length)%length);
		EXPECT_FLOAT_EQ(1.0f,exported[onset]);
		const auto finalPhysical=static_cast<unsigned long>((static_cast<std::int64_t>(length)-seam-1+delay+length)%length);
		EXPECT_FLOAT_EQ(0.5f,exported[finalPhysical]);
		EXPECT_EQ(exported.end(),std::find(exported.begin(),exported.end(),9.0f));
	}
}

TEST(RecordedPlaybackLatency, SignedOverdubAndPunchShareOneDestinationMapping)
{
	for (const int delay : {-97,0,97})
	{
		constexpr unsigned long length=2048u;
		const auto seam=delay>0?delay+10:0;
		const auto begin=static_cast<std::int64_t>(constants::MaxLoopFadeSamps)+delay-seam;
		audio::WireMixBehaviourParams wire; wire.Channels={0u};
		engine::LoopParams params; params.FadeSamps=0u;
		auto loop=std::make_shared<engine::Loop>(params,engine::Loop::GetMixerParams({80,80},wire));
		loop->Overdub(); loop->ConfigureTriggerCapture(begin,false,true);
		loop->SetTriggerCaptureCursor(begin);
		ASSERT_TRUE(loop->SetTriggerCaptureLength(length));
		loop->PunchIn();
		std::vector<float> input(length,0.0f); input[seam]=0.5f;
		base::AudioWriteRequest request;
		request.samples=input.data(); request.numSamps=length; request.stride=1;
		request.fadeCurrent=1.0f; request.fadeNew=1.0f;
		request.source=base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request,0);
		EXPECT_FALSE(loop->TriggerCaptureComplete());
		request.source=base::Audible::AUDIOSOURCE_BOUNCE;
		loop->OnBlockWrite(request,0);
		loop->EndWrite(length,true);
		EXPECT_TRUE(loop->TriggerCaptureComplete());
		loop->PunchOut(); loop->Play(constants::MaxLoopFadeSamps,length,true); loop->EndRecording();
		const auto exported=loop->ExportSamples();
		const auto onset=static_cast<unsigned long>((delay+static_cast<int>(length))%length);
		ASSERT_EQ(length,exported.size());
		EXPECT_FLOAT_EQ(1.0f,exported[onset]);
		EXPECT_EQ(1u,std::count(exported.begin(),exported.end(),1.0f));
	}
}

struct RecordedPredelayCapacityResult
{
	std::vector<float> Exported;
	std::vector<float> Rendered;
};

static RecordedPredelayCapacityResult CaptureWithInjectedHistoryCapacity(unsigned int capacity, double delayMs)
{
	constexpr auto rate=44100u;
	constexpr auto block=128u;
	constexpr auto length=2048u;
	constexpr auto trigger=1024u;
	constexpr auto inputLatency=11u;
	constexpr auto largestTestCapacity=8192u;
	const auto delay=io::UserConfig::TriggerPreDelaySamples(delayMs,rate).value();
	const auto guard=(10u*rate+999u)/1000u;
	const auto seam=delay>0?(std::min)(capacity,static_cast<unsigned int>(delay)+guard):0u;
	const auto age=constants::MaxLoopFadeSamps+capacity;
	const auto playbackBoundary=((trigger+length+constants::MaxLoopFadeSamps+largestTestCapacity+3u*block+block-1u)/block)*block;
	engine::LoopTakeParams params; params.Size={100,100}; params.FadeSamps=0u;
	audio::MergeMixBehaviourParams merge;
	auto take=std::make_shared<engine::LoopTake>(params,engine::LoopTake::GetMixerParams(params.Size,merge));
	audio::ChannelMixerParams mixerParams;
	mixerParams.InputBufferSize=age+2u*block; mixerParams.OutputBufferSize=block;
	mixerParams.NumInputChannels=1u; mixerParams.NumOutputChannels=1u;
	audio::ChannelMixer adc(mixerParams);
	for (auto q=0u;q<playbackBoundary;q+=block)
	{
		if (q==trigger)
		{
			take->ConfigureTriggerCapture({trigger,delay,capacity,age,seam});
			take->Record({0u},"capacity-proof"); take->CommitChanges();
		}
		if (q>=trigger) take->BeginCaptureAudioBoundary(q,q,q);
		if (q==trigger+length)
		{
			take->SetTriggerCaptureStop(q);
			take->Play(constants::MaxLoopFadeSamps,length,0u);
		}
		std::array<float,block> input{};
		const auto ingress=trigger+inputLatency;
		if (ingress>=q && ingress<q+block) input[ingress-q]=1.0f;
		adc.FromAdc(input.data(),1u,block);
		// Test-only capacity injection: Jadc=F+R-I, with no production rig setting.
		adc.InitPlay(age-inputLatency,block);
		adc.Source()->SetSourceType(base::Audible::AUDIOSOURCE_ADC);
		if (q>=trigger) adc.WriteToSink(take,block);
		adc.Source()->EndMultiPlay(block);
		if (q>=trigger)
		{
			take->EndMultiWrite(block,true,base::Audible::AUDIOSOURCE_ADC);
			take->EndMultiPlay(block);
			for (const auto& job:take->CommitChanges()) take->OnAction(job);
		}
	}
	take->BeginCaptureAudioBoundary(playbackBoundary,playbackBoundary,playbackBoundary);
	RecordedPredelayCapacityResult result;
	if (take->GetLoops().empty()) return result;
	const auto loop=take->GetLoops().front();
	result.Exported=loop->ExportSamples();
	for (auto q=0u;q<2u*length;q+=block)
	{
		std::array<float,block> output{};
		loop->ReadBlock(output.data(),0,block);
		result.Rendered.insert(result.Rendered.end(),output.begin(),output.end());
		loop->EndMultiPlay(block);
	}
	return result;
}

TEST(RecordedPlaybackLatency, SharedHistoryCapacityCancelsForZeroAndSignedFractionalPredelay)
{
	constexpr auto length=2048u;
	constexpr auto trigger=1024u;
	constexpr auto block=128u;
	constexpr auto playbackBoundary=((trigger+length+constants::MaxLoopFadeSamps+8192u+3u*block+block-1u)/block)*block;
	for (const double delayMs:{-0.37,0.0,0.37})
	{
		SCOPED_TRACE(delayMs);
		const auto lowerCapacityCapture=CaptureWithInjectedHistoryCapacity(2048u,delayMs);
		const auto higherCapacityCapture=CaptureWithInjectedHistoryCapacity(8192u,delayMs);
		ASSERT_EQ(length,lowerCapacityCapture.Exported.size());
		EXPECT_EQ(lowerCapacityCapture.Exported,higherCapacityCapture.Exported);
		EXPECT_EQ(lowerCapacityCapture.Rendered,higherCapacityCapture.Rendered);
		const auto delay=io::UserConfig::TriggerPreDelaySamples(delayMs,44100u).value();
		const auto stored=static_cast<unsigned int>((delay+static_cast<int>(length))%length);
		EXPECT_FLOAT_EQ(1.0f,lowerCapacityCapture.Exported[stored]);
		const auto onset=(trigger+length-playbackBoundary%length)%length;
		ASSERT_EQ(2u*length,lowerCapacityCapture.Rendered.size());
		EXPECT_FLOAT_EQ(1.0f,lowerCapacityCapture.Rendered[onset]);
		EXPECT_FLOAT_EQ(1.0f,lowerCapacityCapture.Rendered[onset+length]);
		EXPECT_EQ(2u,std::count(lowerCapacityCapture.Rendered.begin(),lowerCapacityCapture.Rendered.end(),1.0f));
	}
}

TEST(RecordedPlaybackLatency, Delly4EarlyOnsetSurvivesGuardOnceInExportAndPlayback)
{
	// 44.1kHz: an onset 4052 samples before T with D=3793 belongs at L-259.
	constexpr unsigned long length=8192u;
	constexpr std::int64_t delay=3793;
	constexpr std::int64_t seam=4234;
	constexpr std::int64_t begin=constants::MaxLoopFadeSamps+delay-seam;
	constexpr unsigned int earlyOnsetFromPassStart=4234u-4052u;
	audio::WireMixBehaviourParams wire; wire.Channels={0u};
	engine::LoopParams params; params.FadeSamps=0u;
	auto loop=std::make_shared<engine::Loop>(params,engine::Loop::GetMixerParams({80,80},wire));
	loop->Record(); loop->ConfigureTriggerCapture(begin,true,false);
	loop->SetTriggerCaptureCursor(begin);
	ASSERT_TRUE(loop->SetTriggerCaptureLength(length));
	std::vector<float> input(length+64u,0.0f);
	input[earlyOnsetFromPassStart]=1.0f;
	input[length]=9.0f; // same next physical pass is outside [T-s,T+L-s)
	base::AudioWriteRequest request;
	request.samples=input.data(); request.numSamps=static_cast<unsigned int>(input.size());
	request.stride=1; request.fadeCurrent=0.0f; request.fadeNew=1.0f;
	request.source=base::Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(request,0); loop->EndWrite(request.numSamps,true);
	ASSERT_TRUE(loop->TriggerCaptureComplete());
	loop->Play(constants::MaxLoopFadeSamps,length,true); loop->EndRecording();
	const auto exported=loop->ExportSamples();
	ASSERT_EQ(length,exported.size());
	EXPECT_FLOAT_EQ(1.0f,exported[length-259u]);
	EXPECT_EQ(1u,std::count(exported.begin(),exported.end(),1.0f));
	EXPECT_EQ(exported.end(),std::find(exported.begin(),exported.end(),9.0f));
	std::vector<float> rendered;
	for (auto offset=0u;offset<length;offset+=128u)
	{
		std::array<float,128> output{};
		ASSERT_EQ(128u,loop->ReadBlock(output.data(),0,128u));
		rendered.insert(rendered.end(),output.begin(),output.end());
		loop->EndMultiPlay(128u);
	}
	EXPECT_EQ(exported,rendered);
}

TEST(RecordedPlaybackLatency, HardwareLatencyAboveHistoryAgeRetainsExactCaptureRuler)
{
    const auto inputLatency = constants::MaxLoopFadeSamps
        + io::UserConfig::TriggerHistorySamples(constants::DefaultSampleRate).value() + 31u;
    VerifyRecordedCaptureOriginWithHardwareRuler(false, 0, 0, inputLatency);
    VerifyRecordedCaptureOriginWithHardwareRuler(true, 0, 0, inputLatency);
}

// Exercise the production additive ADC route, then compare its selected pass to
// temporary rendering, the eventual normalized source, and an independent DSP oracle.
TEST(RecordedPlaybackLatency, TemporaryOriginalPassMatchesRecordedAcrossHandover)
{
    for (const int delay : {-23, 0, 23})
    for (const unsigned int length : {127u, 1031u})
    for (const unsigned int inputLatency : {17u, 512u})
    for (const bool enabled : {false, true})
    {
        SCOPED_TRACE(::testing::Message() << delay << " L=" << length << " I=" << inputLatency << " enabled=" << enabled);
        constexpr unsigned int block = 32, age = 512;
        constexpr std::uint32_t trigger = 0xffffff00u;
        const auto seam = delay > 0 ? 39u : 0u;
        const auto fade = delay == 0 ? 0u : 13u;
        auto hanning = std::make_unique<audio::Hanning>(fade);
        const auto begin = static_cast<std::int64_t>(constants::MaxLoopFadeSamps) + delay - seam;
        const auto signal = [](std::int64_t physical, unsigned int channel) {
            return static_cast<float>((physical * 7 + channel * 113 + 100000) % 251) / 512.0f;
        };
        engine::LoopTakeParams params; params.Size = {100,100}; params.FadeSamps = fade;
        audio::MergeMixBehaviourParams merge;
        auto take = std::make_shared<engine::LoopTake>(params, engine::LoopTake::GetMixerParams(params.Size, merge));
        take->ConfigureTriggerCapture({trigger, delay, age, age, seam});
        take->Record({1u,0u}, "temporary-original"); take->CommitChanges();
        ASSERT_EQ(2u, take->GetLoops().size());
        auto loops = take->GetLoops();
        std::vector<std::shared_ptr<engine::Loop>> references;
        for (auto channel : {1u,0u})
        {
            audio::WireMixBehaviourParams wire; wire.Channels = {0u};
            engine::LoopParams lp; lp.FadeSamps = fade;
            auto reference = std::make_shared<engine::Loop>(lp, engine::Loop::GetMixerParams({80,80}, wire));
            reference->Record(); reference->ConfigureTriggerCapture(begin,true,false);
            reference->SetTriggerCaptureCursor(begin); ASSERT_TRUE(reference->SetTriggerCaptureLength(length));
            std::vector<float> pass(length);
            for (auto i=0u;i<length;++i) pass[i]=signal(static_cast<std::int64_t>(i)-seam,channel);
            base::AudioWriteRequest request; request.samples=pass.data();request.numSamps=length;
            request.stride=1;request.fadeCurrent=1;request.fadeNew=1;request.source=base::Audible::AUDIOSOURCE_ADC;
            reference->OnBlockWrite(request,0);reference->EndWrite(length,true);
            reference->Play(constants::MaxLoopFadeSamps,length,true);reference->EndRecording();
            references.push_back(reference);
        }
        // Compare the loop effect + software delay with an independent FIFO:
        // a handover must neither flush the delay nor process samples twice.
        for (auto i=0u;i<loops.size();++i)
            if (auto loop = loops[i])
            {
                actions::JobAction job; job.JobActionType=actions::JobAction::JOB_LOADVST;
                job.VstPath=L"fake-temporary-delay.dll";job.PreInitPlugin=std::make_shared<RecordedLatencyImpulsePlugin>();
                loop->OnAction(job);loop->CommitChanges();ASSERT_TRUE(loop->ConfigureRecordedPlaybackDelay(11));
            }
        audio::ChannelMixer mixer({age-inputLatency+block,block,2u,1u});
        unsigned int reads=0, audibleBlocks=0;
        std::array<std::array<float,40>,2> expectedDelay{};
        std::array<unsigned int,2> expectedDelayCursor{};
        const auto finish = length-seam+age;
        bool stopped=false, normalized=false;
        for (std::int64_t elapsed=-static_cast<int>(age); elapsed<static_cast<std::int64_t>(finish)+128;)
        {
            unsigned int count=block;
            if (elapsed<length) count=(std::min)(count,static_cast<unsigned int>(length-elapsed));
            if (elapsed<finish) count=(std::min)(count,static_cast<unsigned int>(finish-elapsed));
            const auto device = static_cast<std::uint64_t>(trigger)+elapsed;
            take->BeginCaptureAudioBoundary(device,device,device);
            if (elapsed==length && !stopped)
            {
                take->SetTriggerCaptureStop(trigger+length);take->Play(constants::MaxLoopFadeSamps,length,0,0,false,7);
                stopped=true;
            }
            std::array<float,block*2> input{};
            for(auto i=0u;i<count;++i) for(auto channel=0u;channel<2;++channel)
                input[i*2+channel]=signal(elapsed+i-inputLatency,channel);
            mixer.FromAdc(input.data(),2,count);
            mixer.InitPlay(age-inputLatency,count);mixer.Source()->SetSourceType(base::Audible::AUDIOSOURCE_ADC);
            mixer.WriteToSink(take,count);take->EndMultiWrite(count,true,base::Audible::AUDIOSOURCE_ADC);
            auto history=mixer.ObserveInputHistory(device,count,inputLatency,true);history.ReadCount=&reads;
            audio::DacRenderContext dac{history};
            if (stopped)
            {
                for(auto i=0u;i<loops.size();++i)
                {
                    auto loop=loops[i]; auto reference=references[i];
                    reference->SetBodyPlayIndex(loop->BodyPlayIndex());
                    std::array<float,block> actual{},expected{},causalExpected{};
                    const auto ready=loop->TriggerCaptureReady(loop->BodyPlayIndex(),count);
                    const auto written=loop->ReadBlock(actual.data(),0,count,enabled?&dac:nullptr,trigger,delay,1u-i);
                    if(enabled || ready)
                    {
                        ASSERT_EQ(count,written);
                        ASSERT_EQ(count,reference->ReadBlock(expected.data(),0,count));
                        if(elapsed+count-inputLatency >= static_cast<std::int64_t>(length-seam)) EXPECT_EQ(actual,expected);
                        if(enabled)
                        {
                            const auto start=loop->BodyPlayIndex();
                            const auto original=[&](std::int64_t storage) {
                                if(storage >= constants::MaxLoopFadeSamps && storage >= begin+length) storage-=length;
                                const auto physical=storage-constants::MaxLoopFadeSamps-delay;
                                return physical < elapsed+count-inputLatency ? signal(physical,1u-i) : 0.0f;
                            };
                            for(auto frame=0u;frame<count;++frame)
                            {
                                const auto phase=(start+frame)%length;
                                auto wanted=original(constants::MaxLoopFadeSamps+phase);
                                if(fade && start+frame >= length-fade)
                                {
                                    const auto xfade=start+frame-(length-fade);
                                    wanted=hanning->Mix(original(constants::MaxLoopFadeSamps+xfade-fade),wanted,xfade);
                                }
                                causalExpected[frame]=wanted;
                                EXPECT_FLOAT_EQ(wanted,actual[frame]) << "causal frame=" << frame << " elapsed=" << elapsed;
                            }
                        }
                        if(enabled) ++audibleBlocks;
                    }
                    else EXPECT_EQ(0u,written);
                    if(enabled)
                    {
                        auto actualSink=std::make_shared<RecordedLatencyCaptureBus>(count,i);
                        loop->WriteBlock(actualSink,nullptr,0,count,&dac,trigger,delay,1u-i);
                        // Independent FIFO oracle for the serial 29-sample plugin
                        // and 11-sample playback delay, including partial silence.
                        std::vector<float> processed(count);
                        for(auto frame=0u;frame<count;++frame)
                        {
                            auto& cursor=expectedDelayCursor[i];
                            processed[frame]=expectedDelay[i][cursor];
                            expectedDelay[i][cursor]=causalExpected[frame];
                            cursor=(cursor+1u)%40u;
                        }
                        EXPECT_EQ(actualSink->Sink->Samples,processed);
                    }
                }
                take->EndMultiPlay(count);
            }
            const auto end=elapsed+count;
            for(const auto& loop:loops) EXPECT_EQ(end>=finish,loop->TriggerCaptureComplete());
            if(end==finish)
            {
                take->EndRecording();normalized=true;
                for(auto i=0u;i<loops.size();++i) EXPECT_EQ(loops[i]->ExportSamples(),references[i]->ExportSamples());
            }
            if(normalized)
            {
                const auto previous=reads;
                std::array<float,block> output{};
                for(const auto& loop:loops) EXPECT_EQ(count,loop->ReadBlock(output.data(),0,count,&dac,trigger,delay));
                EXPECT_EQ(previous,reads); // fully ready path never consults history
            }
            elapsed=end;
        }
        if(enabled) {EXPECT_GT(audibleBlocks,0u); if(inputLatency<age && length<age) EXPECT_GT(reads,0u);}
        else EXPECT_EQ(0u,reads);
    }
}
