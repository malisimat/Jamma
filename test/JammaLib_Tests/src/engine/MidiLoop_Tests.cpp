#include <vector>

#include "gtest/gtest.h"
#include "engine/MidiEvent.h"
#include "engine/LoopTake.h"
#include "engine/MidiLoop.h"
#include "engine/MidiModel.h"
#include "engine/MidiQuantisation.h"
#include "engine/Scene.h"
#include "engine/Station.h"
#include "engine/Timer.h"
#include "io/UserConfig.h"

using engine::IMidiSink;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::MidiEvent;
using engine::MidiLoop;
using engine::MidiLoopState;
using engine::MidiModel;
using engine::MidiModelParams;
using engine::MidiQuantisationFraction;
using engine::MidiQuantisationSettings;
using engine::Scene;
using engine::SceneParams;
using engine::Station;
using engine::StationParams;
using engine::Timer;
using actions::TouchAction;
using actions::TouchMoveAction;
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

	std::shared_ptr<Station> MakeStation(const std::string& name = "station")
	{
		StationParams params;
		params.Name = name;
		params.Size = { 100, 100 };
		MergeMixBehaviourParams merge;
		auto mixerParams = Station::GetMixerParams(params.Size, merge);
		return std::make_shared<Station>(params, mixerParams);
	}

	class TestScene : public Scene
	{
	public:
		TestScene(SceneParams params,
			io::UserConfig user) :
			Scene(params, user)
		{
		}

		void AddStationForTest(const std::shared_ptr<Station>& station)
		{
			_AddStation(station);
		}

		void SetSelectDepthForTest(base::SelectDepth depth)
		{
			_UpdateSelectDepth(static_cast<unsigned int>(depth));
		}
	};

	std::vector<unsigned char> HoverPathFor(const std::shared_ptr<base::GuiElement>& element)
	{
		std::vector<unsigned char> hoverPath;
		for (auto idPart : element->GlobalId())
			hoverPath.push_back(static_cast<unsigned char>(idPart + 1u));
		hoverPath.push_back(0u);
		return hoverPath;
	}

	void ApplyCtrlShiftDrag(TestScene& scene,
		const utils::Position2d& start,
		const utils::Position2d& finish)
	{
		auto ctrlShift = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);

		TouchAction down;
		down.State = TouchAction::TOUCH_DOWN;
		down.Index = 0;
		down.Position = start;
		down.Modifiers = ctrlShift;
		EXPECT_TRUE(scene.OnAction(down).IsEaten);

		TouchMoveAction move;
		move.Index = 0;
		move.Position = finish;
		move.Modifiers = ctrlShift;
		EXPECT_TRUE(scene.OnAction(move).IsEaten);

		TouchAction up;
		up.State = TouchAction::TOUCH_UP;
		up.Index = 0;
		up.Position = finish;
		up.Modifiers = ctrlShift;
		EXPECT_FALSE(scene.OnAction(up).IsEaten);
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

// ── Slice 6: Non-destructive per-LoopTake MIDI quantisation ───────────────────

TEST(MidiLoopQuantisation, EnabledShiftsEmittedEventsToSnapGrid) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(160u, 0u, 60u, 100u));  // nearest 100-multiple = 200
	loop.RecordEvent(MidiEvent::MakeNoteOff(560u, 0u, 60u));       // shifted +40 -> 600
	loop.EndRecord(1000u);

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;
	loop.SetQuantisation(settings);
	ASSERT_TRUE(loop.IsQuantisationActive());

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(200u, sink.events[0].sampleOffset);
	EXPECT_EQ(600u, sink.events[1].sampleOffset);
}

TEST(MidiLoopQuantisation, DisabledRestoresOriginalTiming) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(140u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(540u, 0u, 60u));
	loop.EndRecord(1000u);

	MidiQuantisationSettings on;
	on.Enabled = true;
	on.Fraction = MidiQuantisationFraction::Whole;
	on.GrainSamps = 100u;
	loop.SetQuantisation(on);

	MidiQuantisationSettings off;
	loop.SetQuantisation(off);
	EXPECT_FALSE(loop.IsQuantisationActive());

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(140u, sink.events[0].sampleOffset);
	EXPECT_EQ(540u, sink.events[1].sampleOffset);
}

