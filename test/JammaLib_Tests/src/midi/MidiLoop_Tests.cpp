#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "midi/MidiEvent.h"
#include "engine/LoopTake.h"
#include "engine/Quantiser.h"
#include "midi/MidiLoop.h"
#include "midi/MidiEditOperations.h"
#include "midi/MidiGridGesture.h"
#include "midi/LoopGridEditor.h"
#include "graphics/LoopGridProjection.h"
#include "glm/ext.hpp"
#include "midi/MidiLoopEditUndo.h"
#include "actions/ActionUndoHistory.h"
#include "graphics/MidiModel.h"
#include "midi/MidiQuantisation.h"
#include "engine/Scene.h"
#include "engine/Station.h"
#include "Timer.h"
#include "io/UserConfig.h"
#include "io/NativeMidiSidecar.h"

using midi::IMidiSink;
using midi::IMidiOutputSink;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Quantiser;
using midi::MidiEvent;
using midi::MidiLoop;
using midi::MidiLoopState;
using graphics::MidiModel;
using graphics::MidiModelParams;
using midi::MidiQuantisationFraction;
using midi::MidiQuantisationSettings;
using engine::Scene;
using engine::SceneParams;
using engine::Station;
using engine::StationParams;
using utils::Timer;
using actions::TouchAction;
using actions::TouchMoveAction;
using audio::MergeMixBehaviourParams;
using base::Audible;

class MidiLoopCapturingSink : public IMidiSink
{
public:
	std::vector<MidiEvent> events;
	void OnEvent(const MidiEvent& ev) noexcept override { events.push_back(ev); }
	void Clear() noexcept { events.clear(); }
};

class MidiLoopCapturingOutputSink : public IMidiOutputSink
{
public:
	struct CapturedEvent
	{
		unsigned int outputIndex;
		MidiEvent event;
	};

	std::vector<CapturedEvent> events;
	void OnEvent(unsigned int outputIndex, const MidiEvent& ev) noexcept override
	{
		events.push_back({ outputIndex, ev });
	}
};

class MidiLoopCapturingGuiReceiver : public base::ActionReceiver
{
public:
	actions::ActionResult OnAction(actions::GuiAction action) override
	{
		Actions.push_back(action);
		return actions::ActionResult::NoAction();
	}

	std::vector<actions::GuiAction> Actions;
};

static std::shared_ptr<LoopTake> MakeLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<LoopTake>(params, mixerParams);
}

static std::shared_ptr<Station> MakeStation(const std::string& name = "station")
{
	StationParams params;
	params.Name = name;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	return std::make_shared<Station>(params, mixerParams);
}

class MidiLoopTestScene : public Scene
{
public:
	MidiLoopTestScene(SceneParams params,
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

static constexpr unsigned int ScenePhaseDragSampleRate = 48000u;

static io::UserConfig MakeSceneUserConfig()
{
	io::UserConfig userConfig = {};
	userConfig.Audio.SampleRate = ScenePhaseDragSampleRate;
	return userConfig;
}

static std::int32_t ExpectedPhaseOffsetForDrag(const utils::Position2d& start,
	const utils::Position2d& finish)
{
	return engine::Quantiser::ResolvePhaseOffsetDrag(0,
		finish.X - start.X,
		ScenePhaseDragSampleRate);
}

static std::vector<unsigned char> HoverPathFor(const std::shared_ptr<base::GuiElement>& element)
{
	std::vector<unsigned char> hoverPath;
	for (auto idPart : element->GlobalId())
		hoverPath.push_back(static_cast<unsigned char>(idPart + 1u));
	hoverPath.push_back(0u);
	return hoverPath;
}

static void AddRecordedLoopForVisual(std::shared_ptr<LoopTake> take,
	const std::string& stationName,
	std::uint64_t transportStartSamps)
{
	take->Record({ 0u }, stationName, {}, {}, {}, transportStartSamps);
	take->EndMultiWrite(1000u, true, Audible::AUDIOSOURCE_ADC);
	take->Play(0u, 1000u, 0u);
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

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, static_cast<std::uint32_t>(capacity + 2u), sink);
	ASSERT_EQ(capacity, sink.events.size());
	ASSERT_EQ(0u, sink.events.front().sampleOffset);
	ASSERT_EQ(static_cast<std::uint32_t>(capacity - 1u), sink.events.back().sampleOffset);
}

TEST(MidiLoop, EmptyLoopProducesNoEvents) {
	MidiLoop loop;
	loop.StartRecord();
	loop.EndRecord(1000u);

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_TRUE(sink.events.empty());
}

TEST(MidiLoop, PlaysBackEventsAtCorrectGlobalSamples) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(900u, 0, 60));
	loop.EndRecord(1000u);

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

// HeldNotes() tracks notes that have been played (NoteOn emitted) but not yet released.
// Station::_DitchLoopTake reads HeldNotes() before calling Ditch() to enqueue NoteOff
// events, preventing stuck notes in the VST instrument.
TEST(MidiLoop, HeldNotesTracksPlayedButUnreleasedNotes) {
	MidiLoop loop;
	loop.StartRecord();
	// NoteOn with no matching NoteOff — note will remain held after playback.
	loop.RecordEvent(MidiEvent::MakeNoteOn(10u, 0, 60, 100));
	loop.EndRecord(1000u);

	// Before any ReadBlock, no notes are held.
	EXPECT_TRUE(loop.HeldNotes().none());

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 100u, sink); // block [0,100) — plays the NoteOn at loopOffset 10.

	ASSERT_EQ(1u, sink.events.size());
	ASSERT_TRUE(sink.events[0].IsNoteOn());

	// After playback, the note should be tracked as held.
	EXPECT_TRUE(loop.HeldNotes().test(MidiLoop::NoteSlot(0, 60)));

	// Other notes and channels are not held.
	EXPECT_FALSE(loop.HeldNotes().test(MidiLoop::NoteSlot(0, 61)));
	EXPECT_FALSE(loop.HeldNotes().test(MidiLoop::NoteSlot(1, 60)));
}


TEST(MidiLoop, EventsBeyondLoopLengthAreNotPlayed) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u,  0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOn(1500u, 0, 64, 100)); // beyond loop length
	loop.EndRecord(1000u);

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(1u, sink.events.size());
	ASSERT_EQ(60u, sink.events[0].data1);
}

TEST(MidiLoop, SnapshotForExportSkipsEventsOutsidePlayableWindow) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOn(1500u, 0, 64, 100));
	loop.EndRecord(1000u);

	MidiLoop::ExportState saved;
	ASSERT_TRUE(loop.SnapshotForExport(saved));
	ASSERT_EQ(1000u, saved.LoopLengthSamps);
	ASSERT_EQ(1u, saved.EventCount);
	EXPECT_EQ(100u, saved.Events[0].sampleOffset);
	EXPECT_EQ(60u, saved.Events[0].data1);
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

TEST(MidiModelDisc, EmptyLoopWithLengthStillRendersDiscInstance) {
	auto model = std::make_shared<MidiModel>(MidiModelParams());
	model->UpdateModel({}, 1000u);

	// No notes, but the middle-C plane disc is always present so the loop
	// remains a reliable hover/select target.
	EXPECT_EQ(0u, model->NoteInstanceCount());
	EXPECT_GE(model->TotalInstanceCount(), 1u);
}

TEST(MidiModelDisc, ZeroLengthLoopRendersNoInstances) {
	auto model = std::make_shared<MidiModel>(MidiModelParams());
	model->UpdateModel({}, 0u);

	EXPECT_EQ(0u, model->NoteInstanceCount());
	EXPECT_EQ(0u, model->TotalInstanceCount());
}

TEST(MidiModelDisc, LoopWithNotesRendersDiscPlusEachNote) {
	MidiLoop loop;
	auto model = std::make_shared<MidiModel>(MidiModelParams());
	loop.AttachModel(model);

	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(100u, 0, 60, 100));
	loop.RecordEvent(MidiEvent::MakeNoteOff(500u, 0, 60));

	ASSERT_TRUE(loop.UpdateModelFromEvents(1000u, true));
	EXPECT_EQ(1u, model->NoteInstanceCount());
	// One disc instance is appended on top of the per-note instances.
	EXPECT_EQ(model->NoteInstanceCount() + 1u, model->TotalInstanceCount());
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
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	auto midiLoop = take->GetMidiLoops()[0];
	auto midiModel = midiLoop->Model();
	ASSERT_NE(nullptr, midiModel);

	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3, 60, 100), 0u));
	take->EndMultiWrite(480u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(0u, 3, 60), 0u));

	take->Play(0ul, 960ul, 0u);
	EXPECT_TRUE(midiLoop->UpdateModelFromEvents(960u, true));

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
	// Device/channel streams remain separate loops, but share one visible pick ring.
	EXPECT_EQ(1u, firstMidiModel->TotalInstanceCount());
	EXPECT_EQ(0u, secondMidiModel->TotalInstanceCount());

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

TEST(LoopTakeMidiTiming, FirstMasterRecordsFromMidiPressBeforeAudioTakeIsPublished)
{
	auto take = MakeLoopTake();
	take->Record({}, "station", { 0u }, {}, {}, 0u, 1000u);
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(1200u, 0u, 38u, 100u), 1300u));
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(1600u, 0u, 36u, 100u), 1700u));
	take->Play(0u, 2000u, 0u);

	ASSERT_EQ(1u, take->GetMidiLoops().size());
	MidiEvent event{};
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, event));
	EXPECT_EQ(200u, event.sampleOffset);
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(1u, event));
	EXPECT_EQ(600u, event.sampleOffset);
	EXPECT_EQ(0u, LoopTake::ResolveMidiRecordSampleFromTrigger(900u, 1000u));
	EXPECT_EQ(200u, LoopTake::ResolveMidiRecordSampleFromTrigger(100u, 0xffff'ff9cu));
}

TEST(LoopTakeMidiTiming, MidiOnlyMasterUsesPressIntervalAndStopPhase)
{
	auto station = MakeStation("midi-master");
	station->SetVisible(true);
	station->SetEnabled(true);
	station->SetAllowedMidiChannels({ 1 });
	auto clock = std::make_shared<Timer>();
	station->SetClock(clock);
	clock->Tick(81280u, 0u);

	io::UserConfig cfg = {};
	const auto physicalLength = 164919u - 77732u;
	const auto expectedTiming = cfg.DeduceLoopTiming(physicalLength, 44100u);
	ASSERT_TRUE(expectedTiming.has_value());
	actions::TriggerAction start;
	start.ActionType = actions::TriggerAction::TRIGGER_REC_START;
	start.MidiSample = 77732u;
	start.SetUserConfig(cfg);
	const auto started = station->OnAction(start);
	ASSERT_TRUE(started.IsEaten);
	ASSERT_EQ(1u, station->GetLoopTakes().size());
	const auto take = station->GetLoopTakes().front();
	ASSERT_TRUE(take->RecordMidiEvent(
		MidiEvent::MakeNoteOn(95277u, 0u, 38u, 100u), 95680u));

	clock->Tick(86656u, 0u); // Scene sample 167936, after stop press 164919.
	actions::TriggerAction end;
	end.ActionType = actions::TriggerAction::TRIGGER_REC_END;
	end.TargetId = started.TargetId;
	end.SampleCount = 84928u; // The trigger block counter lags the press interval.
	end.MidiSample = 164919u;
	end.SetUserConfig(cfg);
	const auto ended = station->OnAction(end);
	ASSERT_TRUE(ended.IsEaten);
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	MidiEvent snare{};
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, snare));
	EXPECT_EQ(17545u, snare.sampleOffset);
	const auto expectedLength = static_cast<unsigned long>(expectedTiming->GrainSamps)
		* expectedTiming->LoopGrains;
	EXPECT_EQ(expectedLength, clock->SeedSourceLength());
	EXPECT_EQ(expectedLength, take->GetMidiLoops()[0]->LoopLengthSamps());
	EXPECT_NEAR(3017u, clock->SampOffset(), 1u);
	EXPECT_EQ(clock->SampOffset(), take->MidiPlayIndex());
}

