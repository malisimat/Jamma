#include <vector>

#include "gtest/gtest.h"
#include "engine/MidiEvent.h"
#include "engine/LoopTake.h"
#include "engine/MidiLoop.h"
#include "engine/MidiModel.h"
#include "engine/Timer.h"

using engine::IMidiSink;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::MidiEvent;
using engine::MidiLoop;
using engine::MidiLoopState;
using engine::MidiModel;
using engine::MidiModelParams;
using engine::Timer;
using audio::MergeMixBehaviourParams;
using base::Audible;

namespace
{
	class CapturingSink : public IMidiSink
	{
	public:
		std::vector<MidiEvent> events;
		void OnEvent(const MidiEvent& ev) noexcept override { events.push_back(ev); }
		void Clear() noexcept { events.clear(); }
	};

	std::shared_ptr<LoopTake> MakeLoopTake(const std::string& id = "take-0")
	{
		LoopTakeParams params;
		params.Id = id;
		params.Size = { 100, 100 };
		MergeMixBehaviourParams merge;
		auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
		return std::make_shared<LoopTake>(params, mixerParams);
	}
}

TEST(MidiLoop, DefaultStateIsEmpty) {
	MidiLoop loop;
	ASSERT_EQ(MidiLoopState::Empty, loop.State());
	ASSERT_EQ(0u, loop.EventCount());
	ASSERT_EQ(0u, loop.LoopLengthSamps());
}

TEST(MidiLoop, RecordEventRejectedWhenNotRecording) {
	MidiLoop loop;
	ASSERT_FALSE(loop.RecordEvent(MidiEvent::MakeNoteOn(0u, 0, 60, 100)));
	ASSERT_EQ(0u, loop.EventCount());
}

TEST(MidiLoop, StartRecordTransitionsAndAcceptsEvents) {
	MidiLoop loop;
	loop.StartRecord();
	ASSERT_EQ(MidiLoopState::Recording, loop.State());

	ASSERT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100)));
	ASSERT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOff(500u, 0, 60)));
	ASSERT_EQ(2u, loop.EventCount());
}

TEST(MidiLoop, DefaultCapacityDropsNewestEventsWhenFull) {
	MidiLoop loop;
	loop.StartRecord();

	const auto capacity = MidiLoop::Capacity();
	for (std::size_t i = 0; i < capacity; ++i)
	{
		ASSERT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOn(static_cast<std::uint32_t>(i), 0u, static_cast<std::uint8_t>(i % 128u), 100u)));
	}

	ASSERT_EQ(capacity, loop.EventCount());
	ASSERT_EQ(0u, loop.DroppedEventCount());

	ASSERT_FALSE(loop.RecordEvent(MidiEvent::MakeNoteOn(static_cast<std::uint32_t>(capacity), 0u, 64u, 100u)));
	ASSERT_FALSE(loop.RecordEvent(MidiEvent::MakeNoteOff(static_cast<std::uint32_t>(capacity + 1u), 0u, 64u)));

	ASSERT_EQ(capacity, loop.EventCount());
	ASSERT_EQ(2u, loop.DroppedEventCount());

	loop.EndRecord(static_cast<std::uint32_t>(capacity + 2u));

	CapturingSink sink;
	loop.ReadBlock(0u, static_cast<std::uint32_t>(capacity + 2u), sink);
	ASSERT_EQ(capacity, sink.events.size());
	ASSERT_EQ(0u, sink.events.front().sampleOffset);
	ASSERT_EQ(static_cast<std::uint32_t>(capacity - 1u), sink.events.back().sampleOffset);
}

TEST(MidiLoop, EmptyLoopProducesNoEvents) {
	MidiLoop loop;
	loop.StartRecord();
	loop.EndRecord(1000u);

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_TRUE(sink.events.empty());
}

TEST(MidiLoop, PlaysBackEventsAtCorrectGlobalSamples) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(900u, 0, 60));
	loop.EndRecord(1000u);

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	ASSERT_EQ(100u, sink.events[0].sampleOffset);
	ASSERT_TRUE(sink.events[0].IsNoteOn());
	ASSERT_EQ(900u, sink.events[1].sampleOffset);
	ASSERT_TRUE(sink.events[1].IsNoteOff());
}

