
#include "gtest/gtest.h"
#include <thread>
#include <atomic>
#include <array>
#include "actions/JobAction.h"
#include "engine/Loop.h"
#include "vst/ReservedMidiEvents.h"
#include "engine/LoopTake.h"

using actions::JobAction;
using engine::Loop;
using engine::LoopParams;
using engine::LoopTake;
using engine::LoopTakeParams;
using audio::MergeMixBehaviourParams;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::shared_ptr<LoopTake> MakeTake(const std::string& id = "take-vst")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<LoopTake>(params, mixerParams);
}

static std::shared_ptr<Loop> MakeLoop()
{
	LoopParams params;
	params.Id = "loop-vst";
	params.Size = { 80, 80 };
	audio::WireMixBehaviourParams wire;
	auto mixerParams = Loop::GetMixerParams(params.Size, wire);
	return std::make_shared<Loop>(params, mixerParams);
}

// ---------------------------------------------------------------------------
// LoopTake staging tests
// ---------------------------------------------------------------------------

TEST(VstStaging, LoopTake_LoadVstPlugin_DoesNotLoadImmediately)
{
	auto take = MakeTake();

	// No chain before staging
	EXPECT_EQ(take->GetVstPlugin(0), nullptr);

	take->LoadVstPlugin(L"dummy.vst3");

	// Chain must still be null — the plugin hasn't been loaded yet
	EXPECT_EQ(take->GetVstPlugin(0), nullptr);
}

TEST(VstStaging, LoopTake_LoadVstPlugin_CreatesJobOnCommit)
{
	auto take = MakeTake();
	take->LoadVstPlugin(L"dummy.vst3");

	// CommitChanges should return exactly one JOB_LOADVST job.
	auto jobs = take->CommitChanges();
	auto vstJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	EXPECT_EQ(vstJobs, 1);
}

TEST(VstStaging, LoopTake_LoadVstPlugin_JobCarriesPath)
{
	auto take = MakeTake();
	take->LoadVstPlugin(L"my_effect.vst3");

	auto jobs = take->CommitChanges();
	auto it = std::find_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	ASSERT_NE(it, jobs.end());
	EXPECT_EQ(it->VstPath, L"my_effect.vst3");
}

TEST(VstStaging, LoopTake_SecondCommitNoExtraJobs)
{
	auto take = MakeTake();
	take->LoadVstPlugin(L"plugin.vst3");

	take->CommitChanges(); // consumes the pending load

	// Second CommitChanges must NOT re-emit a VST job
	auto jobs2 = take->CommitChanges();
	auto vstJobs = std::count_if(jobs2.begin(), jobs2.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	EXPECT_EQ(vstJobs, 0);
}

TEST(VstStaging, LoopTake_UnloadVstPlugin_CreatesJobOnCommit)
{
	auto take = MakeTake();
	take->UnloadVstPlugin(0u);

	auto jobs = take->CommitChanges();
	auto vstJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST;
	});
	EXPECT_EQ(vstJobs, 1);
}

// Two rapid UnloadVstPlugin() calls must each produce their own JOB_UNLOADVST job.
// Previously the second call silently overwrote the first (_pendingVstUnload was
// a single size_t), so only index 1 would be removed.
TEST(VstStaging, LoopTake_TwoRapidUnloads_BothJobsEmitted)
{
	auto take = MakeTake();
	take->UnloadVstPlugin(0u);
	take->UnloadVstPlugin(1u);

	auto jobs = take->CommitChanges();
	auto unloadJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST;
	});
	EXPECT_EQ(unloadJobs, 2);

	// Verify both indices are present.
	auto hasIdx0 = std::any_of(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST && j.VstIndex == 0u;
	});
	auto hasIdx1 = std::any_of(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST && j.VstIndex == 1u;
	});
	EXPECT_TRUE(hasIdx0);
	EXPECT_TRUE(hasIdx1);
}

// ---------------------------------------------------------------------------
// Loop staging tests
// ---------------------------------------------------------------------------

TEST(VstStaging, Loop_LoadVstPlugin_DoesNotLoadImmediately)
{
	auto loop = MakeLoop();

	EXPECT_EQ(loop->GetVstPlugin(0), nullptr);

	loop->LoadVstPlugin(L"dummy.vst3");

	EXPECT_EQ(loop->GetVstPlugin(0), nullptr);
}