TEST(LoopTakeMidiTiming, FirstPlaybackStartsAtRecordedStartAfterAudioDelayCompensation)
{
	io::UserConfig cfg;
	cfg.Trigger.PreDelay = 64u;

	const auto loopLength = 96000ul;
	const auto playPos = cfg.LoopPlayPos(0, loopLength, 128u);
	const auto endRecordSamps = cfg.EndRecordingSamps(0);

	auto take = MakeLoopTake();
	take->Record({}, "station", { 0u });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), 0u));
	take->Play(playPos, loopLength, endRecordSamps);

	MidiLoopCapturingOutputSink sink;
	const auto firstBlockStart = 12345u;
	EXPECT_EQ(1u, take->ReadMidiBlock(firstBlockStart, 64u, sink));

	ASSERT_EQ(1u, sink.events.size());
	EXPECT_EQ(0u, sink.events[0].outputIndex);
	EXPECT_EQ(firstBlockStart, sink.events[0].event.sampleOffset);
	EXPECT_TRUE(sink.events[0].event.IsNoteOn());
}

TEST(LoopTakeMidiPlayback, MasterLevelScalesNoteVelocityOnly)
{
	auto take = MakeLoopTake();
	take->SetVisible(true);
	take->SetEnabled(true);
	take->Record({}, "station", { 0u });
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), 0u));
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(10u, 0u, 60u, 64u), 10u));
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent{ 20u, 0xB0u, 1u, 100u, 0u }, 20u));
	take->Play(0u, 100u, 0u);

	actions::GuiAction masterAction;
	masterAction.ElementType = actions::GuiAction::ACTIONELEMENT_SLIDER;
	masterAction.Index = 0u;
	masterAction.Data = actions::GuiAction::GuiDouble(0.5);
	take->OnAction(masterAction);

	MidiLoopCapturingOutputSink sink;
	EXPECT_EQ(1u, take->ReadMidiBlock(0u, 32u, sink));
	ASSERT_EQ(3u, sink.events.size());
	for (const auto& captured : sink.events)
	{
		if (captured.event.IsNoteOn())
			EXPECT_EQ(50u, captured.event.data2);
		else if (captured.event.IsNoteOff())
			EXPECT_EQ(64u, captured.event.data2);
		else if (captured.event.MessageType() == 0xB0u)
			EXPECT_EQ(100u, captured.event.data2);
		else
			FAIL() << "Unexpected MIDI message type";
	}
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
    MidiLoopCapturingSink sink;
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

    MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink sink;
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

	MidiLoopCapturingSink wholeSink;
	loop.ReadBlock(0u, 1000u, wholeSink);
	ASSERT_EQ(2u, wholeSink.events.size());
	EXPECT_EQ(0u, wholeSink.events[0].sampleOffset);
	EXPECT_EQ(100u, wholeSink.events[1].sampleOffset);

	s.Fraction = MidiQuantisationFraction::Half; // step 100, 70 -> 100
	loop.SetQuantisation(s);

	MidiLoopCapturingSink halfSink;
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

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(600u, sink.events[0].sampleOffset);
	EXPECT_EQ(999u, sink.events[1].sampleOffset);
}

TEST(MidiLoopQuantisation, EmitsQuantisedSameBlockEventsInCanonicalOrder) {
	MidiLoop loop;
	loop.StartRecord();
	loop.RecordEvent(MidiEvent::MakeNoteOn(120u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(220u, 0u, 60u));
	loop.RecordEvent(MidiEvent::MakeNoteOn(200u, 0u, 60u, 100u));
	loop.RecordEvent(MidiEvent::MakeNoteOff(300u, 0u, 60u));
	loop.EndRecord(1000u);

	MidiQuantisationSettings s;
	s.Enabled = true;
	s.Fraction = MidiQuantisationFraction::Whole;
	s.GrainSamps = 100u;
	loop.SetQuantisation(s);

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(4u, sink.events.size());
	EXPECT_EQ(100u, sink.events[0].sampleOffset);
	EXPECT_TRUE(sink.events[0].IsNoteOn());
	EXPECT_EQ(200u, sink.events[1].sampleOffset);
	EXPECT_TRUE(sink.events[1].IsNoteOff());
	EXPECT_EQ(200u, sink.events[2].sampleOffset);
	EXPECT_TRUE(sink.events[2].IsNoteOn());
	EXPECT_EQ(300u, sink.events[3].sampleOffset);
	EXPECT_TRUE(sink.events[3].IsNoteOff());
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

TEST(LoopTakeMidiQuantisation, GlobalStateForcesResolvedEnabledAndMixedRestoresLocal) {
	auto take = MakeLoopTake("global-mode-take");
	auto station = MakeStation("global-mode-station");
	station->AddTake(take);

	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Quarter;
	settings.GrainSamps = 480u;
	take->SetMidiQuantisation(settings);

	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	EXPECT_TRUE(take->ResolvedMidiQuantisation().Enabled);

	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Off);
	EXPECT_FALSE(take->ResolvedMidiQuantisation().Enabled);

	settings.Enabled = true;
	take->SetMidiQuantisation(settings);
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	EXPECT_TRUE(take->ResolvedMidiQuantisation().Enabled);
}

TEST(LoopTakeMidiQuantisation, LocalMidiQuantEditForwardsGuiActionToStationReceiver) {
	auto take = MakeLoopTake("local-edit-take");
	auto station = MakeStation("local-edit-station");
	auto receiver = std::make_shared<MidiLoopCapturingGuiReceiver>();
	station->SetReceiver(receiver);
	station->AddTake(take);
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Eighth;
	settings.GrainSamps = 960u;
	take->SetMidiQuantisationFromUserEdit(settings);

	ASSERT_FALSE(receiver->Actions.empty());
	EXPECT_EQ(actions::GuiAction::ACTIONELEMENT_MIDIQUANTISATION, receiver->Actions.back().ElementType);
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

TEST(LoopTakeMidiQuantisation, TransportStartDoesNotBecomeUserPhaseOffset) {
	auto take = MakeLoopTake("take-phase-anchor");
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;
	take->SetMidiQuantisation(settings);

	take->Record({}, "station", { 0u }, {}, {}, 250u);
	EXPECT_EQ(250u, take->MidiQuantisationTransportStartSamps());
	EXPECT_EQ(0, take->ResolvedMidiQuantisation().PhaseOffsetSamps);

	settings.PhaseOffsetSamps = 20;
	take->SetMidiQuantisation(settings);
	take->SetMidiQuantisationInheritedPhaseOffset(10);
	EXPECT_EQ(30, take->ResolvedMidiQuantisation().PhaseOffsetSamps);
}

TEST(LoopTakeMidiQuantisation, DifferentTakeStartsQuantiseToSharedTransportGrid) {
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;

	auto firstTake = MakeLoopTake("take-start-zero");
	firstTake->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	firstTake->SetMidiQuantisation(settings);
	firstTake->Record({}, "station", { 0u }, {}, {}, 0u);
	firstTake->EndMultiWrite(260u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(firstTake->RecordMidiEvent(MidiEvent::MakeNoteOn(260u, 0u, 60u, 100u), 260u));
	firstTake->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(firstTake->RecordMidiEvent(MidiEvent::MakeNoteOff(360u, 0u, 60u), 360u));
	firstTake->Play(0u, 1000u, 0u);

	auto shiftedTake = MakeLoopTake("take-start-250");
	shiftedTake->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	shiftedTake->SetMidiQuantisation(settings);
	shiftedTake->Record({}, "station", { 0u }, {}, {}, 250u);
	shiftedTake->EndMultiWrite(10u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(shiftedTake->RecordMidiEvent(MidiEvent::MakeNoteOn(260u, 0u, 60u, 100u), 260u));
	shiftedTake->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(shiftedTake->RecordMidiEvent(MidiEvent::MakeNoteOff(360u, 0u, 60u), 360u));
	shiftedTake->Play(0u, 1000u, 0u);

	MidiLoopCapturingOutputSink firstSink;
	EXPECT_EQ(1u, firstTake->ReadMidiBlock(0u, 500u, firstSink));
	ASSERT_EQ(2u, firstSink.events.size());
	EXPECT_EQ(300u, firstSink.events[0].event.sampleOffset);
	EXPECT_TRUE(firstSink.events[0].event.IsNoteOn());

	MidiLoopCapturingOutputSink shiftedSink;
	EXPECT_EQ(1u, shiftedTake->ReadMidiBlock(0u, 500u, shiftedSink));
	ASSERT_EQ(2u, shiftedSink.events.size());
	EXPECT_EQ(50u, shiftedSink.events[0].event.sampleOffset);
	EXPECT_TRUE(shiftedSink.events[0].event.IsNoteOn());

	EXPECT_EQ(300u,
		firstSink.events[0].event.sampleOffset
		+ static_cast<std::uint32_t>(firstTake->MidiQuantisationTransportStartSamps()));
	EXPECT_EQ(300u,
		shiftedSink.events[0].event.sampleOffset
		+ static_cast<std::uint32_t>(shiftedTake->MidiQuantisationTransportStartSamps()));
}

TEST(LoopTakeMidiQuantisation, CapturedStartAndCursorOverrideDelayedTriggerTiming) {
	auto take = MakeLoopTake("delayed-midi-start");
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;
	take->SetMidiQuantisation(settings);

	// The structural trigger reported 250, but the first recorded audio block
	// began at 300. The take's MIDI sample positions use that captured boundary.
	take->Record({ 0u }, "station", { 0u }, {}, {}, 250u);
	take->CaptureMidiTransportStartAtAudioBoundary(300u);
	take->EndMultiWrite(60u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(360u, 0u, 60u, 100u), 360u));
	take->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	ASSERT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(460u, 0u, 60u), 460u));
	take->EndMultiWrite(40u, true, Audible::AUDIOSOURCE_ADC);

	// Trigger duration gives -100, while the captured playback cursor is 200.
	take->Play(700u, 1000u, 0u, -100);
	EXPECT_EQ(300u, take->MidiQuantisationTransportStartSamps());
	EXPECT_EQ(0, take->ResolvedMidiQuantisation().PhaseOffsetSamps);
	const auto visual = take->QuantisationVisual();
	ASSERT_TRUE(visual.has_value());
	EXPECT_NEAR(0.8, visual->LoopIndexFrac, 1.0e-6);
	EXPECT_TRUE(visual->UseAbsoluteLocalGrid);
	MidiLoopCapturingOutputSink sink;
	EXPECT_EQ(1u, take->ReadMidiBlock(0u, 950u, sink));
	const auto noteOn = std::find_if(sink.events.begin(), sink.events.end(),
		[](const auto& event) { return event.event.IsNoteOn(); });
	ASSERT_NE(sink.events.end(), noteOn);
	EXPECT_EQ(900u, noteOn->event.sampleOffset);
}

TEST(LoopTakeMidiQuantisation, RestoredMidiUsesDeferredJobAfterGlobalAllGrainPropagation) {
	constexpr std::uint32_t loopLengthSamps = 400u;
	constexpr std::uint32_t grainSamps = 100u;
	constexpr std::uint64_t transportStartSamps = 250u;
	constexpr unsigned long savedPlayIndex = 20ul;
	constexpr std::uint32_t outputBlockStart = 1000u;

	LoopTake::MidiExportState state;
	state.PlayIndex = savedPlayIndex;
	state.LoopLengthSamps = loopLengthSamps;
	state.Quantisation = { false, MidiQuantisationFraction::Whole, 0u, 0 };
	state.QuantisationTransportStartSamps = transportStartSamps;
	LoopTake::MidiStreamExport stream;
	stream.Channel = 0u;
	stream.Loop.LoopLengthSamps = loopLengthSamps;
	stream.Loop.EventCount = 2u;
	stream.Loop.Events[0] = MidiEvent::MakeNoteOn(35u, 0u, 60u, 100u);
	stream.Loop.Events[1] = MidiEvent::MakeNoteOff(55u, 0u, 60u);
	state.Streams.push_back(std::move(stream));

	auto take = MakeLoopTake("restored-deferred-midi");
	ASSERT_TRUE(take->RestoreMidiFromExport(state));
	EXPECT_EQ(0u, take->MidiQuantisation().GrainSamps);
	EXPECT_EQ(savedPlayIndex, take->MidiPlayIndex());

	auto station = MakeStation("restored-deferred-station");
	station->AddTake(take);
	Quantiser quantiser;
	quantiser.SetMidiGrain(grainSamps, "parsed local manifest", { station });
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);

	const auto jobs = take->CommitChanges();
	const auto job = std::find_if(jobs.begin(), jobs.end(), [](const actions::JobAction& candidate)
		{
			return candidate.JobActionType == actions::JobAction::JOB_UPDATEMIDIQUANTISATION;
		});
	ASSERT_NE(jobs.end(), job);
	auto receiver = job->Receiver.lock();
	ASSERT_TRUE(receiver);
	receiver->OnAction(*job);

	MidiLoopCapturingOutputSink sink;
	EXPECT_EQ(1u, take->ReadMidiBlock(outputBlockStart, 100u, sink));
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_TRUE(sink.events[0].event.IsNoteOn());
	EXPECT_EQ(1030u, sink.events[0].event.sampleOffset);
	EXPECT_EQ(300u, sink.events[0].event.sampleOffset - outputBlockStart
		+ static_cast<std::uint32_t>(savedPlayIndex)
		+ static_cast<std::uint32_t>(transportStartSamps));
	EXPECT_EQ(savedPlayIndex, take->MidiPlayIndex());
}

