#include "gtest/gtest.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioHost.h"
#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "ninjam/NinjamAudioTimingCommand.h"
#include "ninjam/NinjamTiming.h"
#include "io/UserConfig.h"
#include "utils/Timer.h"

// Phase 6 — deterministic end-to-end integration simulation.
//
// These tests wire the real components of the remote-tempo transport together:
//   * ToDeviceTiming sample-rate conversion (44.1 kHz -> 48 kHz),
//   * NinjamTimingCoordinator observation and command emission,
//   * the single-writer/single-reader NinjamAudioTimingCommandMailbox,
//   * utils::Timer::ApplyCommand transport advancement, and
//   * model local takes that mirror LoopTake's generation-gated phase shift.
//
// The harness models one audio block as: consume at most one command, apply the
// same local copy to the Timer and every take, then advance all consumers. This
// is exactly the fan-out AudioHost performs at the top of _OnAudio, so the tests
// assert the cross-consumer invariants the adversarial review requires.

using ninjam::NinjamAudioTimingCommand;
using ninjam::NinjamAudioTimingCommandMailbox;
using ninjam::LocalTransportOffsetLoopFracMailbox;
using ninjam::NinjamRemoteTiming;
using ninjam::NinjamTiming;
using ninjam::NinjamTimingCommandType;
using ninjam::NinjamTimingCoordinator;
using ninjam::NinjamTimingUpdate;
using ninjam::ToDeviceTiming;
using utils::Timer;

class NinjamProductionBoundaryLoopTake : public engine::LoopTake
{
public:
	NinjamProductionBoundaryLoopTake(engine::LoopTakeParams params,
		audio::AudioMixerParams mixerParams) : LoopTake(params, mixerParams)
	{
	}

	void SetMidiTimingState(unsigned long position, unsigned long length) noexcept
	{
		_midiVisualPlayIndex.store(position, std::memory_order_relaxed);
		_midiVisualLoopLength.store(length, std::memory_order_relaxed);
	}

	unsigned long MidiTimingPosition() const noexcept
	{
		return _midiVisualPlayIndex.load(std::memory_order_relaxed);
	}
};

class NinjamProductionBoundaryFixture
{
public:
	static std::shared_ptr<engine::Loop> MakeLoop(unsigned long loopLength,
		unsigned long position)
	{
		audio::WireMixBehaviourParams mixBehaviour;
		mixBehaviour.Channels = { 0u };
		audio::AudioMixerParams mixerParams;
		mixerParams.Size = { 160, 320 };
		mixerParams.Position = { 6, 6 };
		mixerParams.Behaviour = mixBehaviour;

		engine::LoopParams loopParams;
		loopParams.Wav = "desired-state-phase-test";
		loopParams.Size = { 80, 80 };
		loopParams.Position = { 10, 22 };
		auto loop = std::make_shared<engine::Loop>(loopParams, mixerParams);

		loop->Record();
		const auto recordedLength = constants::MaxLoopFadeSamps + loopLength;
		std::vector<float> samples(recordedLength, 1.0f);
		base::AudioWriteRequest request;
		request.samples = samples.data();
		request.numSamps = static_cast<unsigned int>(recordedLength);
		request.stride = 1u;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0);
		loop->EndWrite(static_cast<unsigned int>(recordedLength), true);
		loop->Play(constants::MaxLoopFadeSamps, loopLength, false);
		loop->ShiftPlayIndex(static_cast<long long>(position));
		return loop;
	}

	static std::shared_ptr<NinjamProductionBoundaryLoopTake> MakeTake(
		const std::string& id, unsigned long loopLength, unsigned long position)
	{
		engine::LoopTakeParams params;
		params.Id = id;
		params.Size = { 100, 100 };
		audio::MergeMixBehaviourParams merge;
		auto mixerParams = engine::LoopTake::GetMixerParams(params.Size, merge);
		auto take = std::make_shared<NinjamProductionBoundaryLoopTake>(params, mixerParams);
		take->AddLoop(MakeLoop(loopLength, position));
		take->CommitChanges();
		take->SetMidiTimingState(position % loopLength, loopLength);
		return take;
	}

	static std::shared_ptr<engine::Station> MakeStation(
		const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>>& takes)
	{
		engine::StationParams params;
		params.Name = "desired-state-station";
		params.Size = { 200, 320 };
		audio::MergeMixBehaviourParams merge;
		auto station = std::make_shared<engine::Station>(params,
			engine::Station::GetMixerParams(params.Size, merge));
		for (const auto& take : takes)
			station->AddTake(take);
		station->CommitChanges();
		return station;
	}

	static unsigned long AudioPosition(const NinjamProductionBoundaryLoopTake& take)
	{
		return take.GetLoops().front()->BodyPlayIndex();
	}

	static ninjam::NinjamDesiredTransportState Desired(std::uint64_t epoch,
		std::uint64_t version, std::uint64_t generation,
		ninjam::NinjamLocalFollowPolicy policy, unsigned long length,
		unsigned int phase, std::uint64_t observationSample = 0u)
	{
		ninjam::NinjamDesiredTransportState desired;
		desired.SessionEpoch = epoch;
		desired.Version = version;
		desired.Generation = generation;
		desired.LocalFollowPolicy = policy;
		desired.HasRemoteTiming = policy != ninjam::NinjamLocalFollowPolicy::NoSync;
		desired.IntervalLengthSamps = length;
		desired.GrainSamps = length == 0ul ? 0u : static_cast<unsigned int>(length / 16ul);
		desired.BeatsPerInterval = 16u;
		desired.TempoBpm = 120.0f;
		desired.Quantisation = Timer::QUANTISE_POWER;
		desired.RemotePhaseSamps = phase;
		desired.HasObservationSample = true;
		desired.ObservationSample = observationSample;
		return desired;
	}
};