TEST(VstStaging, Loop_LoadVstPlugin_CreatesJobOnCommit)
{
	auto loop = MakeLoop();
	loop->LoadVstPlugin(L"dummy.vst3");

	auto jobs = loop->CommitChanges();
	auto vstJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	EXPECT_EQ(vstJobs, 1);
}

TEST(VstStaging, Loop_LoadVstPlugin_JobCarriesPath)
{
	auto loop = MakeLoop();
	loop->LoadVstPlugin(L"reverb.vst3");

	auto jobs = loop->CommitChanges();
	auto it = std::find_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	ASSERT_NE(it, jobs.end());
	EXPECT_EQ(it->VstPath, L"reverb.vst3");
}

TEST(VstStaging, Loop_UnloadVstPlugin_CreatesJobOnCommit)
{
	auto loop = MakeLoop();
	loop->UnloadVstPlugin(0u);

	auto jobs = loop->CommitChanges();
	auto vstJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST;
	});
	EXPECT_EQ(vstJobs, 1);
}

// Two rapid UnloadVstPlugin() calls must each produce their own JOB_UNLOADVST job.
TEST(VstStaging, Loop_TwoRapidUnloads_BothJobsEmitted)
{
	auto loop = MakeLoop();
	loop->UnloadVstPlugin(0u);
	loop->UnloadVstPlugin(1u);

	auto jobs = loop->CommitChanges();
	auto unloadJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST;
	});
	EXPECT_EQ(unloadJobs, 2);

	auto hasIdx0 = std::any_of(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST && j.VstIndex == 0u;
	});
	auto hasIdx1 = std::any_of(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_UNLOADVST && j.VstIndex == 1u;
	});
	EXPECT_TRUE(hasIdx0);
	EXPECT_TRUE(hasIdx1);
}

TEST(VstStaging, Loop_SetSampleRateAndBlockSize_PropagateToFields)
{
	auto loop = MakeLoop();
	loop->SetSampleRate(48000.0f);
	loop->SetBlockSize(256u);

	// Staging a load after SetSampleRate/SetBlockSize must still produce the
	// correct JOB_LOADVST job — verifies the setters don't corrupt staging state.
	loop->LoadVstPlugin(L"after_rate_change.vst3");
	auto jobs = loop->CommitChanges();

	auto it = std::find_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	ASSERT_NE(it, jobs.end());
	EXPECT_EQ(it->VstPath, L"after_rate_change.vst3");
}

// ---------------------------------------------------------------------------
// Back-buffer param propagation tests (Task 2 correctness)
//
// Regression coverage for the startup ordering flaw: SetSampleRate /
// SetupBuffers are called on Station/LoopTake BEFORE CommitChanges() swaps
// back-buffer objects into the front buffer.  Consequently those calls must
// propagate to BOTH the live (_loops / _loopTakes) and back-buffer
// (_backLoops / _backLoopTakes) collections so that deferred VST loads receive
// the real device parameters rather than the compile-time defaults.
// ---------------------------------------------------------------------------

// LoopTake::SetSampleRate must reach loops that are still in _backLoops
// (i.e., added via AddLoop() but not yet committed).
TEST(VstStaging, LoopTake_SetSampleRate_PropagatesTo_BackBufferLoops)
{
	auto take = MakeTake();
	auto loop = MakeLoop();

	// AddLoop() places the loop in _backLoops, NOT in the live _loops.
	take->AddLoop(loop);

	// Call SetSampleRate before CommitChanges — simulates InitAudio ordering.
	const float targetRate = 48000.0f;
	take->SetSampleRate(targetRate);

	// The loop must have received the updated rate even though it is still in
	// the back-buffer.
	EXPECT_FLOAT_EQ(loop->GetSampleRate(), targetRate);
}

// LoopTake::SetupBuffers must reach loops that are still in _backLoops.
TEST(VstStaging, LoopTake_SetupBuffers_PropagatesTo_BackBufferLoops)
{
	auto take = MakeTake();
	auto loop = MakeLoop();

	take->AddLoop(loop);

	const unsigned int targetBufSize = 256u;
	take->SetupBuffers(targetBufSize);

	EXPECT_EQ(loop->GetBlockSize(), targetBufSize);
}