TEST(LoopTakeMidiQuantisation, LoadedTakesRestoreDistinctPersistedIds) {
	io::JamFile::Station savedStation;
	savedStation.Name = "loaded-midi-station";
	savedStation.LoopTakes.resize(2u);
	const auto dir = std::filesystem::temp_directory_path() / "jamma-loaded-take-id-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directory(dir);
	for (std::size_t i = 0u; i < savedStation.LoopTakes.size(); ++i)
	{
		auto& savedTake = savedStation.LoopTakes[i];
		savedTake.Name = "loaded-take-" + std::to_string(i);
		savedTake.MidiPlayLength = 100u;
		auto sidecarPath = "loaded-take-" + std::to_string(i) + ".jammidi";
		savedTake.MidiStreams.push_back({ sidecarPath, 0u, "", 100u, 0u });
		io::NativeMidiSidecar::Stream sidecar;
		sidecar.LogicalLength = 100u;
		sidecar.Events.push_back({ 0u, 0x90u, 60u, 100u });
		std::ofstream stream(dir / sidecarPath, std::ios::binary);
		ASSERT_TRUE(stream);
		ASSERT_TRUE(io::NativeMidiSidecar::ToStream(sidecar, stream));
		stream.close();
	}

	StationParams params;
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	auto restored = Station::FromFile(params, mixerParams, savedStation, dir.wstring());
	ASSERT_TRUE(restored.has_value());
	const auto& takes = restored.value()->GetLoopTakes();
	ASSERT_EQ(2u, takes.size());
	ASSERT_TRUE(takes[0]);
	ASSERT_TRUE(takes[1]);
	EXPECT_EQ("loaded-take-0", takes[0]->Id());
	EXPECT_EQ("loaded-take-1", takes[1]->Id());
	std::filesystem::remove_all(dir);
}

TEST(LoopTakeMidiQuantisation, StationPhaseOffsetsComposeForExistingAndNewTakes) {
	auto firstTake = MakeLoopTake("phase-take-a");
	auto secondTake = MakeLoopTake("phase-take-b");
	auto station = MakeStation("phase-station");

	station->SetGlobalPhaseOffsetSamps(100);
	station->SetStationPhaseOffsetSamps(25);
	station->AddTake(firstTake);
	EXPECT_EQ(125, firstTake->ResolvedMidiQuantisation().PhaseOffsetSamps);

	station->SetStationPhaseOffsetSamps(40);
	station->AddTake(secondTake);
	EXPECT_EQ(140, firstTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
	EXPECT_EQ(140, secondTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
}

TEST(LoopTakeMidiQuantisation, QuantisationGlobalPhasePropagatesAcrossStations) {
	auto firstTake = MakeLoopTake("global-phase-take-a");
	auto secondTake = MakeLoopTake("global-phase-take-b");
	auto firstStation = MakeStation("global-phase-station-a");
	auto secondStation = MakeStation("global-phase-station-b");
	firstStation->AddTake(firstTake);
	secondStation->AddTake(secondTake);
	secondStation->SetStationPhaseOffsetSamps(30);

	engine::Quantiser quantisation;
	quantisation.SetGlobalPhaseOffsetSamps(240, { firstStation, secondStation });

	EXPECT_EQ(240, firstTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
	EXPECT_EQ(270, secondTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
}

TEST(LoopTakeMidiQuantisation, ResolvedPhasePublicationComposesGlobalStationTakeAndTransport) {
	auto take = MakeLoopTake("published-phase-take");
	auto station = MakeStation("published-phase-station");
	station->SetGlobalPhaseOffsetSamps(100);
	station->SetStationPhaseOffsetSamps(25);
	station->AddTake(take);
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;
	settings.PhaseOffsetSamps = 10;
	take->SetMidiQuantisation(settings);

	take->Record({}, station->Name(), { 0u }, {}, {}, 250u);
	EXPECT_EQ(135, take->ResolvedMidiQuantisation().PhaseOffsetSamps);
}

TEST(LoopTakeMidiQuantisation, QuantisationVisualPublishesResolvedPhase) {
	auto take = MakeLoopTake("visual-phase-take");
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.GrainSamps = 100u;
	settings.PhaseOffsetSamps = 12;
	take->SetMidiQuantisation(settings);
	take->SetMidiQuantisationInheritedPhaseOffset(30);
	AddRecordedLoopForVisual(take, "visual-station", 250u);

	auto visual = take->QuantisationVisual();
	ASSERT_TRUE(visual.has_value());
	EXPECT_EQ(42, visual->PhaseOffsetSamps);
	EXPECT_FALSE(visual->UseAbsoluteLocalGrid);
	EXPECT_EQ(100u, visual->GrainSamps);
	EXPECT_EQ(10u, visual->LoopGrains);
}

TEST(MidiLoopBuildApi, ReplaceRecordedEventsPreservesPlaybackTiming)
{
	MidiLoop loop;
	std::array<MidiEvent, 2u> events = {
		MidiEvent::MakeNoteOn(100u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(200u, 0u, 60u)
	};

	loop.ReplaceRecordedEvents(events.data(), events.size(), 512u);

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 512u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(100u, sink.events[0].sampleOffset);
	EXPECT_EQ(200u, sink.events[1].sampleOffset);
}

TEST(MidiLoopBuildApi, ReplaceRecordedEventsUpdatesDroppedCountWhenCapacityExceeded)
{
	MidiLoop loop;
	std::vector<MidiEvent> events;
	events.reserve(MidiLoop::Capacity() + 2u);

	for (std::size_t i = 0u; i < MidiLoop::Capacity() + 2u; ++i)
		events.push_back(MidiEvent::MakeNoteOn(static_cast<std::uint32_t>(i), 0u, static_cast<std::uint8_t>(i & 0x7Fu), 100u));

	loop.ReplaceRecordedEvents(events.data(), events.size(), 1024u);

	EXPECT_EQ(MidiLoop::Capacity(), loop.EventCount());
	EXPECT_EQ(2u, loop.DroppedEventCount());
}

TEST(MidiLoopBuildApi, ReplaceRecordedEventsKeepsQuantisationAndModelFlow)
{
	MidiLoop loop;
	auto model = std::make_shared<MidiModel>(MidiModelParams{});
	loop.AttachModel(model);

	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.Fraction = MidiQuantisationFraction::Whole;
	quant.GrainSamps = 100u;
	loop.SetQuantisation(quant);

	std::array<MidiEvent, 2u> events = {
		MidiEvent::MakeNoteOn(140u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(540u, 0u, 60u)
	};

	loop.ReplaceRecordedEvents(events.data(), events.size(), 1000u);

	MidiLoopCapturingSink sink;
	loop.ReadBlock(0u, 1000u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(100u, sink.events[0].sampleOffset);
	EXPECT_EQ(500u, sink.events[1].sampleOffset);
	EXPECT_TRUE(loop.UpdateModelFromEvents(1000u, true));
	EXPECT_EQ(1u, model->NoteInstanceCount());
}

TEST(MidiLoopPersistence, RestoreKeepsRawSimultaneousEventOrderAndAutomationOrigin)
{
	MidiLoop::ExportState saved;
	saved.LoopLengthSamps = 512u;
	saved.AutomationGlobalSampleOrigin = 0xfffffff0u;
	saved.EventCount = 3u;
	// The order is significant for simultaneous events.  The restore path must
	// not apply ReplaceRecordedEvents' playback sort a second time.
	saved.Events[0] = MidiEvent::MakeNoteOn(64u, 2u, 61u, 90u);
	saved.Events[1] = MidiEvent{ 64u, 0xb2u, 7u, 100u, 0u };
	saved.Events[2] = MidiEvent::MakeNoteOff(64u, 2u, 61u);
	auto& lane = saved.AutomationLanes[0];
	lane.MatchKey = midi::AutomationMapping::MakeMatchKey(2u, 7u);
	lane.TargetParameterIndex = 19u;
	lane.PointCount = 2u;
	lane.Points[0] = { 0.25f, 0.1f };
	lane.Points[1] = { 0.75f, 0.9f };

	MidiLoop loop;
	ASSERT_TRUE(loop.RestoreFromExport(saved));
	MidiLoop::ExportState restored;
	ASSERT_TRUE(loop.SnapshotForExport(restored));
	EXPECT_EQ(saved.LoopLengthSamps, restored.LoopLengthSamps);
	EXPECT_EQ(saved.AutomationGlobalSampleOrigin, restored.AutomationGlobalSampleOrigin);
	ASSERT_EQ(saved.EventCount, restored.EventCount);
	for (std::size_t i = 0u; i < saved.EventCount; ++i)
	{
		EXPECT_EQ(saved.Events[i].sampleOffset, restored.Events[i].sampleOffset);
		EXPECT_EQ(saved.Events[i].status, restored.Events[i].status);
		EXPECT_EQ(saved.Events[i].data1, restored.Events[i].data1);
		EXPECT_EQ(saved.Events[i].data2, restored.Events[i].data2);
	}
	EXPECT_EQ(lane.MatchKey, restored.AutomationLanes[0].MatchKey);
	EXPECT_EQ(lane.TargetParameterIndex, restored.AutomationLanes[0].TargetParameterIndex);
	EXPECT_EQ(lane.PointCount, restored.AutomationLanes[0].PointCount);
	EXPECT_EQ(lane.Points[1], restored.AutomationLanes[0].Points[1]);
}

TEST(LoopTakeMidiOverdub, OverdubCreatesMidiLoopsMatchingConfiguredChannelsAndDevices)
{
	auto take = MakeLoopTake("overdub-midi-setup");
	take->Overdub({}, "station", { 1u, 2u }, { "KeysA", "KeysB" });

	EXPECT_EQ(4u, take->GetMidiLoops().size());
	EXPECT_EQ(4u, take->MidiLoopChannels().size());
	EXPECT_EQ(4u, take->MidiLoopDevices().size());
}

TEST(LoopTakeMidiOverdub, RecordMidiEventIgnoredOutsidePunchWindowForOverdub)
{
	auto take = MakeLoopTake("overdub-ignore-outside-punch");
	take->Overdub({}, "station", { 3u }, { "Keys" });

	EXPECT_FALSE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3u, 60u, 100u), "Keys", 0u));
	take->Play(0u, 128u, 0u);

	ASSERT_EQ(1u, take->GetMidiLoops().size());
	EXPECT_EQ(0u, take->GetMidiLoops()[0]->EventCount());
}

TEST(LoopTakeMidiOverdub, PunchInCapturesAlreadyHeldLiveNoteAtPunchStart)
{
	auto take = MakeLoopTake("overdub-held-note-punchin");
	take->Overdub({}, "station", { 3u }, { "Keys" });

	take->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_FALSE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(100u, 3u, 60u, 100u), "Keys", 100u));
	take->PunchIn();
	take->EndMultiWrite(20u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(120u, 3u, 60u), "Keys", 120u));
	take->PunchOut();
	take->Play(0u, 200u, 0u);

	MidiEvent first{};
	MidiEvent second{};
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, first));
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(1u, second));
	EXPECT_TRUE(first.IsNoteOn());
	EXPECT_EQ(100u, first.sampleOffset);
	EXPECT_TRUE(second.IsNoteOff());
	EXPECT_EQ(120u, second.sampleOffset);
}