namespace
{
	// A model local take. Mirrors LoopTake::ApplyTimingCommand's generation gate
	// and raw play-index shift; playback wrapping is irrelevant to the relative
	// offset invariant, so the position is kept as an unwrapped running total.
	struct ModelTake
	{
		long long Position = 0;
		unsigned int Length = 0u;
		std::uint64_t Generation = 0u;
		bool IsRemote = false;
	};

	class TransportHarness
	{
	public:
		Timer Clock;
		NinjamTimingCoordinator Coordinator;
		NinjamAudioTimingCommandMailbox Mailbox;
		LocalTransportOffsetLoopFracMailbox LocalOffsetMailbox;
		double LocalOffsetLoopFrac = 0.0;
		long long LocalOffsetTargetSamps = 0;
		std::vector<ModelTake> Takes;

		unsigned int PhaseCommandsPublished = 0u;
		unsigned int PhaseCommandsConsumed = 0u;
		unsigned int InvalidatesConsumed = 0u;

		void Connect(bool prompt, bool push)
		{
			ninjam::NinjamTempoJoinOptions options;
			options.PromptBeforeApplyingRemoteTempo = prompt;
			options.PushLocalTempoOnJoin = push;
			Coordinator.Connect(options, std::nullopt);
		}

		// Mirrors Scene::_ApplyNinjamTimingUpdate: translates one coordinator
		// update into a single coherent transport command and publishes it.
		bool PublishUpdate(const NinjamTimingUpdate& update)
		{
			NinjamAudioTimingCommand command;
			bool hasCommand = false;
			if (update.ClockSettings.has_value())
			{
				const auto& settings = update.ClockSettings.value();
				command.Type = NinjamTimingCommandType::ReplaceTiming;
				command.Generation = settings.Generation;
				command.SeedLengthSamps = settings.SeedLengthSamps;
				command.QuantiseSamps = settings.QuantiseSamps;
				command.Quantisation = settings.Quantisation;
				command.AbsolutePhaseSamps = settings.PhaseSamps;
				command.PhaseDeltaSamps = update.PhaseCorrection ? update.PhaseCorrection->DeltaSamps : 0;
				hasCommand = true;
			}
			else if (update.PhaseCorrection.has_value())
			{
				const auto& correction = update.PhaseCorrection.value();
				command.Type = correction.IsJoin ? NinjamTimingCommandType::JoinAlignment
					: NinjamTimingCommandType::PhaseDiscipline;
				command.Generation = correction.Generation;
				command.PhaseDeltaSamps = correction.DeltaSamps;
				++PhaseCommandsPublished;
				hasCommand = true;
			}
			else if (update.InvalidatePendingCorrections)
			{
				command.Type = NinjamTimingCommandType::Invalidate;
				hasCommand = true;
			}
			if (hasCommand)
				Mailbox.Publish(command);
			return hasCommand;
		}

		void Publish(const NinjamAudioTimingCommand& command)
		{
			if (command.Type == NinjamTimingCommandType::JoinAlignment
				|| command.Type == NinjamTimingCommandType::PhaseDiscipline)
				++PhaseCommandsPublished;
			Mailbox.Publish(command);
		}

		void PublishLocalOffset(double loopFrac)
		{
			LocalOffsetMailbox.Publish(loopFrac);
		}