// After SetSampleRate+SetupBuffers propagate to the back-buffer loop, the
// loop's JOB_LOADVST is still correctly emitted by CommitChanges().
TEST(VstStaging, LoopTake_SetSampleRate_ThenCommit_EmitsVstJob)
{
	auto take = MakeTake();
	auto loop = MakeLoop();

	take->AddLoop(loop);
	loop->LoadVstPlugin(L"effect.vst3");

	take->SetSampleRate(48000.0f);
	take->SetupBuffers(256u);

	auto jobs = take->CommitChanges();

	auto vstJobs = std::count_if(jobs.begin(), jobs.end(), [](const JobAction& j) {
		return j.JobActionType == JobAction::JOB_LOADVST;
	});
	EXPECT_GE(vstJobs, 1);
}

// LoopTake itself must reflect the updated sample rate for its own JOB_LOADVST.
TEST(VstStaging, LoopTake_SetSampleRate_UpdatesOwnRate)
{
	auto take = MakeTake();
	const float targetRate = 44100.0f;
	take->SetSampleRate(targetRate);

	EXPECT_FLOAT_EQ(take->GetSampleRate(), targetRate);
}

// LoopTake itself must reflect the updated buffer size for its own JOB_LOADVST.
TEST(VstStaging, LoopTake_SetupBuffers_UpdatesOwnBufSize)
{
	auto take = MakeTake();
	const unsigned int targetBufSize = 512u;
	take->SetupBuffers(targetBufSize);

	EXPECT_EQ(take->GetLastBufSize(), targetBufSize);
}


class StateGatePlugin : public vst::IVstPlugin
{
public:
 mutable bool ProbeRejected = false;
 bool ReturnEmpty = false;
 std::vector<std::uint8_t> Restored;
 bool PreInit(const std::wstring&) override { return true; }
 bool Load(const std::wstring&, float, unsigned, unsigned, vst::HostedLayoutMode) override { return true; }
 void Unload() override {}
 void ProcessBlock(float*, int32_t) noexcept override {}
 void ProcessBlockStereo(float*, float*, int32_t) noexcept override {}
 void ProcessBlockMulti(float* const*, int32_t, int32_t) noexcept override {}
 void SetParameter(unsigned, float) noexcept override {}
 float GetParameter(unsigned) const noexcept override { return 0; }
 void BeginMidiBlock(std::uint32_t, std::uint32_t) noexcept override {}
 void SendMidiEvent(const midi::MidiEvent&, bool) noexcept override {}
 bool OpenEditor(HWND) override { return false; }
 void CloseEditor() override {}
 utils::Size2d GetEditorSize() const noexcept override { return {0,0}; }
 bool IsLoaded() const noexcept override { return true; }
 const std::string& Name() const noexcept override { return StateId(); }
 void SetBypassed(bool) noexcept override {}
 bool IsBypassed() const noexcept override { return false; }
 bool Probe() const { PluginCallGuard call(*this); return static_cast<bool>(call); }
 bool CaptureDuringCallback(std::vector<std::uint8_t>& output)
 {
  PluginCallGuard callback(*this);
  return callback && TryCaptureLiveState(output);
 }
 std::vector<std::uint8_t> GetState() const override
 {
  ProbeRejected = !Probe();
  return ReturnEmpty ? std::vector<std::uint8_t>{} : std::vector<std::uint8_t>{1,2,3};
 }
 void SetState(const std::vector<std::uint8_t>& blob) override
 {
  ProbeRejected = !Probe();
  Restored = blob;
  vst::PublishLastTouchedParameter(this, 2, 0.75f);
 }
};

TEST(VstStaging, LiveCaptureRejectsActiveCallbackAndPreservesOutput)
{
 StateGatePlugin plugin;
 std::vector<std::uint8_t> output{9};
 EXPECT_FALSE(plugin.CaptureDuringCallback(output));
 EXPECT_EQ(output, (std::vector<std::uint8_t>{9}));
 EXPECT_TRUE(plugin.TryCaptureLiveState(output));
 EXPECT_EQ(output, (std::vector<std::uint8_t>{1,2,3}));
 EXPECT_TRUE(plugin.ProbeRejected);
 EXPECT_TRUE(plugin.Probe());
}