TEST(LoopTakeMidiOverdub, PunchInDoesNotDuplicateMatchingSourceHeldNote)
{
	auto source = MakeLoopTake("source-midi-dedupe");
	source->Record({}, "station", { 3u }, { "Keys" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "Keys", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "Keys", 100u));
	source->Play(0u, 100u, 0u);

	auto target = MakeLoopTake("target-midi-dedupe");
	target->Overdub({}, "station", { 3u }, { "Keys" }, source);

	target->EndMultiWrite(15u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_FALSE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(15u, 3u, 60u, 100u), "Keys", 15u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	target->PunchIn();
	target->EndMultiWrite(20u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(40u, 3u, 60u), "Keys", 40u));
	target->PunchOut();
	target->Play(0u, 100u, 0u);

	ASSERT_EQ(4u, target->GetMidiLoops()[0]->EventCount());
	std::array<MidiEvent, 4u> events{};
	for (std::size_t i = 0u; i < events.size(); ++i)
		ASSERT_TRUE(target->GetMidiLoops()[0]->TryGetEvent(i, events[i]));

	EXPECT_TRUE(events[0].IsNoteOn());
	EXPECT_EQ(10u, events[0].sampleOffset);
	EXPECT_EQ(60u, events[0].data1);
	EXPECT_TRUE(events[1].IsNoteOff());
	EXPECT_EQ(40u, events[1].sampleOffset);
	EXPECT_EQ(60u, events[1].data1);
	EXPECT_TRUE(events[2].IsNoteOn());
	EXPECT_EQ(40u, events[2].sampleOffset);
	EXPECT_EQ(60u, events[2].data1);
	EXPECT_TRUE(events[3].IsNoteOff());
	EXPECT_EQ(90u, events[3].sampleOffset);
	EXPECT_EQ(60u, events[3].data1);
}

TEST(LoopTakeMidiOverdub, PunchOutClosesHeldLiveNoteAtPunchEnd)
{
	auto take = MakeLoopTake("overdub-held-note-punchout");
	take->Overdub({}, "station", { 3u }, { "Keys" });

	take->PunchIn();
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 3u, 60u, 100u), "Keys", 0u));
	take->EndMultiWrite(64u, true, Audible::AUDIOSOURCE_ADC);
	take->PunchOut();
	take->Play(0u, 128u, 0u);

	MidiEvent first{};
	MidiEvent second{};
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, first));
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(1u, second));
	EXPECT_TRUE(first.IsNoteOn());
	EXPECT_EQ(0u, first.sampleOffset);
	EXPECT_TRUE(second.IsNoteOff());
	EXPECT_EQ(64u, second.sampleOffset);
}

TEST(LoopTakeMidiOverdub, MidiPunchCanOpenWithoutChangingAudioPunchState)
{
	auto take = MakeLoopTake("overdub-midi-only-punch");
	take->Overdub({ 0u }, "station", { 3u }, { "Keys" });

	take->EndMultiWrite(20u, true, Audible::AUDIOSOURCE_ADC);
	take->PunchIn(false, true);
	EXPECT_EQ(LoopTake::STATE_OVERDUBBING, take->TakeState());
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(20u, 3u, 60u, 100u), "Keys", 20u));
	take->EndMultiWrite(10u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOff(30u, 3u, 60u), "Keys", 30u));
	take->PunchOut(false, true);
	take->Play(0u, 64u, 0u);

	ASSERT_EQ(1u, take->GetMidiLoops().size());
	ASSERT_EQ(2u, take->GetMidiLoops()[0]->EventCount());
	MidiEvent first{};
	MidiEvent second{};
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, first));
	ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(1u, second));
	EXPECT_EQ(20u, first.sampleOffset);
	EXPECT_TRUE(first.IsNoteOn());
	EXPECT_EQ(30u, second.sampleOffset);
	EXPECT_TRUE(second.IsNoteOff());
}

TEST(LoopTakeMidiOverdub, PreviewIncludesSourceAndLiveEventsWhileRecording)
{
	auto source = MakeLoopTake("source-midi-preview");
	source->Record({}, "station", { 3u }, { "" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "", 100u));
	source->Play(0u, 100u, 0u);

	auto target = MakeLoopTake("target-midi-preview");
	target->Overdub({}, "station", { 3u }, { "" }, source);
	target->EndMultiWrite(20u, true, Audible::AUDIOSOURCE_ADC);
	target->PunchIn();
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(25u, 3u, 62u, 100u), "", 25u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(30u, 3u, 62u), "", 30u));
	target->EndMultiWrite(10u, true, Audible::AUDIOSOURCE_ADC);

	actions::JobAction update;
	update.JobActionType = actions::JobAction::JOB_UPDATELOOPS;
	target->OnAction(update);

	ASSERT_EQ(1u, target->GetMidiLoops().size());
	ASSERT_EQ(MidiLoopState::Recording, target->GetMidiLoops()[0]->State());
	ASSERT_EQ(4u, target->GetMidiLoops()[0]->EventCount());

	std::array<MidiEvent, 4u> events{};
	for (std::size_t i = 0u; i < events.size(); ++i)
		ASSERT_TRUE(target->GetMidiLoops()[0]->TryGetEvent(i, events[i]));

	EXPECT_TRUE(events[0].IsNoteOn());
	EXPECT_EQ(10u, events[0].sampleOffset);
	EXPECT_TRUE(events[1].IsNoteOff());
	EXPECT_EQ(20u, events[1].sampleOffset);
	EXPECT_TRUE(events[2].IsNoteOn());
	EXPECT_EQ(25u, events[2].sampleOffset);
	EXPECT_TRUE(events[3].IsNoteOff());
	EXPECT_EQ(30u, events[3].sampleOffset);
}

TEST(LoopTakeMidiOverdub, ZeroLengthPunchDoesNotEmitHeldGhostNote)
{
	auto take = MakeLoopTake("overdub-zero-length-punch");
	take->Overdub({}, "station", { 3u }, { "Keys" });

	take->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_FALSE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(100u, 3u, 60u, 100u), "Keys", 100u));
	take->PunchIn();
	take->PunchOut();
	take->Play(0u, 200u, 0u);

	ASSERT_EQ(0u, take->GetMidiLoops()[0]->EventCount());
}

TEST(LoopTakeMidiOverdub, PlayFinalizesCopiedSourceAndLivePunchEvents)
{
	auto source = MakeLoopTake("source-midi");
	source->Record({}, "station", { 3u }, { "" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "", 100u));
	source->Play(0u, 100u, 0u);

	auto target = MakeLoopTake("target-midi");
	target->Overdub({}, "station", { 3u }, { "" }, source);
	target->EndMultiWrite(20u, true, Audible::AUDIOSOURCE_ADC);
	target->PunchIn();
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(25u, 3u, 62u, 100u), "", 25u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(30u, 3u, 62u), "", 30u));
	target->EndMultiWrite(10u, true, Audible::AUDIOSOURCE_ADC);
	target->PunchOut();
	target->Play(0u, 100u, 0u);

	std::vector<MidiEvent> events;
	for (std::size_t i = 0u; i < target->GetMidiLoops()[0]->EventCount(); ++i)
	{
		MidiEvent ev{};
		if (target->GetMidiLoops()[0]->TryGetEvent(i, ev))
			events.push_back(ev);
	}

	ASSERT_EQ(6u, events.size());
	EXPECT_EQ(10u, events[0].sampleOffset);
	EXPECT_TRUE(events[0].IsNoteOn());
	EXPECT_EQ(20u, events[1].sampleOffset);
	EXPECT_TRUE(events[1].IsNoteOff());
	EXPECT_EQ(25u, events[2].sampleOffset);
	EXPECT_EQ(30u, events[3].sampleOffset);
	EXPECT_EQ(40u, events[4].sampleOffset);
	EXPECT_EQ(90u, events[5].sampleOffset);
}