		// Mirrors the top of AudioHost::_OnAudio: consume one command, apply it to
		// the Timer and every take, then advance all consumers by numSamps.
		void AudioBlock(unsigned int numSamps)
		{
			if (const auto command = Mailbox.Consume())
			{
				Timer::Command timerCommand;
				timerCommand.Generation = command->Generation;
				timerCommand.SeedLengthSamps = command->SeedLengthSamps;
				timerCommand.QuantiseSamps = command->QuantiseSamps;
				timerCommand.Quantisation = command->Quantisation;
				bool invalidate = false;
				switch (command->Type)
				{
				case NinjamTimingCommandType::ReplaceTiming:
					timerCommand.Type = Timer::CommandType::ReplaceTiming;
					timerCommand.PhaseDeltaSamps = static_cast<long long>(command->AbsolutePhaseSamps);
					break;
				case NinjamTimingCommandType::Invalidate:
					timerCommand.Type = Timer::CommandType::Invalidate;
					invalidate = true;
					break;
				default:
					timerCommand.Type = Timer::CommandType::PhaseCorrection;
					timerCommand.PhaseDeltaSamps = command->PhaseDeltaSamps;
					break;
				}
				Clock.ApplyCommand(timerCommand);

				for (auto& take : Takes)
					ApplyToTake(take, command->PhaseDeltaSamps, command->Generation, invalidate);

				if (invalidate)
					++InvalidatesConsumed;
				else if (command->Type == NinjamTimingCommandType::JoinAlignment
					|| command->Type == NinjamTimingCommandType::PhaseDiscipline)
				{
					++PhaseCommandsConsumed;
					Coordinator.NotifyPhaseCorrectionConsumed();
				}
			}

			if (const auto localOffsetLoopFrac = LocalOffsetMailbox.ConsumeLatest())
				LocalOffsetLoopFrac = localOffsetLoopFrac.value();
			const auto localOffsetTargetSamps = static_cast<long long>(std::llround(
				LocalOffsetLoopFrac * static_cast<double>(Clock.SeedSourceLength())));
			if (localOffsetTargetSamps != LocalOffsetTargetSamps)
			{
				const auto localOffsetDelta = localOffsetTargetSamps - LocalOffsetTargetSamps;
				LocalOffsetTargetSamps = localOffsetTargetSamps;
				for (auto& take : Takes)
					if (!take.IsRemote) take.Position += localOffsetDelta;
			}

			Clock.Tick(numSamps, 0u);
			for (auto& take : Takes)
				take.Position += numSamps;
		}

		// Pairwise position differences must be invariant while all takes receive
		// the identical signed correction in the same block.
		std::vector<long long> PairwiseDiffs() const
		{
			std::vector<long long> diffs;
			for (std::size_t i = 1u; i < Takes.size(); ++i)
				diffs.push_back(Takes[i].Position - Takes[0].Position);
			return diffs;
		}

	private:
		static void ApplyToTake(ModelTake& take, long long delta,
			std::uint64_t generation, bool invalidate)
		{
			if (invalidate)
			{
				take.Generation = 0u;
				return;
			}
			if (generation == 0u || generation < take.Generation)
				return;
			take.Generation = generation;
			if (delta == 0)
				return;
			take.Position += delta;
		}
	};

	NinjamRemoteTiming MakeRemote(unsigned int lengthSamps, unsigned int positionSamps,
		unsigned int sourceRate, float bpm, unsigned int bpi)
	{
		NinjamRemoteTiming remote;
		remote.IsConnected = true;
		remote.IsValid = true;
		remote.IntervalLengthSamps = lengthSamps;
		remote.IntervalPositionSamps = positionSamps;
		remote.SourceSampleRate = sourceRate;
		remote.Bpm = bpm;
		remote.Bpi = bpi;
		remote.HasAudioBlockStartSample = true;
		remote.AudioBlockStartSample = 0u;
		return remote;
	}

	NinjamTiming MakeTiming48(unsigned int length, unsigned int position,
		std::uint64_t anchor = 0u)
	{
		NinjamTiming timing;
		timing.IsConnected = true;
		timing.IsValid = true;
		timing.DeviceSampleRate = 48000u;
		timing.SourceSampleRate = 48000u;
		timing.IntervalLengthSamps = length;
		timing.IntervalPositionSamps = position;
		timing.HasAudioBlockStartSample = true;
		timing.AudioBlockStartSample = anchor;
		timing.HasLocalTransport = true;
		timing.LocalTransport.MasterLengthSamps = length;
		timing.LocalTransport.MasterPhaseSamps = anchor % length;
		timing.LocalTransport.AbsoluteSamplePos = anchor;
		timing.LocalTransport.SceneSamplePos = anchor;
		timing.LocalBlockStartSample = anchor;
		return timing;
	}
}

TEST(NinjamTimingIntegration, SampleRateConvertedReplacementFansOutToTimerAndTakes)
{
	TransportHarness harness;
	harness.Connect(/*prompt*/ false, /*push*/ false);
	harness.Takes = {
		ModelTake{ 0, 384000u, 0u },
		ModelTake{ 5000, 768000u, 0u },
		ModelTake{ -3000, 192000u, 0u },
		ModelTake{ 12345, 123457u, 0u },
	};
	const auto beforeDiffs = harness.PairwiseDiffs();

	// 352800 source samples at 44.1 kHz is exactly one 8-second bar; scaled to
	// 48 kHz it must round to 384000, matching a native 48 kHz interval.
	const auto remote = MakeRemote(352800u, 100u, 44100u, 120.0f, 16u);
	auto device = ToDeviceTiming(remote, /*connected*/ true, /*deviceRate*/ 48000u,
		/*generation*/ 1u, /*wrap*/ 0ul, /*sequence*/ 1u, /*anchor*/ 0u,
		/*audioBlockStart*/ 0u);
	device.HasLocalTransport = true;
	device.LocalTransport.MasterLengthSamps = 0u;
	device.LocalTransport.MasterPhaseSamps = 0u;
	device.LocalTransport.AbsoluteSamplePos = 0u;
	ASSERT_TRUE(device.IsValid);
	EXPECT_EQ(384000u, device.IntervalLengthSamps);

	const auto update = harness.Coordinator.Observe(device, std::nullopt, false,
		io::UserConfig{}, harness.Clock);
	ASSERT_TRUE(update.ClockSettings.has_value());
	EXPECT_EQ(384000ul, update.ClockSettings->SeedLengthSamps);
	const auto generation = update.ClockSettings->Generation;
	EXPECT_NE(0u, generation);

	ASSERT_TRUE(harness.PublishUpdate(update));
	harness.AudioBlock(512u);

	// The replacement carries no phase delta, so takes only inherit the generation
	// tag; their relative alignment is untouched and the Timer is seeded.
	for (const auto& take : harness.Takes)
		EXPECT_EQ(generation, take.Generation);
	EXPECT_EQ(beforeDiffs, harness.PairwiseDiffs());
	EXPECT_EQ(384000ul, harness.Clock.SeedSourceLength());
}