TEST(MidiLoop, EventOnBlockBoundaryEmittedExactlyOnce) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(256u, 0, 60, 100));
	loop.EndRecord(1024u);

	CapturingSink sink;
	// First block ends at 256 exclusive — event at 256 must NOT fire here.
	loop.ReadBlock(0u, 256u, sink);
	ASSERT_TRUE(sink.events.empty());

	// Next block [256, 512) — event at 256 fires exactly once.
	loop.ReadBlock(256u, 256u, sink);
	ASSERT_EQ(1u, sink.events.size());
	ASSERT_EQ(256u, sink.events[0].sampleOffset);
}

TEST(MidiLoop, EventsSplitAcrossMultipleSmallBlocks) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(50u,  0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOn(150u, 0, 62, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOn(250u, 0, 64, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(50u + 300u,  0, 60));
	loop.RecordEvent(MidiEvent::MakeNoteOff(150u + 300u, 0, 62));
	loop.RecordEvent(MidiEvent::MakeNoteOff(250u + 300u, 0, 64));
	loop.EndRecord(1000u);

	CapturingSink sink;
	for (std::uint32_t s = 0; s < 1000u; s += 64u)
		loop.ReadBlock(s, 64u, sink);

	ASSERT_EQ(6u, sink.events.size());
	// Verify global sample ordering and exact values for the first pass.
	ASSERT_EQ(50u,  sink.events[0].sampleOffset);
	ASSERT_EQ(150u, sink.events[1].sampleOffset);
	ASSERT_EQ(250u, sink.events[2].sampleOffset);
	ASSERT_EQ(350u, sink.events[3].sampleOffset);
	ASSERT_EQ(450u, sink.events[4].sampleOffset);
	ASSERT_EQ(550u, sink.events[5].sampleOffset);
}

TEST(MidiLoop, LoopWrapRebasesGlobalTimestamps) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(200u, 0, 60));
	loop.EndRecord(1000u);

	CapturingSink sink;
	// One block that wraps the loop boundary at sample 1000.
	loop.ReadBlock(900u, 400u, sink);
	// Expect: nothing in 900..999 (no events in that span), then events at 1100, 1200.
	ASSERT_EQ(2u, sink.events.size());
	ASSERT_EQ(1100u, sink.events[0].sampleOffset);
	ASSERT_TRUE(sink.events[0].IsNoteOn());
	ASSERT_EQ(1200u, sink.events[1].sampleOffset);
	ASSERT_TRUE(sink.events[1].IsNoteOff());
}

TEST(MidiLoop, LoopWrapEmitsForcedNoteOffForHeldNote) {
	MidiLoop loop;
	loop.StartRecord();
	// NoteOn with no matching NoteOff before the loop end.
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.EndRecord(1000u);

	CapturingSink sink;
	// Block that spans 900..1100 — crosses the loop wrap.
	loop.ReadBlock(900u, 200u, sink);

	// Expect three events:
	//   1) NoteOn at 1000+100 ... wait, first pass plays 100 (in 900..1000 we hit the
	//      already-recorded NoteOn? No — NoteOn at loopOffset 100 is in [0,100) of pass 1,
	//      and in [900,1100) the loopOffsets visited are [900..999] ∪ [0..99]. NoteOn at
	//      100 is NOT in those ranges, so first hit is on next pass.
	//
	// Actually with the block [900,1100): segment1 covers loopOffsets [900,1000),
	// segment2 covers loopOffsets [0,100). NoteOn at 100 is excluded (half-open).
	// So no events are recorded, no held notes, no forced NoteOff.
	ASSERT_TRUE(sink.events.empty());

	// Now play a block that captures the NoteOn then wraps.
	sink.Clear();
	loop.ReadBlock(100u, 1000u, sink); // covers loopOffsets [100, 1000) then wraps to [0,100)
	// First pass: NoteOn at globalSample 100 (loopOffset 100, globalBase 0).
	// Wrap: held NoteOff for note 60 at globalSample 1000.
	// Second pass segment [0,100): no NoteOn at offset 0..99.
	ASSERT_EQ(2u, sink.events.size());
	ASSERT_TRUE(sink.events[0].IsNoteOn());
	ASSERT_EQ(100u, sink.events[0].sampleOffset);
	ASSERT_TRUE(sink.events[1].IsNoteOff());
	ASSERT_EQ(60u, sink.events[1].data1);
	ASSERT_EQ(1000u, sink.events[1].sampleOffset);
}

