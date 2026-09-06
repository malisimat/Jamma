#include "gtest/gtest.h"

#include <memory>
#include <string>
#include <vector>

#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "io/JamFile.h"
#include "ninjam/NinjamLoopAlignment.h"
#include "utils/Timer.h"

using audio::MergeMixBehaviourParams;
using base::AudioWriteRequest;
using base::Audible;
using engine::LoopTake;
using engine::LoopTakeParams;
using utils::Timer;

class TimingTestLoopTake : public LoopTake
{
public:
	TimingTestLoopTake(LoopTakeParams params, audio::AudioMixerParams mixerParams) :
		LoopTake(params, mixerParams)
	{
	}

	void SetMidiVisualPosition(unsigned long position, unsigned long length)
	{
		_midiVisualPlayIndex.store(position, std::memory_order_relaxed);
		_midiVisualLoopLength.store(length, std::memory_order_relaxed);
	}

	unsigned long MidiVisualPosition() const
	{
		return _midiVisualPlayIndex.load(std::memory_order_relaxed);
	}

	void InvalidateMidiSceneAnchor()
	{
		_hasMidiSceneAnchor.store(false, std::memory_order_release);
	}
};

static std::shared_ptr<TimingTestLoopTake> MakeTimingTestLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<TimingTestLoopTake>(params, mixerParams);
}

static std::shared_ptr<engine::Loop> MakeTimingLoop(unsigned long loopLength)
{
	audio::WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0u };
	audio::AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	engine::LoopParams loopParams;
	loopParams.Wav = "phase-test";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };
	auto loop = std::make_shared<engine::Loop>(loopParams, mixerParams);

	loop->Record();
	const auto recordedLength = constants::MaxLoopFadeSamps + loopLength;
	std::vector<float> samples(recordedLength, 1.0f);
	AudioWriteRequest request;
	request.samples = samples.data();
	request.numSamps = static_cast<unsigned int>(recordedLength);
	request.stride = 1u;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(request, 0);
	loop->EndWrite(static_cast<unsigned int>(recordedLength), true);
	loop->Play(constants::MaxLoopFadeSamps, loopLength, false);
	return loop;
}

static std::shared_ptr<TimingTestLoopTake> MakePlayingTimingTake(const std::string& id,
	unsigned long loopLength, unsigned long position)
{
	auto take = MakeTimingTestLoopTake(id);
	auto loop = MakeTimingLoop(loopLength);
	loop->ShiftPlayIndex(static_cast<long long>(position));
	take->AddLoop(loop);
	take->CommitChanges();
	take->SetMidiVisualPosition(position % loopLength, loopLength);
	return take;
}

static unsigned long TimingLoopBodyPosition(const engine::Loop& loop)
{
	return loop.PlayIndex() - constants::MaxLoopFadeSamps;
}

static void ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases()
{
	constexpr unsigned long shortLength = 1000ul;
	constexpr unsigned long longLength = shortLength * 3ul;
	constexpr unsigned long shortStart = shortLength / 4ul;
	constexpr unsigned long longStart = (longLength * 3ul) / 8ul;
	constexpr long long masterDelta = 7600;
	constexpr unsigned long recurrenceSamps = 1400ul;

	Timer master;
	master.SetSeedSourceLength(shortLength);
	master.Tick(static_cast<unsigned int>(shortStart), 0u);
	const Timer::Command masterAdjustment{
		Timer::CommandType::PhaseCorrection, 1u, 0ul, 0u,
		Timer::QUANTISE_OFF, masterDelta };

	auto shortTake = MakePlayingTimingTake("independent-short", shortLength, shortStart);
	auto longTake = MakePlayingTimingTake("independent-long", longLength, longStart);
	master.ApplyCommand(masterAdjustment);
	shortTake->ApplyAcceptedTimingCorrection(masterDelta, 1u);
	longTake->ApplyAcceptedTimingCorrection(masterDelta, 1u);

	for (auto sample = 0ul; sample < recurrenceSamps; ++sample)
	{
		master.Tick(1u, 0u);
		shortTake->EndMultiPlay(1u);
		longTake->EndMultiPlay(1u);
	}

	EXPECT_EQ(shortStart, master.SampOffset());
	EXPECT_EQ(shortStart, TimingLoopBodyPosition(*shortTake->GetLoops().front()));
	EXPECT_EQ(shortStart, shortTake->MidiVisualPosition());
	EXPECT_EQ(longStart, TimingLoopBodyPosition(*longTake->GetLoops().front()));
	EXPECT_EQ(longStart, longTake->MidiVisualPosition());
}