TEST(NinjamTimingIntegration, DelayedReplacementProjectsAndRebasesAllPhysicalCursors)
{
	constexpr unsigned long oldMasterLength = 1000ul;
	constexpr unsigned int oldMasterPhase = 850u;
	constexpr unsigned int acceptedIntervalLength = 1200u;
	constexpr unsigned int observedRemotePhase = 100u;
	constexpr std::uint64_t observationSample = 10000u;
	constexpr std::uint64_t boundarySample = 11350u;

	NinjamAudioTimingCommandMailbox mailbox;
	NinjamAudioTimingCommand command;
	command.Type = NinjamTimingCommandType::ReplaceTiming;
	command.Generation = 7u;
	command.SeedLengthSamps = acceptedIntervalLength;
	command.AbsolutePhaseSamps = observedRemotePhase;
	command.PhaseObservationSample = observationSample;
	mailbox.Publish(command);

	Timer clock;
	clock.SetSeedSourceLength(oldMasterLength);
	clock.Tick(oldMasterPhase, 0u);
	const auto consumed = mailbox.Consume();
	ASSERT_TRUE(consumed.has_value());
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(oldMasterLength,
		oldMasterPhase, static_cast<unsigned int>(consumed->SeedLengthSamps),
		consumed->AbsolutePhaseSamps, consumed->PhaseObservationSample, boundarySample);

	Timer::Command timerCommand;
	timerCommand.Type = Timer::CommandType::ReplaceTiming;
	timerCommand.Generation = consumed->Generation;
	timerCommand.SeedLengthSamps = consumed->SeedLengthSamps;
	timerCommand.PhaseDeltaSamps = replacement.RemotePhaseSamps;
	ASSERT_TRUE(clock.ApplyCommand(timerCommand));
	EXPECT_EQ(250u, clock.SampOffset());

	auto wrap = [](long long value, unsigned long length)
	{
		value %= static_cast<long long>(length);
		return static_cast<unsigned long>(value < 0 ? value + length : value);
	};
	EXPECT_EQ(500ul, wrap(100 + replacement.LocalDeltaSamps, oldMasterLength));
	EXPECT_EQ(1500ul, wrap(1100 + replacement.LocalDeltaSamps, oldMasterLength * 2ul));
	EXPECT_EQ(500ul, wrap(100 + replacement.LocalDeltaSamps, oldMasterLength));
	EXPECT_EQ(400, replacement.LocalDeltaSamps);
}

TEST(NinjamTimingIntegration, LocalOffsetAndNinjamCorrectionComposeAtOneBoundary)
{
	TransportHarness harness;
	harness.Clock.SetSeedSourceLength(1000ul);
	harness.Clock.Tick(100u, 0u);
	harness.Takes = {
		ModelTake{ 100, 1000u, 7u, false },
		ModelTake{ 400, 1500u, 7u, false },
		ModelTake{ 700, 1000u, 7u, true },
	};

	NinjamAudioTimingCommand correction;
	correction.Type = NinjamTimingCommandType::PhaseDiscipline;
	correction.Generation = 7u;
	correction.PhaseDeltaSamps = 25;
	harness.Publish(correction);
	harness.PublishLocalOffset(0.040);
	harness.PublishLocalOffset(0.010);
	harness.AudioBlock(0u);

	EXPECT_EQ(125u, harness.Clock.SampOffset());
	EXPECT_EQ(135, harness.Takes[0].Position);
	EXPECT_EQ(435, harness.Takes[1].Position);
	EXPECT_EQ(725, harness.Takes[2].Position);
}