TEST(MidiLoopQuantisation, FractionAdjustmentRetargetsSnapGrid) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(70u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(170u, 0u, 60u));
	loop.EndRecord(1000u);

	MidiQuantisationSettings s;
	s.Enabled = true;
	s.GrainSamps = 200u;
	s.Fraction = MidiQuantisationFraction::Whole; // step 200, 70 -> 0
	loop.SetQuantisation(s);

	CapturingSink wholeSink;
	loop.ReadBlock(0u, 1000u, wholeSink);
	ASSERT_EQ(2u, wholeSink.events.size());
	EXPECT_EQ(0u, wholeSink.events[0].sampleOffset);
	EXPECT_EQ(100u, wholeSink.events[1].sampleOffset);

	s.Fraction = MidiQuantisationFraction::Half; // step 100, 70 -> 100
	loop.SetQuantisation(s);

	CapturingSink halfSink;
	loop.ReadBlock(0u, 1000u, halfSink);
	ASSERT_EQ(2u, halfSink.events.size());
	EXPECT_EQ(100u, halfSink.events[0].sampleOffset);
	EXPECT_EQ(200u, halfSink.events[1].sampleOffset);
}

TEST(MidiLoopQuantisation, ClampsShiftedNoteOffAtLoopBoundary) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(550u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(990u, 0u, 60u));
	loop.EndRecord(1000u);

	MidiQuantisationSettings s;
	s.Enabled = true;
	s.Fraction = MidiQuantisationFraction::Whole;
	s.GrainSamps = 100u;
	loop.SetQuantisation(s);

	CapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(600u, sink.events[0].sampleOffset);
	EXPECT_EQ(999u, sink.events[1].sampleOffset);
}

TEST(MidiLoopQuantisation, ModelRebuildsWithQuantisedSpans) {
	MidiModelParams modelParams;
	modelParams.ModelScale = 1.0f;
	auto model = std::make_shared<MidiModel>(modelParams);

	MidiLoop loop;
	loop.AttachModel(model);
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(140u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(540u, 0u, 60u));
	loop.EndRecord(1000u);

	ASSERT_TRUE(loop.UpdateModelFromEvents(1000u, true));
	EXPECT_EQ(1u, model->NoteInstanceCount());

	MidiQuantisationSettings s;
	s.Enabled = true;
	s.Fraction = MidiQuantisationFraction::Whole;
	s.GrainSamps = 100u;
	loop.SetQuantisation(s);

	// Settings change bumps the loop revision so the model rebuild reflects the
	// new placement on the next refresh.
	ASSERT_TRUE(loop.UpdateModelFromEvents(1000u, false));
	EXPECT_EQ(1u, model->NoteInstanceCount());
}

TEST(LoopTakeMidiQuantisation, SetMidiQuantisationPropagatesToMidiLoops) {
	auto take = MakeLoopTake();
	take->Record({}, "station", { 3u });

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Quarter;
	settings.GrainSamps = 800u;
	take->SetMidiQuantisation(settings);

	const auto& applied = take->MidiQuantisation();
	EXPECT_TRUE(applied.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, applied.Fraction);
	EXPECT_EQ(800u, applied.GrainSamps);
}

TEST(LoopTakeMidiQuantisation, GuiActionTogglesQuantisation) {
	auto take = MakeLoopTake();
	take->Record({}, "station", { 3u });

	actions::GuiAction action;
	action.Index = 0u;
	action.ElementType = actions::GuiAction::ACTIONELEMENT_MIDIQUANTISATION;
	action.Data = actions::GuiAction::GuiIntArray{ { 1, static_cast<int>(MidiQuantisationFraction::Eighth), 1600 } };
	take->OnAction(action);

	const auto& applied = take->MidiQuantisation();
	EXPECT_TRUE(applied.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Eighth, applied.Fraction);
	EXPECT_EQ(1600u, applied.GrainSamps);
}

TEST(LoopTakeMidiQuantisation, CtrlShiftDragEditsFraction) {
	auto take = MakeLoopTake();

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	take->SetMidiQuantisation(settings);

	TouchAction down;
	down.State = TouchAction::TOUCH_DOWN;
	down.Index = 0;
	down.Position = { 20, 20 };
	down.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);
	const auto downResult = take->OnAction(down);
	EXPECT_TRUE(downResult.IsEaten);

	TouchMoveAction move;
	move.Index = 0;
	move.Position = { 20, -44 };
	move.Modifiers = down.Modifiers;
	const auto moveResult = take->OnAction(move);
	EXPECT_TRUE(moveResult.IsEaten);

	const auto& moved = take->MidiQuantisation();
	EXPECT_TRUE(moved.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, moved.Fraction);
	EXPECT_EQ(1600u, moved.GrainSamps);

	TouchAction up;
	up.State = TouchAction::TOUCH_UP;
	up.Index = 0;
	up.Position = move.Position;
	up.Modifiers = down.Modifiers;
	const auto upResult = take->OnAction(up);
	EXPECT_TRUE(upResult.IsEaten);

	const auto& finished = take->MidiQuantisation();
	EXPECT_TRUE(finished.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, finished.Fraction);
}