TEST(VstStaging, FailedLiveCaptureKeepsExistingSnapshot)
{
 StateGatePlugin plugin;
 plugin.ReturnEmpty = true;
 std::vector<std::uint8_t> output{9};
 EXPECT_FALSE(plugin.TryCaptureLiveState(output));
 EXPECT_EQ(output, (std::vector<std::uint8_t>{9}));
 EXPECT_TRUE(plugin.Probe());
}

TEST(VstStaging, LiveRestoreExcludesSdkCallsInvalidatesAutomationAndSuppressesRecording)
{
 StateGatePlugin plugin;
 const auto sequence = vst::_lastTouchedParam.Sequence.load();
 const auto generation = plugin.AutomationGeneration();
 EXPECT_TRUE(plugin.TryRestoreLiveState({4,5}));
 EXPECT_EQ(plugin.Restored, (std::vector<std::uint8_t>{4,5}));
 EXPECT_TRUE(plugin.ProbeRejected);
 EXPECT_GT(plugin.AutomationGeneration(), generation);
 EXPECT_EQ(vst::_lastTouchedParam.Sequence.load(), sequence);
 EXPECT_TRUE(plugin.Probe());
}

TEST(VstStaging, GroupPluginIdentityDoesNotFollowReplacementPosition)
{
 StateGatePlugin original;
 StateGatePlugin replacement;
 EXPECT_NE(original.StateId(), replacement.StateId());
 replacement.SetStateId("persisted-plugin");
 EXPECT_EQ(replacement.StateId(), "persisted-plugin");
}


class RaceStateGatePlugin : public StateGatePlugin
{
public:
 mutable std::atomic<unsigned> ActiveSdk{0};
 mutable std::atomic<bool> Overlap{false};
 mutable std::atomic<bool> StateEntered{false};
 mutable std::atomic<bool> ReleaseState{false};
 std::atomic<bool> CallbackEntered{false};
 std::atomic<bool> ReleaseCallback{false};
 bool HoldState = false;
 bool GuardedCallback(bool hold)
 {
  PluginCallGuard call(*this);
  if (!call) return false;
  if (ActiveSdk.fetch_add(1) != 0) Overlap.store(true);
  CallbackEntered.store(true);
  while (hold && !ReleaseCallback.load()) std::this_thread::yield();
  ActiveSdk.fetch_sub(1);
  return true;
 }
 std::vector<std::uint8_t> GetState() const override
 {
  if (ActiveSdk.fetch_add(1) != 0) Overlap.store(true);
  StateEntered.store(true);
  while (HoldState && !ReleaseState.load()) std::this_thread::yield();
  ActiveSdk.fetch_sub(1);
  return {1,2,3};
 }
};

TEST(VstStaging, TwoThreadLiveStateGateNeverOverlapsCallbackSdkRegion)
{
 RaceStateGatePlugin plugin;
 std::vector<std::uint8_t> output{9};
 std::thread callback([&] { plugin.GuardedCallback(true); });
 while (!plugin.CallbackEntered.load()) std::this_thread::yield();
 const bool captureWhileCallback = plugin.TryCaptureLiveState(output);
 plugin.ReleaseCallback.store(true);
 callback.join();
 EXPECT_FALSE(captureWhileCallback);
 EXPECT_EQ(output, (std::vector<std::uint8_t>{9}));
 plugin.HoldState = true;
 bool callbackWhileCapture = true;
 std::thread retry([&] {
  while (!plugin.StateEntered.load()) std::this_thread::yield();
  callbackWhileCapture = plugin.GuardedCallback(false);
  plugin.ReleaseState.store(true);
 });
 const bool captured = plugin.TryCaptureLiveState(output);
 retry.join();
 EXPECT_TRUE(captured);
 EXPECT_FALSE(callbackWhileCapture);
 EXPECT_FALSE(plugin.Overlap.load());
 EXPECT_TRUE(plugin.GuardedCallback(false));
}