TEST(NinjamTimingIntegration, ReplacementPreservesOneActiveLocalOffset)
{
	constexpr unsigned long oldMasterLength = 1000ul;
	constexpr unsigned int oldMasterPhase = 850u;
	constexpr unsigned int remotePhase = 250u;
	constexpr long long localOffset = 250;
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(oldMasterLength,
		oldMasterPhase, 1200u, 100u, 10000u, 11350u);
	ASSERT_EQ(remotePhase, replacement.RemotePhaseSamps);

	const auto wrap = [](long long value, unsigned long length)
	{
		value %= static_cast<long long>(length);
		return value < 0 ? value + length : value;
	};
	const auto localPosition = wrap(static_cast<long long>(oldMasterPhase) + localOffset,
		oldMasterLength);
	const auto rebasedPosition = wrap(localPosition + replacement.LocalDeltaSamps, oldMasterLength);

	EXPECT_EQ(wrap(static_cast<long long>(remotePhase) + localOffset, oldMasterLength), rebasedPosition);
	EXPECT_NE(wrap(static_cast<long long>(remotePhase) + (2 * localOffset), oldMasterLength), rebasedPosition);
}

TEST(NinjamTimingIntegration, RelativeTakeOffsetsInvariantAcrossPhaseCorrections)
{
	TransportHarness harness;
	harness.Takes = {
		ModelTake{ 0, 384000u, 0u },
		ModelTake{ 7000, 768000u, 0u },
		ModelTake{ 19000, 192000u, 0u },
		ModelTake{ 40000, 123457u, 0u },
	};
	const auto invariantDiffs = harness.PairwiseDiffs();

	// Establish a baseline generation and seed with an explicit replacement.
	NinjamAudioTimingCommand replace;
	replace.Type = NinjamTimingCommandType::ReplaceTiming;
	replace.Generation = 5u;
	replace.SeedLengthSamps = 384000ul;
	replace.QuantiseSamps = 24000u;
	replace.Quantisation = Timer::QUANTISE_MULTIPLE;
	replace.AbsolutePhaseSamps = 1000u;
	replace.PhaseDeltaSamps = 0;
	harness.Publish(replace);
	harness.AudioBlock(0u);
	ASSERT_EQ(invariantDiffs, harness.PairwiseDiffs());

	const long long deltas[] = { 50, -30, 120, -200, 15, -75 };
	std::uint64_t generation = 6u;
	for (const auto delta : deltas)
	{
		const auto priorOffset = static_cast<long long>(harness.Clock.SampOffset());
		std::vector<long long> priorPositions;
		for (const auto& take : harness.Takes)
			priorPositions.push_back(take.Position);

		NinjamAudioTimingCommand discipline;
		discipline.Type = NinjamTimingCommandType::PhaseDiscipline;
		discipline.Generation = generation++;
		discipline.PhaseDeltaSamps = delta;
		harness.Publish(discipline);
		harness.AudioBlock(0u);

		// Every take moved by exactly the published delta, and the Timer offset
		// moved by the same amount, so the relative alignment is invariant.
		for (std::size_t i = 0u; i < harness.Takes.size(); ++i)
			EXPECT_EQ(priorPositions[i] + delta, harness.Takes[i].Position);
		EXPECT_EQ(priorOffset + delta, static_cast<long long>(harness.Clock.SampOffset()));
		EXPECT_EQ(invariantDiffs, harness.PairwiseDiffs());
	}
}

TEST(NinjamTimingIntegration, StaleAndZeroGenerationCommandsMoveNothing)
{
	TransportHarness harness;
	harness.Takes = { ModelTake{ 0, 384000u, 0u }, ModelTake{ 9000, 192000u, 0u } };

	NinjamAudioTimingCommand replace;
	replace.Type = NinjamTimingCommandType::ReplaceTiming;
	replace.Generation = 5u;
	replace.SeedLengthSamps = 384000ul;
	replace.AbsolutePhaseSamps = 2000u;
	harness.Publish(replace);
	harness.AudioBlock(0u);

	const auto offsetAfterSeed = harness.Clock.SampOffset();
	std::vector<long long> seeded;
	for (const auto& take : harness.Takes)
		seeded.push_back(take.Position);

	// A correction from an older generation must be ignored by every consumer.
	NinjamAudioTimingCommand stale;
	stale.Type = NinjamTimingCommandType::PhaseDiscipline;
	stale.Generation = 3u;
	stale.PhaseDeltaSamps = 100;
	harness.Publish(stale);
	harness.AudioBlock(0u);
	EXPECT_EQ(offsetAfterSeed, harness.Clock.SampOffset());
	for (std::size_t i = 0u; i < harness.Takes.size(); ++i)
		EXPECT_EQ(seeded[i], harness.Takes[i].Position);

	// A generation-zero command is likewise inert.
	NinjamAudioTimingCommand zero;
	zero.Type = NinjamTimingCommandType::PhaseDiscipline;
	zero.Generation = 0u;
	zero.PhaseDeltaSamps = 100;
	harness.Publish(zero);
	harness.AudioBlock(0u);
	EXPECT_EQ(offsetAfterSeed, harness.Clock.SampOffset());
	for (std::size_t i = 0u; i < harness.Takes.size(); ++i)
		EXPECT_EQ(seeded[i], harness.Takes[i].Position);

	// The next command generation applies to all consumers.
	NinjamAudioTimingCommand current;
	current.Type = NinjamTimingCommandType::PhaseDiscipline;
	current.Generation = 6u;
	current.PhaseDeltaSamps = 100;
	harness.Publish(current);
	harness.AudioBlock(0u);
	EXPECT_EQ(offsetAfterSeed + 100u, harness.Clock.SampOffset());
	for (std::size_t i = 0u; i < harness.Takes.size(); ++i)
		EXPECT_EQ(seeded[i] + 100, harness.Takes[i].Position);
}