TEST(LoopTakeMidiQuantisation, CtrlShiftClickTogglesEnableDisable) {
	auto take = MakeLoopTake();

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Eighth;
	settings.GrainSamps = 1600u;
	take->SetMidiQuantisation(settings);

	auto ctrlShift = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);

	TouchAction down;
	down.State = TouchAction::TOUCH_DOWN;
	down.Index = 0;
	down.Position = { 20, 20 };
	down.Modifiers = ctrlShift;
	EXPECT_TRUE(take->OnAction(down).IsEaten);

	TouchAction up = down;
	up.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(take->OnAction(up).IsEaten);
	EXPECT_TRUE(take->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Eighth, take->MidiQuantisation().Fraction);

	EXPECT_TRUE(take->OnAction(down).IsEaten);
	EXPECT_TRUE(take->OnAction(up).IsEaten);
	EXPECT_FALSE(take->MidiQuantisation().Enabled);

	EXPECT_TRUE(take->OnAction(down).IsEaten);

	TouchMoveAction moveAway;
	moveAway.Index = 0;
	moveAway.Position = { 20, -44 };
	moveAway.Modifiers = ctrlShift;
	EXPECT_TRUE(take->OnAction(moveAway).IsEaten);

	TouchMoveAction moveBack = moveAway;
	moveBack.Position = down.Position;
	EXPECT_TRUE(take->OnAction(moveBack).IsEaten);

	EXPECT_TRUE(take->OnAction(up).IsEaten);
	EXPECT_TRUE(take->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Eighth, take->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, StationDepthMapsHoveredStationToLoopTakes) {
	auto take = MakeLoopTake();
	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	take->SetMidiQuantisation(settings);

	auto station = MakeStation();
	station->AddTake(take);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(station);
	scene.CommitChanges();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_STATION);
	scene.SetHover3d(HoverPathFor(take), base::Action::MODIFIER_NONE);
	ApplyCtrlShiftDrag(scene, { 220, 220 }, { 220, 156 });

	const auto& moved = take->MidiQuantisation();
	EXPECT_TRUE(moved.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, moved.Fraction);

	EXPECT_TRUE(take->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, take->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, StationDepthDragAffectsHoveredAndSelectedStations) {
	auto hoveredTake = MakeLoopTake("take-a");
	auto selectedTake = MakeLoopTake("take-b");

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	hoveredTake->SetMidiQuantisation(settings);
	selectedTake->SetMidiQuantisation(settings);

	auto hoveredStation = MakeStation("station-a");
	hoveredStation->AddTake(hoveredTake);
	auto selectedStation = MakeStation("station-b");
	selectedStation->AddTake(selectedTake);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(hoveredStation);
	scene.AddStationForTest(selectedStation);
	scene.CommitChanges();
	selectedStation->Select();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_STATION);
	scene.SetHover3d(HoverPathFor(hoveredTake), base::Action::MODIFIER_NONE);

	ApplyCtrlShiftDrag(scene, { 220, 220 }, { 220, 156 });

	EXPECT_TRUE(hoveredTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, hoveredTake->MidiQuantisation().Fraction);
	EXPECT_TRUE(selectedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, selectedTake->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, LoopTakeDepthDragAffectsHoveredAndSelectedLoopTakes) {
	auto hoveredTake = MakeLoopTake("take-a");
	auto selectedTake = MakeLoopTake("take-b");

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	hoveredTake->SetMidiQuantisation(settings);
	selectedTake->SetMidiQuantisation(settings);

	auto hoveredStation = MakeStation("station-a");
	hoveredStation->AddTake(hoveredTake);
	auto selectedStation = MakeStation("station-b");
	selectedStation->AddTake(selectedTake);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(hoveredStation);
	scene.AddStationForTest(selectedStation);
	scene.CommitChanges();
	selectedTake->Select();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_LOOPTAKE);
	scene.SetHover3d(HoverPathFor(hoveredTake), base::Action::MODIFIER_NONE);

	ApplyCtrlShiftDrag(scene, { 220, 220 }, { 220, 156 });

	EXPECT_TRUE(hoveredTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, hoveredTake->MidiQuantisation().Fraction);
	EXPECT_TRUE(selectedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, selectedTake->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, LoopDepthDragAffectsHoveredAndSelectedLoops) {
	auto hoveredTake = MakeLoopTake("take-a");
	auto selectedTake = MakeLoopTake("take-b");

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	hoveredTake->SetMidiQuantisation(settings);
	selectedTake->SetMidiQuantisation(settings);

	auto hoveredLoop = hoveredTake->AddLoop(0u, "station-a");
	auto selectedLoop = selectedTake->AddLoop(0u, "station-b");
	auto hoveredStation = MakeStation("station-a");
	hoveredStation->AddTake(hoveredTake);
	auto selectedStation = MakeStation("station-b");
	selectedStation->AddTake(selectedTake);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(hoveredStation);
	scene.AddStationForTest(selectedStation);
	scene.CommitChanges();
	selectedLoop->Select();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_LOOP);
	scene.SetHover3d(HoverPathFor(hoveredLoop), base::Action::MODIFIER_NONE);

	ApplyCtrlShiftDrag(scene, { 220, 220 }, { 220, 156 });

	EXPECT_TRUE(hoveredTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, hoveredTake->MidiQuantisation().Fraction);
	EXPECT_TRUE(selectedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, selectedTake->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, DragKeepsTargetsWhenSelectDepthChangesMidGesture) {
	auto hoveredTake = MakeLoopTake("take-a");
	auto selectedTake = MakeLoopTake("take-b");

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	hoveredTake->SetMidiQuantisation(settings);
	selectedTake->SetMidiQuantisation(settings);

	auto hoveredStation = MakeStation("station-a");
	hoveredStation->AddTake(hoveredTake);
	auto selectedStation = MakeStation("station-b");
	selectedStation->AddTake(selectedTake);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(hoveredStation);
	scene.AddStationForTest(selectedStation);
	scene.CommitChanges();
	selectedStation->Select();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_STATION);
	scene.SetHover3d(HoverPathFor(hoveredTake), base::Action::MODIFIER_NONE);

	auto ctrlShift = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);

	TouchAction down;
	down.State = TouchAction::TOUCH_DOWN;
	down.Index = 0;
	down.Position = { 220, 220 };
	down.Modifiers = ctrlShift;
	EXPECT_TRUE(scene.OnAction(down).IsEaten);

	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_LOOPTAKE);

	TouchMoveAction move;
	move.Index = 0;
	move.Position = { 220, 156 };
	move.Modifiers = ctrlShift;
	EXPECT_TRUE(scene.OnAction(move).IsEaten);

	TouchAction up;
	up.State = TouchAction::TOUCH_UP;
	up.Index = 0;
	up.Position = move.Position;
	up.Modifiers = ctrlShift;
	EXPECT_FALSE(scene.OnAction(up).IsEaten);

	EXPECT_TRUE(hoveredTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, hoveredTake->MidiQuantisation().Fraction);
	EXPECT_TRUE(selectedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, selectedTake->MidiQuantisation().Fraction);
}