TEST(LoopTakeMidiOverdub, PlayFinalizesPunchWindowRelativeToSourceLoopPhase)
{
	auto source = MakeLoopTake("source-midi-phase");
	source->Record({}, "station", { 3u }, { "" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "", 100u));
	source->Play(20u, 100u, 0u);

	auto target = MakeLoopTake("target-midi-phase");
	target->Overdub({}, "station", { 3u }, { "" }, source);
	target->PunchIn();
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(5u, 3u, 62u, 100u), "", 5u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(10u, 3u, 62u), "", 10u));
	target->PunchOut();
	target->Play(0u, 100u, 0u);

	std::vector<MidiEvent> events;
	for (std::size_t i = 0u; i < target->GetMidiLoops()[0]->EventCount(); ++i)
	{
		MidiEvent ev{};
		if (target->GetMidiLoops()[0]->TryGetEvent(i, ev))
			events.push_back(ev);
	}

	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(5u, events[0].sampleOffset);
	EXPECT_TRUE(events[0].IsNoteOn());
	EXPECT_EQ(62u, events[0].data1);
	EXPECT_EQ(10u, events[1].sampleOffset);
	EXPECT_TRUE(events[1].IsNoteOff());
	EXPECT_EQ(62u, events[1].data1);
	EXPECT_EQ(10u, events[2].sampleOffset);
	EXPECT_TRUE(events[2].IsNoteOn());
	EXPECT_EQ(60u, events[2].data1);
	EXPECT_EQ(90u, events[3].sampleOffset);
	EXPECT_TRUE(events[3].IsNoteOff());
	EXPECT_EQ(60u, events[3].data1);
}

TEST(LoopTakeMidiOverdub, FirstPlaybackBlockAnchorsToTriggerTimeline)
{
	auto source = MakeLoopTake("source-midi-phase-start");
	source->Record({}, "station", { 3u }, { "" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "", 100u));
	source->Play(20u, 100u, 0u);

	auto target = MakeLoopTake("target-midi-phase-start");
	target->Overdub({}, "station", { 3u }, { "" }, source);
	target->PunchIn();
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(5u, 3u, 62u, 100u), "", 5u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(10u, 3u, 62u), "", 10u));
	target->PunchOut();
	target->Play(0u, 100u, 0u);

	MidiLoopCapturingOutputSink sink;
	const auto firstBlockStart = 5000u;
	EXPECT_EQ(1u, target->ReadMidiBlock(firstBlockStart, 10u, sink));

	ASSERT_EQ(1u, sink.events.size());
	EXPECT_EQ(firstBlockStart + 5u, sink.events[0].event.sampleOffset);
	EXPECT_TRUE(sink.events[0].event.IsNoteOn());
	EXPECT_EQ(62u, sink.events[0].event.data1);
}

TEST(LoopTakeMidiOverdub, OverdubWithoutSourceMidiStillRecordsLivePunchOnly)
{
	auto target = MakeLoopTake("target-midi-live-only");
	target->Overdub({}, "station", { 3u }, { "" });
	target->PunchIn();
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOn(5u, 3u, 62u, 100u), "", 5u));
	target->EndMultiWrite(5u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(target->RecordMidiEvent(MidiEvent::MakeNoteOff(10u, 3u, 62u), "", 10u));
	target->PunchOut();
	target->Play(0u, 64u, 0u);

	ASSERT_EQ(2u, target->GetMidiLoops()[0]->EventCount());
	MidiEvent first{};
	MidiEvent second{};
	ASSERT_TRUE(target->GetMidiLoops()[0]->TryGetEvent(0u, first));
	ASSERT_TRUE(target->GetMidiLoops()[0]->TryGetEvent(1u, second));
	EXPECT_EQ(5u, first.sampleOffset);
	EXPECT_EQ(10u, second.sampleOffset);
}

TEST(LoopTakeMidiOverdub, NoPunchDoubleLengthOverdubIgnoresSourceAudioPlayPos)
{
	auto source = MakeLoopTake("source-midi-nopunch-2x");
	source->Record({}, "station", { 3u }, { "" });
	source->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOn(10u, 3u, 60u, 100u), "", 100u));
	EXPECT_TRUE(source->RecordMidiEvent(MidiEvent::MakeNoteOff(90u, 3u, 60u), "", 100u));
	// Non-zero audio playPos with zero MIDI error should not phase-shift copied MIDI.
	source->Play(520u, 100u, 0u, 0);

	auto target = MakeLoopTake("target-midi-nopunch-2x");
	target->Overdub({}, "station", { 3u }, { "" }, source);
	target->EndMultiWrite(200u, true, Audible::AUDIOSOURCE_ADC);
	target->Play(0u, 200u, 0u, 0);

	ASSERT_EQ(4u, target->GetMidiLoops()[0]->EventCount());
	std::array<MidiEvent, 4u> events{};
	for (std::size_t i = 0u; i < events.size(); ++i)
		ASSERT_TRUE(target->GetMidiLoops()[0]->TryGetEvent(i, events[i]));

	EXPECT_TRUE(events[0].IsNoteOn());
	EXPECT_EQ(10u, events[0].sampleOffset);
	EXPECT_TRUE(events[1].IsNoteOff());
	EXPECT_EQ(90u, events[1].sampleOffset);
	EXPECT_TRUE(events[2].IsNoteOn());
	EXPECT_EQ(110u, events[2].sampleOffset);
	EXPECT_TRUE(events[3].IsNoteOff());
	EXPECT_EQ(190u, events[3].sampleOffset);
}

TEST(LoopTakeMidiPlayback, MutedTakeDoesNotEmitMidiEvents)
{
	auto take = MakeLoopTake("muted-midi-take");
	take->Record({}, "station", { 0u }, { "" });
	EXPECT_TRUE(take->RecordMidiEvent(MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "", 0u));
	take->Play(0u, 64u, 0u);
	take->Mute();

	MidiLoopCapturingOutputSink sink;
	EXPECT_EQ(1u, take->ReadMidiBlock(123u, 64u, sink));
	EXPECT_TRUE(sink.events.empty());
}

TEST(LoopTakeMidiVisualization, MultipleChannelsShareOneSelectionRing)
{
	auto take = MakeLoopTake();
	take->Record({}, "station", { 2u, 3u });
	ASSERT_EQ(2u, take->GetMidiLoops().size());
	auto first = take->GetMidiLoops()[0]->Model();
	auto second = take->GetMidiLoops()[1]->Model();
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);

	const std::vector<midi::MidiNote> notes{ midi::MidiNote{ 0u, 240u, 2u, 60u, 100u } };
	first->UpdateModel(notes, 960u);
	second->UpdateModel(notes, 960u);
	EXPECT_EQ(2u, first->TotalInstanceCount());
	EXPECT_EQ(1u, second->TotalInstanceCount());
}

TEST(MidiLoopEdit, PublishesRawAndPlaybackTogetherAndRejectsStaleOrOverflow)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(10u, 2u, 60u, 99u),
		MidiEvent::MakeNoteOff(20u, 2u, 60u),
		MidiEvent{ 15u, 0xB2u, 7u, 45u, 0u }
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	ASSERT_TRUE(midi::MidiEditOperations::Create(edit, 30u, 10u, 2u, 64u));
	ASSERT_TRUE(loop.PublishEdit(edit));
	EXPECT_EQ(5u, loop.EventCount());
	MidiEvent event{};
	ASSERT_TRUE(loop.TryGetEvent(1u, event));
	EXPECT_EQ(0xB2u, event.status);
	EXPECT_FALSE(loop.PublishEdit(edit));
	MidiLoop::EditState overflow;
	ASSERT_TRUE(loop.SnapshotForEdit(overflow));
	overflow.EventCount = MidiLoop::DefaultCapacity + 1u;
	EXPECT_FALSE(loop.PublishEdit(overflow));
	MidiLoop::ExportState exported;
	ASSERT_TRUE(loop.SnapshotForExport(exported));
	EXPECT_EQ(5u, exported.EventCount);
	EXPECT_EQ(0xB2u, exported.Events[1].status);
}

TEST(MidiLoopEdit, QuantisationKeepsRawSourceAndFlushesHeldOnRevision)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(11u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOff(90u, 0u, 60u)
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.GrainSamps = 20u;
	quant.Fraction = MidiQuantisationFraction::Whole;
	loop.SetQuantisation(quant);
	MidiEvent raw{};
	MidiEvent playback{};
	ASSERT_TRUE(loop.TryGetEvent(0u, raw));
	ASSERT_TRUE(loop.TryGetPlaybackEvent(0u, playback));
	EXPECT_EQ(11u, raw.sampleOffset);
	EXPECT_EQ(20u, playback.sampleOffset);
	MidiLoopCapturingSink sink;
	loop.ReadBlock(20u, 1u, sink);
	ASSERT_TRUE(loop.HeldNotes().test(MidiLoop::NoteSlot(0u, 60u)));
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	edit.EventCount = 0u;
	ASSERT_TRUE(loop.PublishEdit(edit));
	sink.Clear();
	loop.ReadBlock(21u, 1u, sink);
	ASSERT_EQ(1u, sink.events.size());
	EXPECT_TRUE(sink.events[0].IsNoteOff());
	EXPECT_EQ(21u, sink.events[0].sampleOffset);
	EXPECT_TRUE(loop.HeldNotes().none());
	MidiLoop::ExportState exported;
	ASSERT_TRUE(loop.SnapshotForExport(exported));
	EXPECT_EQ(0u, exported.EventCount);
}

TEST(MidiLoopEdit, FinalCellUsesHeldToWrapSeamConvention)
{
	MidiLoop loop;
	loop.ReplaceRecordedEvents(nullptr, 0u, 16u);
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	ASSERT_TRUE(midi::MidiEditOperations::Create(edit, 15u, 1u, 1u, 72u));
	ASSERT_EQ(1u, edit.EventCount);
	ASSERT_TRUE(loop.PublishEdit(edit));
	MidiLoopCapturingSink sink;
	loop.ReadBlock(15u, 2u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_TRUE(sink.events[0].IsNoteOn());
	EXPECT_EQ(15u, sink.events[0].sampleOffset);
	EXPECT_TRUE(sink.events[1].IsNoteOff());
	EXPECT_EQ(16u, sink.events[1].sampleOffset);
}

TEST(MidiLoopEdit, RejectsChangedNonNoteWithoutPublishing)
{
	MidiLoop loop;
	const std::array events{ MidiEvent{ 5u, 0xB0u, 7u, 42u, 0u } };
	loop.ReplaceRecordedEvents(events.data(), events.size(), 16u);
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	edit.Events[0].data2 = 99u;
	EXPECT_FALSE(loop.PublishEdit(edit));
	MidiEvent original{};
	ASSERT_TRUE(loop.TryGetEvent(0u, original));
	EXPECT_EQ(42u, original.data2);
}

TEST(MidiLoopEdit, QuantisedCellSplitPreservesAdjacentCoverage)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(7u, 1u, 60u, 87u),
		MidiEvent::MakeNoteOff(67u, 1u, 60u)
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.Fraction = MidiQuantisationFraction::Whole;
	quant.RemoteIntervalSamps = 100u;
	quant.RemoteBpi = 5u;
	quant.RemoteOriginSamps = 7;
	ASSERT_TRUE(loop.SetQuantisation(quant));
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	ASSERT_TRUE(midi::MidiEditOperations::SetCell(edit, 27u, 47u, 60u, 1u,
		false, quant, 0u));
	ASSERT_TRUE(loop.PublishEdit(edit));
	std::array<MidiEvent, 4u> playback{};
	for (std::size_t i = 0u; i < playback.size(); ++i)
		ASSERT_TRUE(loop.TryGetPlaybackEvent(i, playback[i]));
	const auto spans = midi::MidiNote::ExtractSpans(playback.data(), playback.size(), 100u);
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(7u, spans[0].StartSample);
	EXPECT_EQ(20u, spans[0].DurationSamples);
	EXPECT_EQ(47u, spans[1].StartSample);
	EXPECT_EQ(20u, spans[1].DurationSamples);
}