TEST(NinjamTimingIntegration, InvalidateStopsCorrectionsUntilReconnectRaisesGeneration)
{
	TransportHarness harness;
	harness.Takes = { ModelTake{ 0, 384000u, 0u }, ModelTake{ 6000, 192000u, 0u } };

	NinjamAudioTimingCommand replace;
	replace.Type = NinjamTimingCommandType::ReplaceTiming;
	replace.Generation = 5u;
	replace.SeedLengthSamps = 384000ul;
	replace.AbsolutePhaseSamps = 0u;
	harness.Publish(replace);
	harness.AudioBlock(0u);

	NinjamAudioTimingCommand correction;
	correction.Type = NinjamTimingCommandType::PhaseDiscipline;
	correction.Generation = 5u;
	correction.PhaseDeltaSamps = 250;
	harness.Publish(correction);
	harness.AudioBlock(0u);
	std::vector<long long> beforeInvalidate;
	for (const auto& take : harness.Takes)
		beforeInvalidate.push_back(take.Position);

	// Disconnect publishes an Invalidate: generations reset but no phase moves.
	NinjamAudioTimingCommand invalidate;
	invalidate.Type = NinjamTimingCommandType::Invalidate;
	harness.Publish(invalidate);
	harness.AudioBlock(0u);
	EXPECT_EQ(1u, harness.InvalidatesConsumed);
	for (std::size_t i = 0u; i < harness.Takes.size(); ++i)
		EXPECT_EQ(beforeInvalidate[i], harness.Takes[i].Position);
	for (const auto& take : harness.Takes)
		EXPECT_EQ(0u, take.Generation);

	// Reconnect at a higher generation re-seeds every consumer.
	NinjamAudioTimingCommand reconnect;
	reconnect.Type = NinjamTimingCommandType::ReplaceTiming;
	reconnect.Generation = 6u;
	reconnect.SeedLengthSamps = 768000ul;
	reconnect.AbsolutePhaseSamps = 0u;
	harness.Publish(reconnect);
	harness.AudioBlock(0u);
	for (const auto& take : harness.Takes)
		EXPECT_EQ(6u, take.Generation);
	EXPECT_EQ(768000ul, harness.Clock.SeedSourceLength());
}

TEST(NinjamTimingIntegration, DiagnosticsReconcileQueuedAndConsumedCorrections)
{
	TransportHarness harness;
	harness.Clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	harness.Clock.SetSeedSourceLength(384000ul);
	harness.Connect(/*prompt*/ false, /*push*/ false);
	harness.Takes = { ModelTake{ 0, 384000u, 0u } };

	unsigned int emittedCorrections = 0u;
	const unsigned int length = 384000u;
	for (unsigned int cycle = 0u; cycle < 12u; ++cycle)
	{
		const auto endAnchor = harness.Clock.AbsoluteSamplePos();
		harness.Coordinator.Observe(MakeTiming48(length, length - (length / 8u), endAnchor),
			std::nullopt, false, io::UserConfig{}, harness.Clock);
		const auto wrapAnchor = harness.Clock.AbsoluteSamplePos();
		const auto wrap = harness.Coordinator.Observe(MakeTiming48(length, 5000u, wrapAnchor),
			std::nullopt, false, io::UserConfig{}, harness.Clock);
		if (wrap.PhaseCorrection.has_value())
			++emittedCorrections;
		harness.PublishUpdate(wrap);
		harness.AudioBlock(512u);
	}

	const auto diagnostics = harness.Coordinator.Diagnostics();
	EXPECT_GT(emittedCorrections, 0u);
	EXPECT_EQ(emittedCorrections, harness.PhaseCommandsPublished);
	EXPECT_EQ(emittedCorrections, harness.PhaseCommandsConsumed);
	EXPECT_EQ(static_cast<std::uint64_t>(emittedCorrections), diagnostics.PhaseEventsQueued);
	EXPECT_EQ(static_cast<std::uint64_t>(emittedCorrections), diagnostics.PhaseEventsConsumed);
	EXPECT_GT(diagnostics.GenerationChanges, 0u);
}