TEST(SceneInteractionRouting, TwoDimensionalLoopTakeTouchDoesNotStartSceneRouting) {
	auto touchedTake = MakeLoopTake("take-a");
	auto selectedTake = MakeLoopTake("take-b");

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 1600u;
	touchedTake->SetMidiQuantisation(settings);
	selectedTake->SetMidiQuantisation(settings);

	auto touchedStation = MakeStation("station-a");
	touchedStation->AddTake(touchedTake);
	auto selectedStation = MakeStation("station-b");
	selectedStation->AddTake(selectedTake);

	SceneParams sceneParams{ base::DrawableParams(), base::MoveableParams(), base::SizeableParams{ 400, 300 } };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(touchedStation);
	scene.AddStationForTest(selectedStation);
	scene.CommitChanges();
	selectedStation->Select();
	scene.SetSelectDepthForTest(base::SelectDepth::DEPTH_STATION);
	scene.SetHover3d(HoverPathFor(touchedTake), base::Action::MODIFIER_NONE);

	ApplyCtrlShiftDrag(scene, { 10, 10 }, { 10, -54 });

	EXPECT_TRUE(touchedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, touchedTake->MidiQuantisation().Fraction);
	EXPECT_FALSE(selectedTake->MidiQuantisation().Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Whole, selectedTake->MidiQuantisation().Fraction);
}