TEST(MidiLoopEdit, QuantisedAddRejectsNonBoundaryWithPhaseOffset)
{
	MidiLoop::EditState edit;
	edit.LoopLengthSamps = 100u;
	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.GrainSamps = 20u;
	quant.Fraction = MidiQuantisationFraction::Whole;
	quant.PhaseOffsetSamps = 3;
	EXPECT_FALSE(midi::MidiEditOperations::SetCell(edit, 20u, 40u,
		60u, 0u, true, quant, 0u));
	EXPECT_EQ(0u, edit.EventCount);
	EXPECT_TRUE(midi::MidiEditOperations::SetCell(edit, 23u, 43u,
		60u, 0u, true, quant, 0u));
}

TEST(MidiLoopEdit, QuantisedAddInvertsLargePhaseOffset)
{
	MidiLoop loop;
	loop.ReplaceRecordedEvents(nullptr, 0u, 100u);
	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.GrainSamps = 20u;
	quant.Fraction = MidiQuantisationFraction::Whole;
	quant.PhaseOffsetSamps = 12;
	ASSERT_TRUE(loop.SetQuantisation(quant));
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	EXPECT_FALSE(midi::MidiEditOperations::SetCell(edit, 0u, 12u,
		60u, 0u, true, quant, 0u));
	EXPECT_EQ(0u, edit.EventCount);
	ASSERT_TRUE(midi::MidiEditOperations::SetCell(edit, 12u, 32u,
		60u, 0u, true, quant, 0u));
	ASSERT_TRUE(loop.PublishEdit(edit));
	MidiEvent raw{}, displayed{};
	ASSERT_TRUE(loop.TryGetEvent(0u, raw));
	ASSERT_TRUE(loop.TryGetPlaybackEvent(0u, displayed));
	EXPECT_EQ(0u, raw.sampleOffset);
	EXPECT_EQ(12u, displayed.sampleOffset);
}

TEST(MidiLoopEdit, QuantisedFinalCellUsesHeldSeamWhenShifted)
{
	MidiLoop loop;
	loop.ReplaceRecordedEvents(nullptr, 0u, 100u);
	MidiQuantisationSettings quant;
	quant.Enabled = true;
	quant.GrainSamps = 20u;
	quant.Fraction = MidiQuantisationFraction::Whole;
	quant.PhaseOffsetSamps = 12;
	ASSERT_TRUE(loop.SetQuantisation(quant));
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	ASSERT_TRUE(midi::MidiEditOperations::SetCell(edit, 92u, 100u,
		60u, 0u, true, quant, 0u));
	ASSERT_TRUE(loop.PublishEdit(edit));
	MidiEvent raw{}, displayed{};
	ASSERT_TRUE(loop.TryGetEvent(0u, raw));
	ASSERT_TRUE(loop.TryGetPlaybackEvent(0u, displayed));
	EXPECT_EQ(80u, raw.sampleOffset);
	EXPECT_EQ(92u, displayed.sampleOffset);
	MidiLoopCapturingSink sink;
	loop.ReadBlock(92u, 9u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(100u, sink.events[1].sampleOffset);
	MidiLoop::EditState erase;
	ASSERT_TRUE(loop.SnapshotForEdit(erase));
	ASSERT_TRUE(midi::MidiEditOperations::SetCell(erase, 92u, 100u,
		60u, 0u, false, quant, 0u));
	ASSERT_TRUE(loop.PublishEdit(erase));
	EXPECT_EQ(0u, loop.EventCount());
}

TEST(MidiLoopEdit, PreservesFinalisedCaptureTail)
{
	MidiLoop loop;
	loop.StartRecord();
	ASSERT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u)));
	ASSERT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOff(120u, 0u, 60u)));
	loop.EndRecord(100u);
	MidiLoop::EditState edit;
	ASSERT_TRUE(loop.SnapshotForEdit(edit));
	ASSERT_TRUE(midi::MidiEditOperations::Create(edit, 30u, 10u, 0u, 64u));
	ASSERT_TRUE(loop.PublishEdit(edit));
	EXPECT_EQ(4u, loop.EventCount());
	MidiEvent tail{};
	ASSERT_TRUE(loop.TryGetEvent(3u, tail));
	EXPECT_EQ(120u, tail.sampleOffset);
	MidiLoop::ExportState exported;
	ASSERT_TRUE(loop.SnapshotForExport(exported));
	EXPECT_EQ(3u, exported.EventCount);
}

TEST(MidiLoopEdit, CallbackKeepsOneImmutableRevisionAcrossPublication)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOff(20u, 0u, 60u)
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	class PublishingSink final : public IMidiSink
	{
	public:
		MidiLoop& Loop;
		std::array<MidiEvent, 4u> Events{};
		std::size_t Count = 0u;
		bool Published = false;
		explicit PublishingSink(MidiLoop& loop) : Loop(loop) {}
		void OnEvent(const MidiEvent& event) noexcept override
		{
			Events[Count++] = event;
			if (Published) return;
			MidiLoop::EditState edit;
			Published = Loop.SnapshotForEdit(edit)
				&& midi::MidiEditOperations::MoveOrTrim(edit, 0u, 10u, 30u, 60u)
				&& Loop.PublishEdit(edit);
		}
	} sink(loop);
	loop.ReadBlock(0u, 40u, sink);
	ASSERT_TRUE(sink.Published);
	ASSERT_EQ(2u, sink.Count);
	EXPECT_EQ(20u, sink.Events[1].sampleOffset);
	MidiLoopCapturingSink later;
	loop.ReadBlock(100u, 40u, later);
	ASSERT_EQ(2u, later.events.size());
	EXPECT_EQ(130u, later.events[1].sampleOffset);
}

TEST(MidiLoopEdit, DitchFlushUsesLastSampleInsideBlock)
{
	MidiLoop loop;
	const std::array events{ MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u) };
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	class DitchSink final : public IMidiSink
	{
	public:
		MidiLoop& Loop;
		std::array<MidiEvent, 2u> Events{};
		std::size_t Count = 0u;
		explicit DitchSink(MidiLoop& loop) : Loop(loop) {}
		void OnEvent(const MidiEvent& event) noexcept override
		{
			Events[Count++] = event;
			if (event.IsNoteOn()) Loop.RequestHeldFlush();
		}
	} sink(loop);
	loop.ReadBlock(10u, 10u, sink);
	ASSERT_EQ(2u, sink.Count);
	EXPECT_TRUE(sink.Events[1].IsNoteOff());
	EXPECT_EQ(19u, sink.Events[1].sampleOffset);
}

TEST(MidiLoopEdit, DitchFlushRequestDoesNotTruncateLaterNote)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOn(30u, 0u, 62u, 90u),
		MidiEvent::MakeNoteOff(70u, 0u, 62u)
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	MidiLoopCapturingSink sink;
	loop.RequestHeldFlush();
	loop.ReadBlock(10u, 10u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_TRUE(sink.events[0].IsNoteOn());
	EXPECT_TRUE(sink.events[1].IsNoteOff());
	EXPECT_EQ(19u, sink.events[1].sampleOffset);

	sink.Clear();
	loop.ReadBlock(30u, 10u, sink);
	ASSERT_EQ(1u, sink.events.size());
	EXPECT_TRUE(sink.events[0].IsNoteOn());
	EXPECT_EQ(30u, sink.events[0].sampleOffset);
	loop.ReadBlock(40u, 10u, sink);
	EXPECT_EQ(1u, sink.events.size());
	EXPECT_TRUE(loop.HeldNotes().test(MidiLoop::NoteSlot(0u, 62u)));
	loop.ReadBlock(70u, 1u, sink);
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_TRUE(sink.events[1].IsNoteOff());
	EXPECT_EQ(70u, sink.events[1].sampleOffset);
}

TEST(MidiLoopEdit, SnapshotPoolRejectsAndRetriesWithoutReusingActiveReader)
{
	MidiLoop loop;
	const std::array events{
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOff(20u, 0u, 60u)
	};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	class PoolSink final : public IMidiSink
	{
	public:
		MidiLoop& Loop;
		std::array<MidiEvent, 2u> Events{};
		std::size_t Count = 0u;
		std::array<bool, 3u> Results{};
		explicit PoolSink(MidiLoop& loop) : Loop(loop) {}
		void OnEvent(const MidiEvent& event) noexcept override
		{
			Events[Count++] = event;
			if (Count != 1u) return;
			for (std::uint32_t i = 0u; i < Results.size(); ++i)
			{
				MidiQuantisationSettings settings;
				settings.Enabled = true;
				settings.GrainSamps = 20u + i;
				settings.Fraction = MidiQuantisationFraction::Whole;
				Results[i] = Loop.SetQuantisation(settings);
			}
		}
	} sink(loop);
	loop.ReadBlock(0u, 40u, sink);
	EXPECT_EQ((std::array<bool, 3u>{ true, true, false }), sink.Results);
	ASSERT_EQ(2u, sink.Count);
	EXPECT_EQ(20u, sink.Events[1].sampleOffset);
	MidiQuantisationSettings retry;
	retry.Enabled = true;
	retry.GrainSamps = 22u;
	retry.Fraction = MidiQuantisationFraction::Whole;
	EXPECT_TRUE(loop.SetQuantisation(retry));
}