TEST(NinjamTimingIntegration, JobSchedulingDelayDoesNotChangeCorrectionTarget)
{
	// §2.7 acceptance test #8: a remote reading anchored to the block-start sample
	// must yield the same correction regardless of how long the job thread takes
	// to observe it. Two scenarios differ only by clock advancement (simulated
	// job-scheduling latency) between capture and observation.
	const unsigned int length = 384000u;
	const unsigned int captureOffset = 1000u;   // Timer offset when the reading was produced.
	const unsigned int wrapPosition = 1600u;    // Remote interval position at the wrap observation.

	const auto runScenario = [&](unsigned int delayBlocks) -> long long {
		Timer clock;
		clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
		clock.SetSeedSourceLength(length);
		clock.Tick(captureOffset, 0u);

		NinjamTimingCoordinator coordinator;
		ninjam::NinjamTempoJoinOptions options;
		options.PromptBeforeApplyingRemoteTempo = false;
		options.PushLocalTempoOnJoin = false;
		coordinator.Connect(options, std::nullopt);

		// First observation (final quarter) establishes the generation.
		const auto anchor = static_cast<std::uint64_t>(clock.AbsoluteSamplePos(0u));
		coordinator.Observe(MakeTiming48(length, length - (length / 8u), anchor), std::nullopt,
			false, io::UserConfig{}, clock);

		// The anchor is the Timer-domain absolute sample captured when the audio
		// block produced the reading, before any job-scheduling delay.

		// Simulate the job thread being late: the Timer keeps advancing before the
		// wrap observation is processed.
		for (unsigned int i = 0u; i < delayBlocks; ++i)
			clock.Tick(512u, 0u);

		const auto wrap = coordinator.Observe(MakeTiming48(length, wrapPosition, anchor),
			std::nullopt, false, io::UserConfig{}, clock);
		EXPECT_TRUE(wrap.PhaseCorrection.has_value());
		return wrap.PhaseCorrection ? wrap.PhaseCorrection->DeltaSamps : 0;
	};

	const unsigned int delayBlocks = 5u;
	const auto promptDelta = runScenario(0u);
	const auto delayedDelta = runScenario(delayBlocks);

	// Anchoring projects the current offset back to the capture-time offset, so the
	// correction is identical in both scenarios. Without anchoring, the delayed
	// observation would read an offset advanced by delayBlocks * 512 samples and
	// produce a different (wrong) correction.
	EXPECT_EQ(promptDelta, delayedDelta);
	EXPECT_NE(0, promptDelta);
	EXPECT_LE(std::llabs(promptDelta), static_cast<long long>(length / 2u));
}

TEST(NinjamTimingIntegration, TelemetryReconcilesObservationAgeAndEmittedCommands)
{
	// Phase 7: the job-thread telemetry counters must reconcile with the emitted
	// command stream without any per-block logging in the callback.
	TransportHarness harness;
	const unsigned int length = 384000u;
	harness.Clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	harness.Clock.SetSeedSourceLength(length);
	// A nonzero starting offset makes the callback anchor nonzero so the projection
	// records a real observation age.
	harness.Clock.Tick(50000u, 0u);
	harness.Connect(/*prompt*/ false, /*push*/ false);
	harness.Takes = { ModelTake{ 0, length, 0u } };

	// The first observation is a generation change that auto-accepts the remote
	// tempo, so the coordinator emits an Invalidate followed by a Replace.
	const auto firstAnchor = harness.Clock.AbsoluteSamplePos();
	const auto first = harness.Coordinator.Observe(MakeTiming48(
		length, length - (length / 8u), firstAnchor),
		std::nullopt, false, io::UserConfig{}, harness.Clock);
	harness.PublishUpdate(first);

	auto diagnostics = harness.Coordinator.Diagnostics();
	EXPECT_EQ(ninjam::NinjamEmittedCommand::Replace, diagnostics.LastCommandType);
	EXPECT_GE(diagnostics.CommandsEmitted, 2u);
	EXPECT_NE(0u, diagnostics.LastCommandGeneration);
	const auto beforeEmitted = diagnostics.CommandsEmitted;

	// Capture the callback anchor, then simulate job-scheduling delay so the wrap
	// observation carries a measurable age when the coordinator processes it.
	const auto anchor = static_cast<std::uint64_t>(harness.Clock.AbsoluteSamplePos(0u));
	harness.Clock.Tick(1536u, 0u);
	auto projected = ninjam::ProjectTimingToAudioSample(
		MakeTiming48(length, 5000u, anchor), anchor + 1536u);
	projected.LocalTransport.MasterPhaseSamps = harness.Clock.SampOffset();
	projected.LocalTransport.AbsoluteSamplePos = harness.Clock.AbsoluteSamplePos();
	projected.LocalTransport.SceneSamplePos = harness.Clock.SceneSamplePos();
	projected.LocalBlockStartSample = projected.LocalTransport.AbsoluteSamplePos;
	const auto wrap = harness.Coordinator.Observe(projected,
		std::nullopt, false, io::UserConfig{}, harness.Clock);
	harness.PublishUpdate(wrap);

	diagnostics = harness.Coordinator.Diagnostics();
	// The projection records the observation age regardless of whether the resulting
	// correction is accepted.
	EXPECT_GT(diagnostics.MaxObservationAgeSamps, 0u);
	// A correction was emitted for the wrap, advancing the emitted-command sequence.
	EXPECT_GT(diagnostics.CommandsEmitted, beforeEmitted);
	EXPECT_TRUE(diagnostics.LastCommandType == ninjam::NinjamEmittedCommand::Join
		|| diagnostics.LastCommandType == ninjam::NinjamEmittedCommand::Discipline);
}