// Regression test: when a ReadBlock call ends *exactly* at the loop boundary
// (remaining == roomInLoop), the within-block wraps condition is false so no
// flush fires. The fix flushes at the start of the next iteration when
// loopOffset == 0. Without the fix, held notes are replayed without a NoteOff.
TEST(MidiLoop, HeldNoteIsFlushedWhenBlockEndsExactlyAtLoopBoundary) {
	MidiLoop loop;
	loop.StartRecord();
	// NoteOn near the end of the loop — no matching NoteOff before loop boundary.
	loop.RecordEvent(MidiEvent::MakeNoteOn(800u, 0, 60, 100));
	loop.EndRecord(1000u);

	CapturingSink sink;
	// First block exactly fills one loop iteration. NoteOn is emitted; block ends
	// at the loop boundary so no within-block wrap flush fires.
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(1u, sink.events.size());
	ASSERT_TRUE(sink.events[0].IsNoteOn());
	ASSERT_EQ(800u, sink.events[0].sampleOffset);

	// Second block starts exactly at the loop boundary. The held NoteOn from the
	// first iteration must be flushed (NoteOff sent) before the new NoteOn fires.
	sink.Clear();
	loop.ReadBlock(1000u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	ASSERT_TRUE(sink.events[0].IsNoteOff());        // forced flush at loop start
	ASSERT_EQ(1000u, sink.events[0].sampleOffset);  // at the very start of this block
	ASSERT_EQ(60u,   sink.events[0].data1);
	ASSERT_TRUE(sink.events[1].IsNoteOn());          // new iteration's NoteOn
	ASSERT_EQ(1800u, sink.events[1].sampleOffset);   // 1000 + 800
}

TEST(MidiLoop, ResetReturnsToEmpty) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(0u, 0, 60, 100));
	loop.EndRecord(500u);
	ASSERT_EQ(MidiLoopState::Playing, loop.State());

	loop.Reset();
	ASSERT_EQ(MidiLoopState::Empty, loop.State());
	ASSERT_EQ(0u, loop.EventCount());
	ASSERT_EQ(0u, loop.LoopLengthSamps());
}

TEST(MidiLoop, EventsBeyondLoopLengthAreNotPlayed) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u,  0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOn(1500u, 0, 64, 100)); // beyond loop length
	loop.EndRecord(1000u);

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(1u, sink.events.size());
	ASSERT_EQ(60u, sink.events[0].data1);
}

TEST(MidiLoop, AttachedModelUpdatesFromRecordedNoteSpans) {
	MidiLoop loop;
	auto model = std::make_shared<MidiModel>(MidiModelParams());
	loop.AttachModel(model);

	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(500u, 0, 60));

	ASSERT_TRUE(loop.UpdateModelFromEvents(1000u, true));
	EXPECT_EQ(1u, model->NoteInstanceCount());
	EXPECT_FALSE(loop.UpdateModelFromEvents(1000u, false));
}

TEST(MidiLoop, AttachedModelClampsRecordingNoteToDisplayLength) {
	MidiLoop loop;
	auto model = std::make_shared<MidiModel>(MidiModelParams());
	loop.AttachModel(model);

	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(250u, 0, 64, 90));

	ASSERT_TRUE(loop.UpdateModelFromEvents(1000u, true));
	EXPECT_EQ(1u, model->NoteInstanceCount());
}

TEST(LoopTakeMidiVisualization, RecordCreatesMidiModelChild)
{
	auto take = MakeLoopTake();

	take->Record({}, "station", { 3u });

	auto child = take->TryGetChild(1u);
	auto midiModel = std::dynamic_pointer_cast<MidiModel>(child);
	ASSERT_NE(nullptr, midiModel);
	EXPECT_EQ(0u, midiModel->NoteInstanceCount());
}

TEST(LoopTakeMidiVisualization, PlayFinalizesMidiModelSpans)
{
	auto take = MakeLoopTake();
	take->Record({}, "station", { 3u });
	auto midiModel = std::dynamic_pointer_cast<MidiModel>(take->TryGetChild(1u));
	ASSERT_NE(nullptr, midiModel);

	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3, 60, 100), 0u));
	take->EndMultiWrite(480u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(0u, 3, 60), 0u));

	take->Play(0ul, 960ul, 0u);

	EXPECT_EQ(1u, midiModel->NoteInstanceCount());
}