TEST(MidiLoopEdit, CompletionWaitsForReaderBeforeReplacingSource)
{
	MidiLoop loop;
	const std::array original{
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u),
		MidiEvent::MakeNoteOff(20u, 0u, 60u)
	};
	loop.ReplaceRecordedEvents(original.data(), original.size(), 100u);
	std::atomic<bool> readerEntered{ false };
	std::atomic<bool> releaseReader{ false };
	class HoldingSink final : public IMidiSink
	{
	public:
		std::atomic<bool>& Entered;
		std::atomic<bool>& Release;
		HoldingSink(std::atomic<bool>& entered, std::atomic<bool>& release)
			: Entered(entered), Release(release) {}
		void OnEvent(const MidiEvent&) noexcept override
		{
			Entered.store(true, std::memory_order_release);
			while (!Release.load(std::memory_order_acquire))
				std::this_thread::yield();
		}
	} sink(readerEntered, releaseReader);
	std::thread reader([&] { loop.ReadBlock(0u, 40u, sink); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!readerEntered.load(std::memory_order_acquire)
		&& std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool entered = readerEntered.load(std::memory_order_acquire);
	if (entered)
	{
		MidiQuantisationSettings settings;
		settings.Enabled = true;
		settings.Fraction = MidiQuantisationFraction::Whole;
		settings.GrainSamps = 20u;
		EXPECT_TRUE(loop.SetQuantisation(settings));
		settings.GrainSamps = 25u;
		EXPECT_TRUE(loop.SetQuantisation(settings));

		const std::array replacement{
			MidiEvent::MakeNoteOn(30u, 0u, 64u, 90u),
			MidiEvent::MakeNoteOff(60u, 0u, 64u)
		};
		const auto beforeRevision = loop.Revision();
		std::atomic<bool> completionStarted{ false };
		std::atomic<bool> completionFinished{ false };
		std::thread completion([&]
		{
			completionStarted.store(true, std::memory_order_release);
			loop.ReplaceRecordedEvents(replacement.data(), replacement.size(), 100u);
			completionFinished.store(true, std::memory_order_release);
		});
		while (!completionStarted.load(std::memory_order_acquire))
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		EXPECT_FALSE(completionFinished.load(std::memory_order_acquire));
		EXPECT_EQ(beforeRevision, loop.Revision());
		MidiEvent source{};
		EXPECT_TRUE(loop.TryGetEvent(0u, source));
		EXPECT_EQ(60u, source.data1);
		releaseReader.store(true, std::memory_order_release);
		reader.join();
		completion.join();
		EXPECT_TRUE(completionFinished.load(std::memory_order_acquire));
		EXPECT_EQ(beforeRevision + 1u, loop.Revision());
		EXPECT_TRUE(loop.TryGetEvent(0u, source));
		EXPECT_EQ(64u, source.data1);
		EXPECT_TRUE(loop.TryGetPlaybackEvent(0u, source));
		EXPECT_EQ(64u, source.data1);

		// Record finalisation uses the same bounded publication contract.
		readerEntered.store(false, std::memory_order_release);
		releaseReader.store(false, std::memory_order_release);
		reader = std::thread([&] { loop.ReadBlock(0u, 40u, sink); });
		const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!readerEntered.load(std::memory_order_acquire)
			&& std::chrono::steady_clock::now() < secondDeadline)
			std::this_thread::yield();
		const bool secondEntered = readerEntered.load(std::memory_order_acquire);
		if (secondEntered)
		{
			bool poolExhausted = false;
			for (std::uint32_t grain = 20u; grain < 25u; ++grain)
			{
				settings.GrainSamps = grain;
				if (!loop.SetQuantisation(settings))
				{
					poolExhausted = true;
					break;
				}
			}
			EXPECT_TRUE(poolExhausted);
			loop.StartRecord();
			EXPECT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOn(40u, 0u, 67u, 90u)));
			EXPECT_TRUE(loop.RecordEvent(MidiEvent::MakeNoteOff(80u, 0u, 67u)));
			const auto recordRevision = loop.Revision();
			completionStarted.store(false, std::memory_order_release);
			completionFinished.store(false, std::memory_order_release);
			completion = std::thread([&]
			{
				completionStarted.store(true, std::memory_order_release);
				loop.EndRecord(100u);
				completionFinished.store(true, std::memory_order_release);
			});
			while (!completionStarted.load(std::memory_order_acquire))
				std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			EXPECT_FALSE(completionFinished.load(std::memory_order_acquire));
			EXPECT_EQ(MidiLoopState::Recording, loop.State());
			EXPECT_EQ(0u, loop.CompletedLengthForEditor());
			EXPECT_EQ(recordRevision, loop.Revision());
		}
		releaseReader.store(true, std::memory_order_release);
		reader.join();
		if (secondEntered)
		{
			completion.join();
			EXPECT_EQ(MidiLoopState::Playing, loop.State());
			EXPECT_EQ(100u, loop.CompletedLengthForEditor());
			EXPECT_EQ(67u, loop.TryGetPlaybackEvent(0u, source) ? source.data1 : 0u);
		}
		else
			ADD_FAILURE() << "Second playback reader did not enter the test sink";
	}
	else
	{
		releaseReader.store(true, std::memory_order_release);
		reader.join();
		FAIL() << "Playback reader did not enter the test sink";
	}
}

TEST(MidiLoopEdit, ZeroLengthCompletionClearsPriorPlayback)
{
	MidiLoop loop;
	const std::array events{ MidiEvent::MakeNoteOn(10u, 0u, 60u, 90u) };
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	loop.ReplaceRecordedEvents(nullptr, 0u, 0u);
	EXPECT_EQ(0u, loop.EventCount());
	EXPECT_EQ(0u, loop.LoopLengthSamps());
	EXPECT_EQ(0u, loop.CompletedLengthForEditor());
	MidiLoopCapturingSink sink;
	loop.ReadBlock(10u, 1u, sink);
	EXPECT_TRUE(sink.events.empty());
}

TEST(MidiLoopEdit, FinalCellNeedsOnlyOneFreeEventSlot)
{
	MidiLoop::EditState edit;
	edit.LoopLengthSamps = 100u;
	edit.EventCount = MidiLoop::DefaultCapacity - 1u;
	EXPECT_TRUE(midi::MidiEditOperations::Create(edit, 99u, 1u, 0u, 60u));
	EXPECT_EQ(MidiLoop::DefaultCapacity, edit.EventCount);
	EXPECT_FALSE(midi::MidiEditOperations::Create(edit, 98u, 1u, 0u, 61u));
}

TEST(MidiLoopEdit, TwoGestureUndoRedoFollowsMonotonicRevisions)
{
	auto take = MakeLoopTake("two-gesture-midi-edit");
	take->Record({}, "station", { 0u }, { "" });
	take->Play(0u, 100u, 0u);
	auto loop = take->GetMidiLoops().at(0u);
	actions::ActionUndoHistory history;
	auto cursor = std::make_shared<actions::MidiEditRevisionCursor>();
	for (std::uint8_t pitch : { 60u, 64u })
	{
		MidiLoop::EditState before;
		ASSERT_TRUE(loop->SnapshotForEdit(before));
		auto after = before;
		ASSERT_TRUE(midi::MidiEditOperations::Create(after, pitch, 10u, 0u, pitch));
		std::uint64_t accepted = 0u;
		ASSERT_TRUE(take->PublishMidiEdit(loop, after, &accepted));
		history.Add(std::make_shared<actions::MidiLoopEditUndo>(take, loop,
			before, after, accepted, cursor));
	}
	ASSERT_EQ(4u, loop->EventCount());
	EXPECT_TRUE(history.Undo());
	EXPECT_TRUE(history.Undo());
	EXPECT_EQ(0u, loop->EventCount());
	EXPECT_TRUE(history.Redo());
	EXPECT_TRUE(history.Redo());
	EXPECT_EQ(4u, loop->EventCount());
}

TEST(MidiLoopEdit, EntirePaintGesturePublishesOnceAndUndoRedoPreservesExactTiming)
{
	auto take = MakeLoopTake("paint-midi-edit");
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	take->Record({}, "station", {0u}, {""});
	take->Play(0u, 100u, 0u);
	auto settings = take->ResolvedMidiQuantisation();
	settings.Enabled = true; settings.GrainSamps = 10u;
	settings.Fraction = MidiQuantisationFraction::Whole; settings.PhaseOffsetSamps = 1;
	take->SetMidiQuantisation(settings);
	auto loop = take->GetMidiLoops().at(0u);
	// Apply the job-side quantisation publication before taking the UI snapshot.
	ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), take->MidiQuantisationTransportStartSamps()));
	MidiLoop::EditState before;
	ASSERT_TRUE(loop->SnapshotForEdit(before));
	midi::MidiGridGesture paint;
	ASSERT_TRUE(paint.Begin(before, {0u, 60u, 0.0}, 0u));
	ASSERT_EQ(midi::MidiGridGesture::Kind::Paint, paint.Mode());
	ASSERT_TRUE(paint.Update({35u, 60u, 0.35}));
	EXPECT_EQ(0u, loop->EventCount()); EXPECT_EQ(before.Revision, loop->Revision());
	std::uint64_t accepted = 0;
	ASSERT_TRUE(take->PublishMidiEdit(loop, paint.Working(), &accepted));
	EXPECT_EQ(before.Revision + 1u, accepted);
	actions::ActionUndoHistory history;
	auto cursor = std::make_shared<actions::MidiEditRevisionCursor>();
	history.Add(std::make_shared<actions::MidiLoopEditUndo>(take, loop, before,
		paint.Working(), accepted, cursor));
	ASSERT_TRUE(history.Undo()); EXPECT_EQ(0u, loop->EventCount());
	EXPECT_FALSE(history.Undo()); // Whole paint is exactly one undo entry.
	ASSERT_TRUE(history.Redo());
	MidiLoop::EditState after;
	ASSERT_TRUE(loop->SnapshotForEdit(after));
	EXPECT_EQ(paint.Working().EventCount, after.EventCount);
	for (std::size_t i = 0; i < after.EventCount; ++i)
	{
		EXPECT_EQ(paint.Working().Events[i].sampleOffset, after.Events[i].sampleOffset);
		EXPECT_EQ(MidiEvent::ExactTiming, after.Events[i].flags);
	}
	midi::MidiGridGesture erase;
	ASSERT_TRUE(erase.Begin(after, {0u, 60u, 0.0}, 0u));
	ASSERT_TRUE(erase.Update({35u, 60u, 0.35}));
	ASSERT_TRUE(take->PublishMidiEdit(loop, erase.Working()));
	EXPECT_EQ(0u, loop->EventCount());
	EXPECT_FALSE(take->PublishMidiEdit(loop, after)); // stale publication
}

TEST(MidiLoopEdit, PartialDisplayedCellPaintPreservesCapturedNoteDespiteRawOverlap)
{
	MidiLoop loop;
	const std::array events{MidiEvent::MakeNoteOn(12u, 2u, 60u, 87u),
		MidiEvent::MakeNoteOff(28u, 2u, 60u)};
	loop.ReplaceRecordedEvents(events.data(), events.size(), 100u);
	MidiQuantisationSettings settings;
	settings.Enabled = true; settings.GrainSamps = 10u; settings.Fraction = MidiQuantisationFraction::Whole;
	ASSERT_TRUE(loop.SetQuantisation(settings));
	MidiLoop::EditState before;
	ASSERT_TRUE(loop.SnapshotForEdit(before));
	midi::MidiGridGesture add;
	ASSERT_TRUE(add.Begin(before, {27u, 60u, 0.27}, 2u));
	ASSERT_TRUE(loop.PublishEdit(add.Working()));
	std::vector<MidiEvent> playback(loop.EventCount());
	for (std::size_t i = 0; i < playback.size(); ++i) ASSERT_TRUE(loop.TryGetPlaybackEvent(i, playback[i]));
	const auto rendered = midi::MidiNote::ExtractSpans(playback.data(), playback.size(), 100u);
	ASSERT_EQ(2u, rendered.size());
	EXPECT_EQ(10u, rendered[0].StartSample); EXPECT_EQ(16u, rendered[0].DurationSamples);
	EXPECT_EQ(87u, rendered[0].Velocity);
	EXPECT_EQ(26u, rendered[1].StartSample); EXPECT_EQ(4u, rendered[1].DurationSamples);
	MidiLoop::ExportState exported;
	ASSERT_TRUE(loop.SnapshotForExport(exported));
	EXPECT_EQ(4u, exported.EventCount);
}