class PanicStateGatePlugin : public StateGatePlugin
{
public:
 std::array<unsigned,16> Panics{};
 bool Bypassed = false;
 void SetBypassed(bool value) noexcept override { Bypassed = value; }
 bool IsBypassed() const noexcept override { return Bypassed; }
 void SetState(const std::vector<std::uint8_t>& blob) override
 {
  StateGatePlugin::SetState(blob);
  PluginCallGuard lostMidi(*this, true);
  ProbeRejected = !static_cast<bool>(lostMidi);
 }
 void SendMidiEvent(const midi::MidiEvent& event, bool) noexcept override
 {
  if (event.MessageType() == 0xb0 && event.data1 == 123) ++Panics[event.Channel()];
 }
};

TEST(VstStaging, SkippedCallbackRecoversAllChannelsOnceBeforeNormalDelivery)
{
 PanicStateGatePlugin plugin;
 EXPECT_TRUE(plugin.TryRestoreLiveState({1})); // SetState explicitly attempts guarded callback entry.
 EXPECT_TRUE(plugin.ProbeRejected);
 {
  vst::IVstPlugin::PluginCallGuard call(plugin);
  ASSERT_TRUE(static_cast<bool>(call));
  plugin.FlushPendingMidiPanic();
  plugin.FlushPendingMidiPanic();
 }
 for (auto count : plugin.Panics) EXPECT_EQ(count, 1u);
}

TEST(VstStaging, CaptureInvalidatesAutomationAfterExclusiveCallbackSkip)
{
 StateGatePlugin plugin;
 std::vector<std::uint8_t> output;
 const auto generation = plugin.AutomationGeneration();
 EXPECT_TRUE(plugin.TryCaptureLiveState(output));
 EXPECT_GT(plugin.AutomationGeneration(), generation);
 EXPECT_TRUE(plugin.ProbeRejected);
}


TEST(VstStaging, ReservedPanicEventsPreserveFullSameBlockMidiAndRejectOverflow)
{
 vst::ReservedMidiEvents<midi::MidiEvent, 2> events;
 const auto noteOn = midi::MidiEvent::MakeNoteOn(7, 0, 60, 100);
 const auto noteOff = midi::MidiEvent::MakeNoteOff(8, 0, 60);
 ASSERT_TRUE(events.Append(noteOn));
 ASSERT_TRUE(events.Append(noteOff));
 EXPECT_FALSE(events.Append(noteOn));
 for (std::uint8_t channel = 0; channel < 16; ++channel)
  ASSERT_TRUE(events.Append({0, static_cast<std::uint8_t>(0xb0u | channel), 123, 0}, true));
 EXPECT_EQ(events.Count(), 18u);
 EXPECT_EQ(events.At(0).data1, 123u);
 EXPECT_TRUE(events.At(16).IsNoteOn());
 EXPECT_TRUE(events.At(17).IsNoteOff());
 EXPECT_EQ(events.At(17).sampleOffset, 8u);
 EXPECT_FALSE(events.Append({0,0xb0,123,0}, true));
 events.Clear();
 EXPECT_TRUE(events.Append(noteOff));
 EXPECT_EQ(events.Count(), 1u);
}


TEST(VstStaging, CaptureWithOnlyRejectedParameterReadDoesNotInterruptHeldNotes)
{
 PanicStateGatePlugin plugin;
 std::vector<std::uint8_t> state;
 ASSERT_TRUE(plugin.TryCaptureLiveState(state));
 EXPECT_TRUE(plugin.ProbeRejected);
 vst::IVstPlugin::PluginCallGuard call(plugin);
 ASSERT_TRUE(static_cast<bool>(call));
 plugin.FlushPendingMidiPanic();
 for (auto count : plugin.Panics) EXPECT_EQ(count, 0u);
}


TEST(VstStaging, MidiPanicSurvivesBypassUntilProcessorCanReceiveIt)
{
 PanicStateGatePlugin plugin;
 ASSERT_TRUE(plugin.TryRestoreLiveState({1}));
 plugin.SetBypassed(true);
 vst::IVstPlugin::PluginCallGuard call(plugin);
 ASSERT_TRUE(static_cast<bool>(call));
 plugin.FlushPendingMidiPanic();
 for (auto count : plugin.Panics) EXPECT_EQ(count, 0u);
 plugin.SetBypassed(false);
 plugin.FlushPendingMidiPanic();
 for (auto count : plugin.Panics) EXPECT_EQ(count, 1u);
}