TEST(ExternalPhaseCorrection, SharedDeltaPreservesDifferentTakeLengths)
{
	constexpr unsigned long length = 1000ul;
	auto shortTake = MakePlayingTimingTake("short", length, 0ul);
	auto longTake = MakePlayingTimingTake("long", length * 2ul, length);
	auto oddTake = MakePlayingTimingTake("odd", 777ul, 700ul);

	shortTake->QueueTimingCorrection(2, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	longTake->QueueTimingCorrection(2, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	oddTake->QueueTimingCorrection(100, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	shortTake->EndMultiPlay(0u);
	longTake->EndMultiPlay(0u);
	oddTake->EndMultiPlay(0u);

	EXPECT_EQ(2ul, TimingLoopBodyPosition(*shortTake->GetLoops().front()));
	EXPECT_EQ(length + 2ul, TimingLoopBodyPosition(*longTake->GetLoops().front()));
	EXPECT_EQ(23ul, TimingLoopBodyPosition(*oddTake->GetLoops().front()));
}

TEST(ExternalPhaseCorrection, EveryChannelConsumesInSameBlock)
{
	auto take = MakePlayingTimingTake("channels", 1000ul, 100ul);
	auto secondLoop = MakeTimingLoop(1000ul);
	secondLoop->ShiftPlayIndex(100);
	take->AddLoop(secondLoop);
	take->CommitChanges();

	take->QueueTimingCorrection(-150, 2u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(0u);
	ASSERT_EQ(2u, take->GetLoops().size());
	EXPECT_EQ(950ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(950ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
}

TEST(ExternalPhaseCorrection, NegativeDeltaMovesAudioAndMidiWithSameSign)
{
	auto take = MakePlayingTimingTake("negative", 30000ul, 5000ul);
	take->QueueTimingCorrection(-4900, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(0u);

	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(4900, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, QueuedEventsAccumulateAndConsumeExactlyOnce)
{
	auto take = MakePlayingTimingTake("accumulate", 1000ul, 100ul);
	take->QueueTimingCorrection(20, 7u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->QueueTimingCorrection(-5, 7u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(10u);
	EXPECT_EQ(125ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(2u, take->QueuedExternalPhaseCorrectionCount());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());

	take->EndMultiPlay(10u);
	EXPECT_EQ(135ul, TimingLoopBodyPosition(*take->GetLoops().front()));
}

TEST(ExternalPhaseCorrection, QueuedLocalCorrectionSurvivesNinjamEpochReset)
{
	auto take = MakePlayingTimingTake("queued-local-across-ninjam-epoch", 1000ul, 100ul);
	take->QueueTimingCorrection(200, 3u, LoopTake::TimingCorrectionReason::PhaseDiscipline);

	// This reset belongs only to the direct NINJAM desired-state consumer. The
	// separate queued local correction remains pending for normal block consume.
	take->ResetTimingEpoch();
	take->EndMultiPlay(0u);

	EXPECT_EQ(300ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(300ul, take->MidiVisualPosition());
	EXPECT_EQ(-200, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, AppliesOnceAndZeroingAppliesExactInverse)
{
	auto take = MakePlayingTimingTake("transport-offset", 1000ul, 100ul);
	take->SetLocalTransportOffsetSamps(350);
	EXPECT_EQ(450ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(450ul, take->MidiVisualPosition());
	EXPECT_EQ(-350, take->MidiAnchorCorrection());

	take->SetLocalTransportOffsetSamps(350);
	EXPECT_EQ(450ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(450ul, take->MidiVisualPosition());

	take->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(TransportPhaseOffset, PendingOffsetWaitsForPlayableLoop)
{
	auto take = MakeTimingTestLoopTake("transport-offset-pending");
	take->SetLocalTransportOffsetSamps(250);
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->EndMultiPlay(0u);
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(-250, take->MidiAnchorCorrection());

	take->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(TransportPhaseOffset, FractionalTargetsRestoreOriginalCursorWithoutRoundingResidue)
{
	constexpr long long masterLength = 101;
	auto take = MakePlayingTimingTake("transport-offset-fractional", 1000ul, 100ul);
	const auto setFraction = [&](double fraction)
	{
		take->SetLocalTransportOffsetSamps(std::llround(fraction * masterLength));
	};

	setFraction(0.005);
	setFraction(0.010);
	setFraction(0.0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(JamFile, SignedTransportOffsetPreservesM2M3MEntityPhases)
{
	constexpr unsigned long masterLength = 1000ul;
	auto parsed = io::JamFile::FromStream(std::stringstream(
		"{\"name\":\"jam\",\"transportoffsetloopfrac\":-0.25,\"stations\":[]}"));
	ASSERT_TRUE(parsed.has_value());
	const auto targetSamps = std::llround(parsed->TransportOffsetLoopFrac
		* static_cast<double>(masterLength));

	auto take = MakeTimingTestLoopTake("signed-persisted-local-offset");
	for (const auto length : { masterLength, 2ul * masterLength, 3ul * masterLength, 777ul })
	{
		auto loop = MakeTimingLoop(length);
		loop->ShiftPlayIndex(100);
		take->AddLoop(loop);
	}
	take->CommitChanges();
	take->SetMidiVisualPosition(100ul, 2500ul);
	take->SetLocalTransportOffsetSamps(targetSamps);

	ASSERT_EQ(4u, take->GetLoops().size());
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(1850ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
	EXPECT_EQ(2850ul, TimingLoopBodyPosition(*take->GetLoops()[2]));
	EXPECT_EQ(627ul, TimingLoopBodyPosition(*take->GetLoops()[3]));
	EXPECT_EQ(2350ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());

	constexpr auto globalSample = 5000;
	constexpr auto frozenAutomationGlobalSampleOrigin = 2400;
	const auto automationPosition = static_cast<unsigned long>(
		(globalSample - frozenAutomationGlobalSampleOrigin - take->MidiAnchorCorrection()) % 2500);
	EXPECT_EQ(take->MidiVisualPosition(), automationPosition);
}

TEST(TransportPhaseOffset, DirectTimingCommandRebasesMidiOnlyTake)
{
	auto take = MakeTimingTestLoopTake("midi-only");
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->ApplyAcceptedTimingCorrection(-1250, 1u);
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(1250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandMovesAudioAndMidiOnce)
{
	auto take = MakePlayingTimingTake("direct-audio-midi", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(1250, 1u);
	EXPECT_EQ(350ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(-1250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandPreservesGenerationAndZeroDeltaGates)
{
	auto take = MakePlayingTimingTake("direct-generation-gates", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(-250, 0u);
	take->ApplyAcceptedTimingCorrection(0, 1u);
	take->ApplyAcceptedTimingCorrection(-250, 1u);

	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());

	take->ApplyAcceptedTimingCorrection(-250, 2u);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandUsesEachAudioAndMidiModulo)
{
	auto take = MakeTimingTestLoopTake("direct-independent-modulo");
	auto shortLoop = MakeTimingLoop(1000ul);
	auto longLoop = MakeTimingLoop(1500ul);
	shortLoop->ShiftPlayIndex(100);
	longLoop->ShiftPlayIndex(1400);
	take->AddLoop(shortLoop);
	take->AddLoop(longLoop);
	take->CommitChanges();
	take->SetMidiVisualPosition(700ul, 777ul);

	take->ApplyAcceptedTimingCorrection(-250, 1u);

	ASSERT_EQ(2u, take->GetLoops().size());
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(1150ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
	EXPECT_EQ(450ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandHandlesAudioOnlyAndEmptyTakes)
{
	auto audioOnly = MakeTimingTestLoopTake("direct-audio-only");
	auto loop = MakeTimingLoop(1000ul);
	loop->ShiftPlayIndex(100);
	audioOnly->AddLoop(loop);
	audioOnly->CommitChanges();
	audioOnly->ApplyAcceptedTimingCorrection(250, 1u);

	EXPECT_EQ(350ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	EXPECT_EQ(0ul, audioOnly->MidiVisualPosition());
	EXPECT_EQ(0, audioOnly->MidiAnchorCorrection());
	EXPECT_EQ(1u, audioOnly->ConsumedExternalPhaseCorrectionCount());

	auto empty = MakeTimingTestLoopTake("direct-empty");
	empty->ApplyAcceptedTimingCorrection(-250, 1u);
	EXPECT_EQ(0ul, empty->MidiVisualPosition());
	EXPECT_EQ(0, empty->MidiAnchorCorrection());
	EXPECT_EQ(0u, empty->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, LocalOffsetHandlesAudioOnlyMidiOnlyAndEmptyTakes)
{
	auto audioOnly = MakeTimingTestLoopTake("local-offset-audio-only");
	auto loop = MakeTimingLoop(1000ul);
	loop->ShiftPlayIndex(100);
	audioOnly->AddLoop(loop);
	audioOnly->CommitChanges();
	audioOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	EXPECT_EQ(0, audioOnly->MidiAnchorCorrection());
	audioOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	audioOnly->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));

	auto midiOnly = MakeTimingTestLoopTake("local-offset-midi-only");
	midiOnly->SetMidiVisualPosition(700ul, 777ul);
	midiOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(450ul, midiOnly->MidiVisualPosition());
	EXPECT_EQ(250, midiOnly->MidiAnchorCorrection());

	auto empty = MakeTimingTestLoopTake("local-offset-empty");
	empty->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(0ul, empty->MidiVisualPosition());
	EXPECT_EQ(0, empty->MidiAnchorCorrection());
	auto lateLoop = MakeTimingLoop(1000ul);
	lateLoop->ShiftPlayIndex(100);
	empty->AddLoop(lateLoop);
	empty->CommitChanges();
	empty->EndMultiPlay(0u);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*empty->GetLoops().front()));
}

TEST(TransportPhaseOffset, DirectTimingCommandKeepsMidiAutomationWithNoteCursor)
{
	auto take = MakePlayingTimingTake("direct-midi-automation", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(250, 1u);

	constexpr auto globalSample = 1000;
	constexpr auto frozenAutomationGlobalSampleOrigin = 900;
	const auto automationPosition = static_cast<unsigned long>(
		(globalSample - frozenAutomationGlobalSampleOrigin - take->MidiAnchorCorrection()) % 1000);
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(take->MidiVisualPosition(), automationPosition);
}

TEST(TransportPhaseOffset, ContinuousSyncMasterAdjustmentPreservesIndependentAudioAndMidiPhases)
{
	ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases();
}

TEST(TransportPhaseOffset, BlockSyncMasterAdjustmentPreservesIndependentAudioAndMidiPhases)
{
	ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases();
}

TEST(TransportPhaseOffset, SyncPhaseMapRebasesAfterTimingCorrection)
{
	auto take = MakePlayingTimingTake("boundary-restore", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(6100u));
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	take->ApplyAcceptedTimingCorrection(125, 1u);
	// The source ruler moved by the same correction. Rebase it without
	// recapturing this take's anchor.
	map.Rebase(6100u, 1125);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(7200u));
	EXPECT_EQ(225ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(225ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, SyncPhaseMapRestoresBeforeRebasingAfterDeviceRateAdvance)
{
	auto take = MakePlayingTimingTake("boundary-restore-device-advance", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	take->EndMultiPlay(550u);
	EXPECT_EQ(650ul, take->MidiVisualPosition());
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(5550u));
	EXPECT_EQ(600ul, take->MidiVisualPosition());

	take->ApplyAcceptedTimingCorrection(125, 1u);
	map.Rebase(5550u, 625);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(6100u));

	EXPECT_EQ(225ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(225ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, SyncPhaseMapMapsAudioAndMidiFromTheSameRemoteMaster)
{
	auto take = MakePlayingTimingTake("boundary-map", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(125, 1u);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);

	for (const auto scene : { 5000u, 5550u, 6100u, 7200u })
	{
		take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		const auto elapsed = static_cast<std::uint64_t>(scene - 5000u);
		const auto scaled = (elapsed * 1000u + 550u) / 1100u;
		const auto expected = static_cast<unsigned long>((225u + scaled) % 1000u);
		EXPECT_EQ(expected, TimingLoopBodyPosition(*take->GetLoops().front()));
		EXPECT_EQ(expected, take->MidiVisualPosition());
	}
}

TEST(TransportPhaseOffset, SyncPhaseMapRebasePreservesIndependentTakeOrigins)
{
	auto earlyTake = MakePlayingTimingTake("early-sync-origin", 1000ul, 100ul);
	auto lateTake = MakePlayingTimingTake("late-sync-origin", 2000ul, 1700ul);

	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 400);
	earlyTake->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	lateTake->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	for (const auto scene : { 5550u, 6100u, 7200u })
	{
		const auto elapsed = static_cast<std::uint64_t>(scene - 5000u);
		const auto sourceCoordinate = static_cast<std::int64_t>(400u
			+ ((elapsed * 1000u + 550u) / 1100u));
		earlyTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		lateTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 300, 1000ul),
			TimingLoopBodyPosition(*earlyTake->GetLoops().front()));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 700, 2000ul),
			TimingLoopBodyPosition(*lateTake->GetLoops().front()));
	}

	// A remote phase correction changes the common source phase, never either
	// loop's independently recorded origin.
	earlyTake->ApplyAcceptedTimingCorrection(125, 1u);
	lateTake->ApplyAcceptedTimingCorrection(125, 1u);
	map.Rebase(7200u, 2925);

	for (const auto scene : { 7200u, 7750u, 8300u, 9400u })
	{
		const auto elapsed = static_cast<std::uint64_t>(scene - 7200u);
		const auto sourceCoordinate = static_cast<std::int64_t>(2925u
			+ ((elapsed * 1000u + 550u) / 1100u));
		earlyTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		lateTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 300, 1000ul),
			TimingLoopBodyPosition(*earlyTake->GetLoops().front()));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 700, 2000ul),
			TimingLoopBodyPosition(*lateTake->GetLoops().front()));
	}
}

TEST(TransportPhaseOffset, SyncPhaseMapPreservesLateAudioAndMidiOrigins)
{
	auto take = MakePlayingTimingTake("late-sync-map", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	auto loop = take->GetLoops().front();
	loop->SetBodyPlayIndex(700ul);
	loop->InvalidateSceneAnchor();
	take->SetMidiVisualPosition(700ul, 1000ul);
	take->InvalidateMidiSceneAnchor();

	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(5550u));
	EXPECT_EQ(700ul, TimingLoopBodyPosition(*loop));
	EXPECT_EQ(700ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, AbsoluteLocalOffsetIsIndependentOfNinjamGeneration)
{
	auto take = MakeTimingTestLoopTake("midi-only-local-offset");
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->ApplyAcceptedTimingCorrection(0, 7u);
	take->SetLocalTransportOffsetSamps(-1250);
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(1250, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandLeavesEmptyTakeUnmoved)
{
	auto take = MakeTimingTestLoopTake("empty");
	take->ApplyAcceptedTimingCorrection(250, 2u);
	take->ApplyAcceptedTimingCorrection(500, 1u);
	EXPECT_EQ(0ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(ExternalPhaseCorrection, InvalidationLeavesDisconnectedAdvanceUnchanged)
{
	auto take = MakePlayingTimingTake("disconnect", 1000ul, 100ul);
	take->QueueTimingCorrection(200, 3u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->InvalidateTimingCorrections();
	take->EndMultiPlay(10u);
	take->EndMultiPlay(10u);

	EXPECT_EQ(120ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(120ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, ReconnectCannotConsumeStaleGeneration)
{
	auto take = MakePlayingTimingTake("reconnect", 1000ul, 100ul);
	take->QueueTimingCorrection(300, 4u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->InvalidateTimingCorrections();
	take->QueueTimingCorrection(-20, 5u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(10u);

	EXPECT_EQ(90ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(90ul, take->MidiVisualPosition());
	EXPECT_EQ(20, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, LongSimulationRemainsExactAcrossLengths)
{
	const unsigned long lengths[] = { 1000ul, 2000ul, 500ul, 777ul };
	std::shared_ptr<TimingTestLoopTake> takes[std::size(lengths)];
	unsigned long expected[std::size(lengths)]{};
	for (auto index = 0u; index < std::size(lengths); ++index)
		takes[index] = MakePlayingTimingTake("simulation-" + std::to_string(index), lengths[index], 0ul);

	for (auto interval = 0u; interval < 2000u; ++interval)
	{
		const auto delta = static_cast<long long>(interval % 15u) - 7;
		for (auto index = 0u; index < std::size(lengths); ++index)
		{
			takes[index]->QueueTimingCorrection(delta, 9u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
			takes[index]->EndMultiPlay(64u);
			const auto length = static_cast<long long>(lengths[index]);
			auto next = (static_cast<long long>(expected[index]) + 64 + delta) % length;
			if (next < 0)
				next += length;
			expected[index] = static_cast<unsigned long>(next);
			EXPECT_EQ(expected[index], TimingLoopBodyPosition(*takes[index]->GetLoops().front()))
				<< "interval=" << interval << " length=" << lengths[index];
		}
	}
}