TEST(MidiLoopEdit, VelocityAndSnappedMoveEachPublishOneUndoableRevision)
{
	auto take = MakeLoopTake("pointer-midi-edit");
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
	take->Record({}, "station", {0u}, {""});
	take->Play(0u, 100u, 0u);
	auto settings = take->ResolvedMidiQuantisation();
	settings.Enabled = true; settings.GrainSamps = 10u;
	settings.Fraction = MidiQuantisationFraction::Whole;
	take->SetMidiQuantisation(settings);
	auto loop = take->GetMidiLoops().at(0u);
	const std::array events{MidiEvent::MakeNoteOn(10u, 2u, 60u, 80u),
		MidiEvent::MakeNoteOff(30u, 2u, 60u)};
	loop->ReplaceRecordedEvents(events.data(), events.size(), 100u);
	ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), take->MidiQuantisationTransportStartSamps()));
	auto cursor = std::make_shared<actions::MidiEditRevisionCursor>();
	for (bool velocity : {true, false})
	{
		actions::ActionUndoHistory history;
		MidiLoop::EditState before;
		ASSERT_TRUE(loop->SnapshotForEdit(before));
		midi::MidiGridGesture gesture;
		ASSERT_TRUE(velocity ? gesture.BeginVelocity(before, {15u, 60u, 0.15})
			: gesture.BeginSnappedMove(before, {15u, 60u, 0.15}));
		ASSERT_TRUE(velocity ? gesture.UpdateRelative(40.0) : gesture.Update({45u, 64u, 0.45}));
		EXPECT_EQ(before.Revision, loop->Revision());
		std::uint64_t accepted = 0u;
		ASSERT_TRUE(take->PublishMidiEdit(loop, gesture.Working(), &accepted));
		EXPECT_EQ(before.Revision + 1u, accepted);
		history.Add(std::make_shared<actions::MidiLoopEditUndo>(take, loop,
			before, gesture.Working(), accepted, cursor));
		ASSERT_TRUE(history.Undo());
		EXPECT_FALSE(history.Undo());
		MidiLoop::EditState restored;
		ASSERT_TRUE(loop->SnapshotForEdit(restored));
		ASSERT_EQ(before.EventCount, restored.EventCount);
		for (std::size_t i = 0; i < before.EventCount; ++i)
		{
			EXPECT_EQ(before.Events[i].sampleOffset, restored.Events[i].sampleOffset);
			EXPECT_EQ(before.Events[i].data1, restored.Events[i].data1);
			EXPECT_EQ(before.Events[i].data2, restored.Events[i].data2);
			EXPECT_EQ(before.Events[i].flags, restored.Events[i].flags);
		}
		ASSERT_TRUE(history.Redo());
		EXPECT_FALSE(history.Redo());
	}
	MidiEvent on;
	ASSERT_TRUE(loop->TryGetEvent(0u, on));
	EXPECT_EQ(40u, on.sampleOffset);
	EXPECT_EQ(64u, on.data1);
	EXPECT_GT(on.data2, 80u);
}

class MidiPointerEditorTest : public testing::Test
{
protected:
	void SetUp() override
	{
		station = MakeStation("pointer-editor");
		station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
		take = MakeLoopTake("pointer-editor-take");
		take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
		take->Record({}, "station", {0u}, {""});
		take->Play(0u, 100u, 0u);
		auto settings = take->ResolvedMidiQuantisation();
		settings.Enabled = true; settings.GrainSamps = 10u;
		settings.Fraction = MidiQuantisationFraction::Whole;
		take->SetMidiQuantisation(settings);
		loop = take->GetMidiLoops().at(0u);
		const std::array events{MidiEvent::MakeNoteOn(10u, 0u, 60u, 80u), MidiEvent::MakeNoteOff(30u, 0u, 60u)};
		loop->ReplaceRecordedEvents(events.data(), events.size(), 100u);
		ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), take->MidiQuantisationTransportStartSamps()));
		station->AddTake(take);
		stations.push_back(station);
		station->SetModelPosition({0,0,0}); station->SetModelScale(1);
		take->SetModelPosition({0,0,0}); take->SetModelScale(1);
		ASSERT_TRUE(loop->Model());
		loop->Model()->SetModelPosition({0,0,0}); loop->Model()->SetModelScale(1);
		loop->Model()->UpdateModel({midi::MidiNote{10u, 20u, 0u, 60u, 80u, 0u}}, 100u);
		take->Select(); loop->Model()->Select();
		vp = glm::ortho(-70.0f, 70.0f, -70.0f, 70.0f, 1.0f, 1000.0f)
			* glm::lookAt(glm::vec3(0,400,0), glm::vec3(0), glm::vec3(0,0,-1));
		editor = std::make_unique<midi::LoopGridEditor>(midi::LoopGridEditor::Host{
			camera, history, mutex, stations, {}, [this](float) { return vp; }, {},
			[this](int button, utils::Position2d) { relativeButton = button; return allowRelative; },
			[this](int) { relativeButton = -1; ++endCount; }}, utils::Size2d{800u,800u});
		ASSERT_TRUE(editor->Open(take, nullptr, loop));
		for (int frame = 0; frame < 10; ++frame)
		{ camera.TickBackgroundDrag(0.05f); editor->Tick(0.05f); }
		ASSERT_TRUE(editor->IsReady());
		loop->Model()->SetEditorPitchRange(48,24);
	}
	utils::Position2d Pixel(double u, int pitch)
	{
		const auto v = (pitch + 0.5 - loop->Model()->EditorBottomPitch()) / loop->Model()->EditorVisibleRows();
		return *graphics::LoopGridProjection::Project(vp, glm::mat4(1.0f),
			{static_cast<float>((u-0.5)*100), 2.0f, static_cast<float>((0.5-v)*78)},800,800);
	}
	void Button(int index, bool down, utils::Position2d pixel, unsigned int buttons, bool control=false)
	{
		TouchAction action;
		action.Index=index; action.State=down ? TouchAction::TOUCH_DOWN : TouchAction::TOUCH_UP;
		action.Position=pixel; action.MouseButtonsDown=buttons;
		action.Modifiers=control ? base::Action::MODIFIER_CTRL : base::Action::MODIFIER_NONE;
		editor->OnAction(action);
	}
	void Relative(int dx, int dy, unsigned int buttons)
	{
		TouchMoveAction move; move.IsRelative=true; move.RelativeDelta={dx,dy};
		move.MouseButtonsDown=buttons; editor->OnAction(move);
	}
	std::shared_ptr<Station> station;
	std::shared_ptr<LoopTake> take;
	std::shared_ptr<MidiLoop> loop;
	std::vector<std::shared_ptr<Station>> stations;
	graphics::Camera camera{graphics::CameraParams(base::MoveableParams(),0u)};
	actions::ActionUndoHistory history;
	std::mutex mutex;
	glm::mat4 vp{1};
	std::unique_ptr<midi::LoopGridEditor> editor;
	int relativeButton=-1, endCount=0;
	bool allowRelative=true;
};

TEST_F(MidiPointerEditorTest, RightNoteOwnsReleaseAndModifierChangesCannotSwitchMode)
{
	const auto pixel=Pixel(0.15,60);
	const auto revision=loop->Revision();
	Button(2,true,pixel,4u,true);
	ASSERT_TRUE(editor->OwnsPointer()); EXPECT_EQ(2,relativeButton);
	Relative(500,40,4u);
	Button(0,true,Pixel(0.7,65),5u);
	Button(0,false,Pixel(0.7,65),4u);
	EXPECT_TRUE(editor->OwnsPointer()); EXPECT_EQ(revision,loop->Revision());
	Button(2,false,pixel,0u);
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	EXPECT_EQ(revision+1u,loop->Revision());
	MidiEvent on; ASSERT_TRUE(loop->TryGetEvent(0u,on)); EXPECT_EQ(90u,on.data2);
	EXPECT_EQ(10u,on.sampleOffset); EXPECT_EQ(60u,on.data1);
	EXPECT_EQ(-1,loop->Model()->EditorHeldInstance());
	EXPECT_TRUE(history.Undo()); EXPECT_FALSE(history.Undo()); EXPECT_TRUE(history.Redo());
}

TEST_F(MidiPointerEditorTest, ControlEmptyViewAndRightEmptyOrbitNeverPublish)
{
	const auto pixel=Pixel(0.7,65);
	const auto revision=loop->Revision();
	Button(0,true,pixel,1u,true);
	ASSERT_TRUE(editor->OwnsPointer()); EXPECT_EQ(0,relativeButton);
	Relative(16,24,1u); // No Ctrl: gesture remains latched.
	EXPECT_EQ(26,loop->Model()->EditorVisibleRows());
	Button(2,true,pixel,5u); Button(2,false,pixel,1u);
	EXPECT_TRUE(editor->OwnsPointer());
	Button(0,false,pixel,0u);
	EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
	Button(2,true,pixel,4u);
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	Button(2,false,pixel,0u); EXPECT_EQ(revision,loop->Revision());
}

TEST_F(MidiPointerEditorTest, FailedAnchorAndCaptureLossLeaveSourceAndUndoUnchanged)
{
	const auto pixel=Pixel(0.15,60); const auto revision=loop->Revision();
	allowRelative=false; Button(2,true,pixel,4u);
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(revision,loop->Revision());
	allowRelative=true; Button(2,true,pixel,4u); Relative(0,40,4u);
	editor->CancelInput();
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	EXPECT_EQ(-1,loop->Model()->EditorHeldInstance());
	Button(2,false,pixel,0u); EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
}

TEST_F(MidiPointerEditorTest, ControlNoteMovesAbsolutelyWhilePlainLeftStillPaints)
{
	const auto note=Pixel(0.15,60), destination=Pixel(0.45,64);
	Button(0,true,note,1u,true);
	ASSERT_TRUE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	TouchMoveAction move; move.Position=destination; move.MouseButtonsDown=1u;
	editor->OnAction(move); Button(0,false,destination,0u);
	MidiEvent on; ASSERT_TRUE(loop->TryGetEvent(0u,on));
	EXPECT_EQ(40u,on.sampleOffset); EXPECT_EQ(64u,on.data1); EXPECT_EQ(80u,on.data2);
	ASSERT_TRUE(history.Undo());
	const auto empty=Pixel(0.75,66);
	Button(0,true,empty,1u); Button(0,false,empty,0u);
	EXPECT_EQ(4u,loop->EventCount());
}

TEST_F(MidiPointerEditorTest, NoteClickAndReturnToStartingVelocityAreNoOps)
{
	const auto pixel=Pixel(0.15,60); const auto revision=loop->Revision();
	Button(2,true,pixel,4u); Button(2,false,pixel,0u);
	EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
	Button(2,true,pixel,4u); Relative(0,40,4u); Relative(0,-40,4u);
	Button(2,false,pixel,0u);
	EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
}

TEST_F(MidiPointerEditorTest, ModelReplacementAndResizeCancelCapturedEdit)
{
	const auto pixel=Pixel(0.15,60); const auto revision=loop->Revision();
	Button(2,true,pixel,4u); Relative(0,40,4u);
	loop->Model()->UpdateModel({midi::MidiNote{10u,20u,0u,60u,80u,0u}},100u);
	Button(2,false,pixel,0u);
	EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(editor->OwnsPointer());
	Button(2,true,pixel,4u); Relative(0,40,4u);
	editor->SetSize({640u,480u});
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
}

TEST_F(MidiPointerEditorTest, ControlNoteWithoutGridRejectsInsteadOfPainting)
{
	auto q=take->ResolvedMidiQuantisation(); q.Enabled=false; take->SetMidiQuantisation(q);
	ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(),take->MidiQuantisationTransportStartSamps()));
	const auto revision=loop->Revision(); const auto count=loop->EventCount();
	const auto pixel=Pixel(0.15,60);
	Button(0,true,pixel,1u,true); Button(0,false,pixel,0u);
	EXPECT_FALSE(editor->OwnsPointer()); EXPECT_EQ(-1,relativeButton);
	EXPECT_EQ(count,loop->EventCount()); EXPECT_EQ(revision,loop->Revision()); EXPECT_FALSE(history.Undo());
}