TEST(LoopTakeMidiVisualization, RecordMatchesConfiguredMidiDevices)
{
	auto take = MakeLoopTake();
	take->Record({}, "station", { 3u }, { "Keys A", "Keys B" });

	auto firstMidiModel = std::dynamic_pointer_cast<MidiModel>(take->TryGetChild(1u));
	auto secondMidiModel = std::dynamic_pointer_cast<MidiModel>(take->TryGetChild(2u));
	ASSERT_NE(nullptr, firstMidiModel);
	ASSERT_NE(nullptr, secondMidiModel);

	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3, 60, 100), "Keys A", 0u));
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3, 61, 100), "Keys B", 0u));
	EXPECT_FALSE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3, 62, 100), "Keys C", 0u));
	EXPECT_FALSE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 2, 63, 100), "Keys A", 0u));
}

TEST(LoopTakeMidiTiming, ResolveMidiRecordSampleCompensatesQueueDelay)
{
	EXPECT_EQ(200u, LoopTake::ResolveMidiRecordSample(1200u, 1600u, 600u));
	EXPECT_EQ(0u, LoopTake::ResolveMidiRecordSample(700u, 1600u, 600u));
	EXPECT_EQ(600u, LoopTake::ResolveMidiRecordSample(1650u, 1600u, 600u));
}

// ── Slice 5: Quantised record-end ─────────────────────────────────────────────
//
// MidiLoop has no Timer dependency in its hot path, but record-end length
// snapping is driven by the surrounding orchestration. These tests pin down the
// expected behavior: callers compute the snapped length via Timer::QuantiseLength
// and pass that to EndRecord(); MidiLoop then plays back aligned to the snapped
// length, mirroring audio Loop semantics.

TEST(MidiLoop, EndRecordUsesQuantisedLengthFromTimer) {
    Timer t;
    t.SetQuantisation(1024u, Timer::QUANTISE_MULTIPLE);

    // Caller measured a "raw" record length that doesn't sit on the quantise grid.
    const unsigned long rawLength = 3100ul;
    const auto [snapped, drift] = t.QuantiseLength(rawLength);
    // 3100 closer to 3*1024=3072 than 4*1024=4096
    ASSERT_EQ(3072ul, snapped);
    ASSERT_EQ(28, drift);

    MidiLoop loop;
    loop.StartRecord();
    loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
    loop.RecordEvent(MidiEvent::MakeNoteOff(2000u, 0, 60));
    loop.EndRecord(static_cast<std::uint32_t>(snapped));

    ASSERT_EQ(3072u, loop.LoopLengthSamps());

    // Playback wraps at the snapped boundary, not at the raw record end.
    CapturingSink sink;
    loop.ReadBlock(0u, 3072u + 200u, sink);
    // Events: NoteOn@100, NoteOff@2000 (first pass), NoteOn@(3072+100)=3172 (second pass start).
    ASSERT_GE(sink.events.size(), 3u);
    ASSERT_EQ(100u, sink.events[0].sampleOffset);
    ASSERT_EQ(2000u, sink.events[1].sampleOffset);
    ASSERT_EQ(3172u, sink.events[2].sampleOffset);
}

TEST(MidiLoop, EndRecordWithPowerQuantisationSnapsToPowerOfTwo) {
    Timer t;
    t.SetQuantisation(1024u, Timer::QUANTISE_POWER);

    // Raw length sits between 2*1024 and 4*1024 — closer to 2*1024.
    const unsigned long rawLength = 2500ul;
    const auto [snapped, drift] = t.QuantiseLength(rawLength);
    ASSERT_EQ(2048ul, snapped);
    (void)drift;

    MidiLoop loop;
    loop.StartRecord();
    loop.RecordEvent(MidiEvent::MakeNoteOn(0u, 0, 60, 100));
    loop.RecordEvent(MidiEvent::MakeNoteOff(1024u, 0, 60));
    loop.EndRecord(static_cast<std::uint32_t>(snapped));

    ASSERT_EQ(2048u, loop.LoopLengthSamps());

    CapturingSink sink;
    loop.ReadBlock(0u, 2048u, sink);
    ASSERT_EQ(2u, sink.events.size());
    ASSERT_EQ(0u, sink.events[0].sampleOffset);
    ASSERT_EQ(1024u, sink.events[1].sampleOffset);
}