TEST(NinjamTimingProductionBoundary, CompleteDesiredStateSupersedesFormerCommandSequences)
{
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());

	// The old Invalidate -> Replace -> Discipline history is represented by one
	// self-sufficient latest value. Only the final value reaches the boundary.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 0u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 2u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 1000ul, 200u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 3u, 2u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 1000ul, 350u));
	EXPECT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(1000ul, clock->SeedSourceLength());
	EXPECT_EQ(350u, clock->SampOffset());
	auto receipt = host.LastAppliedTimingCommand();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(1u, receipt->SessionEpoch);
	EXPECT_EQ(3u, receipt->Version);

	// A complete geometry-change value also subsumes a preceding replacement
	// publication and cannot be interpreted against the old 1000-sample geometry.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 4u, 3u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 500u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 5u, 4u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 750u));
	EXPECT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(2000ul, clock->SeedSourceLength());
	EXPECT_EQ(750u, clock->SampOffset());
	receipt = host.LastAppliedTimingCommand();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(5u, receipt->Version);
}

TEST(NinjamTimingProductionBoundary, OverlappingIntentsPublishOneCoherentDesiredVersion)
{
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());

	for (std::uint64_t version = 1u; version <= 64u; ++version)
	{
		const auto length = static_cast<unsigned long>(1000u + version);
		const auto phase = static_cast<unsigned int>(version * 7u);
		host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(9u, version,
			version, ninjam::NinjamLocalFollowPolicy::ContinuousSync, length, phase));
	}

	EXPECT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	const auto receipt = host.LastAppliedTimingCommand();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(9u, receipt->SessionEpoch);
	EXPECT_EQ(64u, receipt->Version);
	EXPECT_EQ(64u, receipt->Generation);
	EXPECT_EQ(1064ul, clock->SeedSourceLength());
	EXPECT_EQ(448u, clock->SampOffset());
	EXPECT_FALSE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(64u, host.LastAppliedTimingCommand()->Version);
}

TEST(NinjamTimingProductionBoundary, ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne)
{
	constexpr unsigned long masterLength = 1000ul;
	auto takeM = NinjamProductionBoundaryFixture::MakeTake("epoch-m", masterLength, 100ul);
	auto take2M = NinjamProductionBoundaryFixture::MakeTake("epoch-2m", masterLength * 2ul, 1300ul);
	auto take3M = NinjamProductionBoundaryFixture::MakeTake("epoch-3m", masterLength * 3ul, 2600ul);
	const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes{
		takeM, take2M, take3M };
	auto station = NinjamProductionBoundaryFixture::MakeStation(takes);

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(masterLength);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));

	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 8u, 8u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 100u));
	ASSERT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	const auto beforeNoSyncAudio = std::vector<unsigned long>{
		NinjamProductionBoundaryFixture::AudioPosition(*takeM),
		NinjamProductionBoundaryFixture::AudioPosition(*take2M),
		NinjamProductionBoundaryFixture::AudioPosition(*take3M) };
	const auto beforeNoSyncMidi = std::vector<unsigned long>{ takeM->MidiTimingPosition(),
		take2M->MidiTimingPosition(), take3M->MidiTimingPosition() };
	const auto beforeNoSyncAutomation = std::vector<long long>{ takeM->MidiAnchorCorrection(),
		take2M->MidiAnchorCorrection(), take3M->MidiAnchorCorrection() };

	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 9u, 0u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	ASSERT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	for (std::size_t i = 0u; i < takes.size(); ++i)
	{
		EXPECT_EQ(beforeNoSyncAudio[i], NinjamProductionBoundaryFixture::AudioPosition(*takes[i]));
		EXPECT_EQ(beforeNoSyncMidi[i], takes[i]->MidiTimingPosition());
		EXPECT_EQ(beforeNoSyncAutomation[i], takes[i]->MidiAnchorCorrection());
	}

	// Session 2 deliberately restarts version/generation at one. Epoch authority
	// makes it newer, resets every gate, and applies one shared +250 correction.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(2u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 350u));
	ASSERT_TRUE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(350u, clock->SampOffset());
	for (std::size_t i = 0u; i < takes.size(); ++i)
	{
		EXPECT_EQ((beforeNoSyncAudio[i] + 250ul) % (masterLength * (i + 1u)),
			NinjamProductionBoundaryFixture::AudioPosition(*takes[i]));
		EXPECT_EQ((beforeNoSyncMidi[i] + 250ul) % (masterLength * (i + 1u)),
			takes[i]->MidiTimingPosition());
		EXPECT_EQ(beforeNoSyncAutomation[i] - 250, takes[i]->MidiAnchorCorrection());
	}

	const auto acceptedAudio = NinjamProductionBoundaryFixture::AudioPosition(*take2M);
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 99u, 99u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 800u));
	EXPECT_FALSE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(acceptedAudio, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(2u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 900u));
	EXPECT_FALSE(host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u));
	EXPECT_EQ(acceptedAudio, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
}

