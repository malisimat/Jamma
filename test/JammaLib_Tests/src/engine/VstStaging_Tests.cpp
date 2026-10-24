
#include "gtest/gtest.h"
#include <thread>
#include <atomic>
#include <array>
#include "actions/JobAction.h"
#include "engine/Loop.h"
#include "vst/ReservedMidiEvents.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "engine/PerformanceGroupController.h"
#include "vst/VstEditorWindowManager.h"
#include "gui/VstRackControls.h"
#include <functional>

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
 vst::RenderRole Role() const noexcept override { return vst::RenderRole::Effect; }
 void SetBypassed(bool) noexcept override {}
 bool IsBypassed() const noexcept override { return false; }
 bool Probe() const { PluginCallGuard call(*this); return static_cast<bool>(call); }
 bool HousekeepingProbe() const { ExclusivePluginCallGuard call(*this); return static_cast<bool>(call); }
 bool HousekeepingDuringCallback() const
 {
  PluginCallGuard callback(*this);
  return HousekeepingProbe();
 }
 bool HousekeepingRejectsReentry() const
 {
  ExclusivePluginCallGuard housekeeping(*this);
  return housekeeping && !Probe() && !HousekeepingProbe();
 }
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

TEST(VstStaging, ControllerHousekeepingDefersActiveReadersAndNestedMessagePump)
{
 StateGatePlugin plugin;
 EXPECT_FALSE(plugin.HousekeepingDuringCallback());
 EXPECT_TRUE(plugin.HousekeepingRejectsReentry());
 EXPECT_TRUE(plugin.HousekeepingProbe());
 EXPECT_TRUE(plugin.Probe());
 // Housekeeping never changes the saved-state automation generation.
 EXPECT_EQ(plugin.AutomationGeneration(), 0u);
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


class MuteRecallPlugin final : public StateGatePlugin
{
public:
 mutable unsigned int CaptureCalls = 0u;
 unsigned int RestoreCalls = 0u;
 unsigned int LifecycleCalls = 0u;
 unsigned int BypassCalls = 0u;
 std::atomic<unsigned int> ProcessCalls{0u};
 std::atomic<unsigned int> SkippedProcessCalls{0u};
 void ProcessBlock(float* samples, int32_t count) noexcept override
 {
  PluginCallGuard call(*this);
  if (!call) { ++SkippedProcessCalls; return; }
  ++ProcessCalls;
  for (int32_t i = 0; i < count; ++i) samples[i] *= 2.0f;
 }
 bool Load(const std::wstring&, float, unsigned, unsigned, vst::HostedLayoutMode) override
 {
  ++LifecycleCalls;
  return true;
 }
 void Unload() override { ++LifecycleCalls; }
 std::vector<std::uint8_t> GetState() const override
 {
  ++CaptureCalls;
  return StateGatePlugin::GetState();
 }
 void SetState(const std::vector<std::uint8_t>& blob) override
 {
  ++RestoreCalls;
  StateGatePlugin::SetState(blob);
 }
 void SetBypassed(bool) noexcept override { ++BypassCalls; }
};

TEST(VstStaging, RapidMuteGroupRecallNeverTouchesStationOrTakePlugins)
{
 engine::StationParams params;
 params.Id = "mute-recall-station";
 params.Size = {100u, 100u};
 auto station = std::make_shared<engine::Station>(params,
  engine::Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
 auto take = station->AddTake();
 auto stationPlugin = std::make_shared<MuteRecallPlugin>();
 auto takePlugin = std::make_shared<MuteRecallPlugin>();
 auto install = [](auto& owner, const auto& plugin) {
  JobAction load;
  load.JobActionType = JobAction::JOB_LOADVST;
  load.VstPath = L"mute-recall.vst3";
  load.PreInitPlugin = plugin;
  owner->OnAction(load);
  owner->CommitChanges();
  ASSERT_EQ(owner->GetVstPlugin(0u), plugin);
  plugin->LifecycleCalls = 0u;
  plugin->BypassCalls = 0u;
 };
 install(station, stationPlugin);
 install(take, takePlugin);
 engine::PerformanceGroupService::Stations stations{station};
 auto audible = engine::PerformanceGroupService::CaptureMuteConfiguration(stations);
 const auto blob = io::JamFile::VstEntry::EncodeState({4, 5});
 audible.Plugins = {
  {{engine::GroupTargetKind::Station, station->Id(), {}, {}}, stationPlugin->StateId(), "mute-recall.vst3", blob, true},
  {{engine::GroupTargetKind::Take, station->Id(), take->Id(), {}}, takePlugin->StateId(), "mute-recall.vst3", blob, true}
 };
 auto muted = audible;
 for (auto& target : muted.Targets) target.Muted = target.EffectiveMuted = true;
 engine::PerformanceGroupController controller;
 ASSERT_TRUE(controller.Groups().Mute.Store(1u, audible));
 ASSERT_TRUE(controller.Groups().Mute.Store(2u, muted));
 std::shared_ptr<const engine::GroupMuteApplication> pending;
 unsigned int publications = 0u;
 auto publish = [&](auto application) { pending = std::move(application); ++publications; };
 // Recall must remain independent of plugin availability. Leave the first
 // publication unapplied to model faster input than audio boundaries.
 {
  vst::IVstPlugin::PluginCallGuard stationBusy(*stationPlugin);
  vst::IVstPlugin::PluginCallGuard takeBusy(*takePlugin);
  ASSERT_TRUE(controller.Execute(false, engine::GroupCommand::RecallOrStore, 2u,
   stations, engine::GroupTargetKind::Take, publish));
  ASSERT_TRUE(pending);
  EXPECT_TRUE(pending->Takes[0].Muted);
  ASSERT_TRUE(controller.Execute(false, engine::GroupCommand::RecallOrStore, 1u,
   stations, engine::GroupTargetKind::Take, publish));
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->Takes[0].Muted);
 }
 EXPECT_EQ(publications, 2u);
 pending->ApplyAudio();
 EXPECT_FALSE(take->IsMuted());
 EXPECT_EQ(controller.Groups().Mute.ActiveSlot(), 1u);
 EXPECT_TRUE(controller.Error().empty());
 EXPECT_EQ(station->GetVstPlugin(0u), stationPlugin);
 EXPECT_EQ(take->GetVstPlugin(0u), takePlugin);
 for (const auto& plugin : {stationPlugin, takePlugin})
 {
  EXPECT_EQ(plugin->CaptureCalls, 0u);
  EXPECT_EQ(plugin->RestoreCalls, 0u);
  EXPECT_EQ(plugin->LifecycleCalls, 0u);
  EXPECT_EQ(plugin->BypassCalls, 0u);
  EXPECT_EQ(plugin->AutomationGeneration(), 0u);
  EXPECT_TRUE(plugin->Probe());
 }
}

TEST(VstStaging, MuteGroupCreationAndOverwriteNeverCaptureOrInterruptBusyStationAndTakePlugins)
{
 engine::StationParams params;
 params.Id = "mute-store-station";
 params.Size = {100u, 100u};
 auto station = std::make_shared<engine::Station>(params,
  engine::Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
 auto take = station->AddTake();
 auto stationPlugin = std::make_shared<MuteRecallPlugin>();
 auto takePlugin = std::make_shared<MuteRecallPlugin>();
 auto install = [](auto& owner, const auto& plugin) {
  JobAction load;
  load.JobActionType = JobAction::JOB_LOADVST;
  load.VstPath = L"mute-store.vst3";
  load.PreInitPlugin = plugin;
  owner->OnAction(load);
  owner->CommitChanges();
  ASSERT_EQ(owner->GetVstPlugin(0u), plugin);
  plugin->LifecycleCalls = plugin->BypassCalls = 0u;
 };
 install(station, stationPlugin);
 install(take, takePlugin);
 engine::PerformanceGroupController controller;
 auto publish = [](auto) { FAIL() << "Store must not publish audio work"; };
 actions::KeyAction key;
 key.KeyChar = 112u;
 key.KeyActionType = actions::KeyAction::KEY_DOWN;
 {
  vst::IVstPlugin::PluginCallGuard stationBusy(*stationPlugin);
  vst::IVstPlugin::PluginCallGuard takeBusy(*takePlugin);
  ASSERT_TRUE(static_cast<bool>(stationBusy));
  ASSERT_TRUE(static_cast<bool>(takeBusy));
  ASSERT_TRUE(controller.HandleKey(key, {station}, engine::GroupTargetKind::Take, publish));
  ASSERT_NE(controller.Groups().Mute.Get(1u), nullptr);
  EXPECT_FALSE(controller.Groups().Mute.Get(1u)->Targets[1].Muted);
  take->SetGroupMutedAudio(true);
  ASSERT_TRUE(controller.Execute(false, engine::GroupCommand::Add, 0u,
   {station}, engine::GroupTargetKind::Take, publish));
  ASSERT_NE(controller.Groups().Mute.Get(2u), nullptr);
  EXPECT_TRUE(controller.Groups().Mute.Get(2u)->Targets[1].Muted);
  key.KeyActionType = actions::KeyAction::KEY_UP;
  ASSERT_TRUE(controller.ConsumeRelease(key));
  key.KeyActionType = actions::KeyAction::KEY_DOWN;
  key.Modifiers = base::Action::MODIFIER_SHIFT;
  ASSERT_TRUE(controller.HandleKey(key, {station}, engine::GroupTargetKind::Take, publish));
  EXPECT_TRUE(controller.Groups().Mute.Get(1u)->Targets[1].Muted);
  // Rejected hidden keys preserve the immediately completed store.
  key.KeyChar = 120u;
  key.Modifiers = base::Action::MODIFIER_NONE;
  ASSERT_TRUE(controller.HandleKey(key, {station}, engine::GroupTargetKind::Take, publish));
  key.KeyChar = 115u;
  key.Modifiers = base::Action::MODIFIER_ALT;
  ASSERT_TRUE(controller.HandleKey(key, {station}, engine::GroupTargetKind::Take, publish));
  // The processors remain usable throughout the stores, including while
  // another callback entry holds its normal shared SDK guard.
  for (const auto& plugin : {stationPlugin, takePlugin})
  {
   float samples[2]{1.0f, -1.0f};
   plugin->ProcessBlock(samples, 2);
   EXPECT_FLOAT_EQ(samples[0], 2.0f);
   EXPECT_FLOAT_EQ(samples[1], -2.0f);
  }
 }
 // A nonbusy store catches the old vendor capture path; repeat stores while
 // processing audio to exercise the cross-thread exclusion regression too.
 ASSERT_TRUE(controller.Execute(false, engine::GroupCommand::Store, 1u,
  {station}, engine::GroupTargetKind::Take, publish));
 std::atomic<bool> stopAudio{false};
 std::atomic<bool> audioReady{false};
 std::atomic<bool> samplesIncorrect{false};
 std::thread audio([&] {
  do
  {
   for (const auto& plugin : {stationPlugin, takePlugin})
   {
    float samples[2]{1.0f, -1.0f};
    plugin->ProcessBlock(samples, 2);
    if (samples[0] != 2.0f || samples[1] != -2.0f) samplesIncorrect.store(true);
   }
   audioReady.store(true);
  } while (!stopAudio.load());
 });
 while (!audioReady.load()) std::this_thread::yield();
 for (unsigned int i = 0u; i < 64u; ++i)
  EXPECT_TRUE(controller.Execute(false, engine::GroupCommand::Store, 1u,
   {station}, engine::GroupTargetKind::Take, publish));
 stopAudio.store(true);
 audio.join();
 EXPECT_FALSE(samplesIncorrect.load());
 EXPECT_EQ(controller.Groups().Mute.ActiveSlot(), 1u);
 EXPECT_TRUE(controller.Error().empty());
 EXPECT_TRUE(controller.Groups().Mute.Get(1u)->Plugins.empty());
 EXPECT_TRUE(controller.Groups().Mute.Get(2u)->Plugins.empty());
 EXPECT_EQ(station->GetVstPlugin(0u), stationPlugin);
 EXPECT_EQ(take->GetVstPlugin(0u), takePlugin);
 for (const auto& plugin : {stationPlugin, takePlugin})
 {
  EXPECT_EQ(plugin->CaptureCalls, 0u);
  EXPECT_EQ(plugin->RestoreCalls, 0u);
  EXPECT_EQ(plugin->LifecycleCalls, 0u);
  EXPECT_EQ(plugin->BypassCalls, 0u);
  EXPECT_EQ(plugin->AutomationGeneration(), 0u);
  EXPECT_TRUE(plugin->Restored.empty());
  EXPECT_GE(plugin->ProcessCalls.load(), 2u);
  EXPECT_EQ(plugin->SkippedProcessCalls.load(), 0u);
 }
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
 EXPECT_EQ(events.NormalCount(), 2u);
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

// Model plugins that pump native messages during getState: a message can
// reenter metadata updates on the same owner before state capture returns.
class ReentrantMetadataPlugin final : public StateGatePlugin
{
public:
 std::function<void()> DuringCapture;
 std::vector<std::uint8_t> GetState() const override
 {
  if (DuringCapture) DuringCapture();
  return {1, 2, 3};
 }
};

class VstMetadataReentryTest : public testing::Test
{
protected:
 template<typename Owner, typename Capture>
 static void CheckCapture(const std::shared_ptr<Owner>& owner, Capture capture)
 {
  auto plugin = std::make_shared<ReentrantMetadataPlugin>();
  JobAction load;
  load.JobActionType = JobAction::JOB_LOADVST;
  load.VstPath = L"reentrant.vst3";
  load.PreInitPlugin = plugin;
  owner->OnAction(load);
  owner->CommitChanges();
  ASSERT_EQ(owner->GetVstPlugin(0u), plugin);

  bool reentered = false;
  plugin->DuringCapture = [&owner, &reentered, identity = plugin->StateId()] {
   JobAction unload;
   unload.JobActionType = JobAction::JOB_UNLOADVST;
   unload.VstIndex = 0u;
   unload.VstIdentity = identity;
   owner->OnAction(unload);
   reentered = true;
  };
  const auto entries = capture(*owner);
  // The plugin can survive this test in a retired snapshot / destroy queue.
  plugin->DuringCapture = {};
  EXPECT_TRUE(reentered);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].Path, "reentrant.vst3");
  EXPECT_EQ(entries[0].DecodeState(), (std::vector<std::uint8_t>{1, 2, 3}));
 }

 static std::shared_ptr<engine::Station> MakeStation()
 {
  engine::StationParams params;
  params.Id = "station-vst-reentry";
  params.Size = {100u, 100u};
  return std::make_shared<engine::Station>(params,
   engine::Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
 }

 template<typename Owner>
 static std::vector<io::JamFile::VstEntry> CaptureGroup(Owner& owner)
 {
  std::vector<io::JamFile::VstEntry> entries;
  EXPECT_TRUE(owner.TryGroupVstEntries(entries));
  return entries;
 }
};

TEST_F(VstMetadataReentryTest, LoopFileCaptureAllowsReentrantMetadataUpdate)
{
 CheckCapture(MakeLoop(), [](Loop& loop) { return loop.ToJamFile("loop.wav").VstChain; });
}

TEST_F(VstMetadataReentryTest, TakeFileCaptureAllowsReentrantMetadataUpdate)
{
 CheckCapture(MakeTake(), [](LoopTake& take) { return take.VstEntries(); });
}

TEST_F(VstMetadataReentryTest, TakeGroupCaptureAllowsReentrantMetadataUpdate)
{
 CheckCapture(MakeTake(), [](LoopTake& take) { return CaptureGroup(take); });
}

TEST_F(VstMetadataReentryTest, StationFileCaptureAllowsReentrantMetadataUpdate)
{
 CheckCapture(MakeStation(), [](engine::Station& station) { return station.VstEntries(); });
}

TEST_F(VstMetadataReentryTest, StationGroupCaptureAllowsReentrantMetadataUpdate)
{
 CheckCapture(MakeStation(), [](engine::Station& station) { return CaptureGroup(station); });
}

class ReentrantEditorDestructionPlugin final : public StateGatePlugin
{
public:
 HWND EditorParent = nullptr;
 std::function<void()> DuringDestruction;
 ~ReentrantEditorDestructionPlugin() override
 {
  if (DuringDestruction) DuringDestruction();
 }
 bool OpenEditor(HWND parent) override
 {
  EditorParent = parent;
  return true;
 }
 utils::Size2d GetEditorSize() const noexcept override
 {
  // Close while Create still owns a hidden frame, before ShowWindow.
  // The closed manager entry retains the final plugin reference until prune.
  SendMessage(EditorParent, WM_CLOSE, 0, 0);
  return {0, 0};
 }
};

TEST(VstStaging, DISABLED_PruningClosedEditorAllowsReentrantPluginDestruction)
{
 vst::VstEditorWindowManager manager;
 bool reentered = false;
 auto plugin = std::make_shared<ReentrantEditorDestructionPlugin>();
 plugin->DuringDestruction = [&] {
  manager.PruneClosedVstEditorWindows();
  reentered = true;
 };
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_FALSE(IsWindow(plugin->EditorParent));
 plugin.reset();
 EXPECT_FALSE(reentered);
 manager.PruneClosedVstEditorWindows();
 EXPECT_TRUE(reentered);
}
class ReentrantEditorIdlePlugin final : public StateGatePlugin
{
public:
 HWND EditorParent = nullptr;
 unsigned int IdleCalls = 0;
 bool OpenEditor(HWND parent) override { EditorParent = parent; return true; }
 void IdleEditor() noexcept override
 {
  ++IdleCalls;
  if (IdleCalls == 1u)
  {
   SendMessage(EditorParent, WM_TIMER, 1, 0);
   SendMessage(EditorParent, graphics::VstEditorWindow::MessageVst2Idle, 0, 0);
  }
 }
};

TEST(VstStaging, DISABLED_EditorIdleRejectsNestedTimerAndVst2IdleMessages)
{
 auto plugin = std::make_shared<ReentrantEditorIdlePlugin>();
 graphics::VstEditorWindow editor;
 ASSERT_TRUE(editor.Create(GetModuleHandle(nullptr), plugin));
 SendMessage(editor.EditorHwnd(), WM_TIMER, 1, 0);
 EXPECT_EQ(plugin->IdleCalls, 1u);
 SendMessage(editor.EditorHwnd(), graphics::VstEditorWindow::MessageVst2Idle, 0, 0);
 EXPECT_EQ(plugin->IdleCalls, 2u);
 editor.Destroy();
}

class ReentrantEditorLifetimePlugin final : public StateGatePlugin
{
public:
 HWND EditorParent = nullptr;
 vst::VstEditorWindowManager* Manager = nullptr;
 std::weak_ptr<graphics::VstEditorWindow> Editor;
 bool KeptAliveDuringIdle = false;
 bool OpenEditor(HWND parent) override { EditorParent = parent; return true; }
 void IdleEditor() noexcept override
 {
  auto* editor = reinterpret_cast<graphics::VstEditorWindow*>(
   GetWindowLongPtr(EditorParent, GWLP_USERDATA));
  Editor = editor->weak_from_this();
  SendMessage(EditorParent, WM_CLOSE, 0, 0);
  Manager->PruneClosedVstEditorWindows();
  Manager->CloseAllVstEditorWindows();
  KeptAliveDuringIdle = !Editor.expired() && IsWindow(EditorParent);
 }
};

TEST(VstStaging, DISABLED_EditorSurvivesReentrantClosePruneAndManagerRemovalDuringIdle)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<ReentrantEditorLifetimePlugin>();
 plugin->Manager = &manager;
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 SendMessage(plugin->EditorParent, graphics::VstEditorWindow::MessageVst2Idle, 0, 0);
 EXPECT_TRUE(plugin->KeptAliveDuringIdle);
 EXPECT_TRUE(plugin->Editor.expired());
 EXPECT_FALSE(IsWindow(plugin->EditorParent));
}

class ReentrantEditorOpenPlugin final : public StateGatePlugin
{
public:
 HWND EditorParent = nullptr;
 unsigned int OpenCalls = 0;
 bool NestedResult = true;
 bool FailOpen = false;
 std::function<bool()> Reopen;
 bool OpenEditor(HWND parent) override
 {
  EditorParent = parent;
  ++OpenCalls;
  // Bound a failing regression rather than overflowing the test's stack.
  if (OpenCalls == 1u) NestedResult = Reopen();
  return !FailOpen;
 }
 utils::Size2d GetEditorSize() const noexcept override
 {
  SendMessage(EditorParent, WM_CLOSE, 0, 0);
  return {0, 0};
 }
};

TEST(VstStaging, DISABLED_ReentrantEditorCreateForSamePluginIsBounded)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<ReentrantEditorOpenPlugin>();
 std::weak_ptr<ReentrantEditorOpenPlugin> weakPlugin = plugin;
 plugin->Reopen = [&manager, weakPlugin] { return manager.OpenVstEditorForPlugin(weakPlugin.lock()); };
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_FALSE(plugin->NestedResult);
 EXPECT_EQ(plugin->OpenCalls, 1u);
 manager.PruneClosedVstEditorWindows();
 // The pending reservation is released even though the editor closed in Create.
 EXPECT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_EQ(plugin->OpenCalls, 2u);
}

TEST(VstStaging, DISABLED_FailedEditorCreateReleasesPendingReservation)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<ReentrantEditorOpenPlugin>();
 std::weak_ptr<ReentrantEditorOpenPlugin> weakPlugin = plugin;
 plugin->Reopen = [&manager, weakPlugin] { return manager.OpenVstEditorForPlugin(weakPlugin.lock()); };
 plugin->FailOpen = true;
 EXPECT_FALSE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_FALSE(plugin->NestedResult);
 plugin->FailOpen = false;
 EXPECT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_EQ(plugin->OpenCalls, 2u);
}
class ReentrantEditorStateClosePlugin final : public StateGatePlugin
{
public:
 HWND EditorParent = nullptr;
 vst::VstEditorWindowManager* Manager = nullptr;
 mutable std::weak_ptr<graphics::VstEditorWindow> Editor;
 mutable bool KeptAliveDuringState = false;
 mutable bool NestedSdkRejected = false;
 mutable bool NestedLiveStateRejected = false;
 unsigned int CloseCalls = 0;
 bool ClosedDuringState = false;
 bool OpenEditor(HWND parent) override { EditorParent = parent; return true; }
 void CloseEditor() override
 {
  ++CloseCalls;
  ClosedDuringState = IsStateOperationActive();
 }
 std::vector<std::uint8_t> GetState() const override
 {
  BlobSdkCallGuard blobCall(*this);
  if (!blobCall) return {};
  NestedSdkRejected = !Probe() && !HousekeepingProbe() && GetState().empty();
  std::vector<std::uint8_t> nestedOutput{9};
  NestedLiveStateRejected = !TryCaptureLiveState(nestedOutput)
   && nestedOutput == std::vector<std::uint8_t>{9};
  auto* editor = reinterpret_cast<graphics::VstEditorWindow*>(
   GetWindowLongPtr(EditorParent, GWLP_USERDATA));
  Editor = editor->weak_from_this();
  SendMessage(EditorParent, WM_CLOSE, 0, 0);
  Manager->CloseAllVstEditorWindows();
  // Repeated pump ticks must neither detach nor continually repost WM_CLOSE.
  for (unsigned int tick = 0u; tick < 3u; ++tick)
   SendMessage(EditorParent, WM_TIMER, 1, 0);
  KeptAliveDuringState = !Editor.expired() && IsWindow(EditorParent) && CloseCalls == 0u;
  return {1, 2, 3};
 }
};

TEST(VstStaging, DISABLED_EditorCloseAndManagerRemovalWaitUntilExclusiveStateCaptureReturns)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<ReentrantEditorStateClosePlugin>();
 plugin->Manager = &manager;
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 std::vector<std::uint8_t> state;
 ASSERT_TRUE(plugin->TryCaptureLiveState(state));
 EXPECT_TRUE(plugin->KeptAliveDuringState);
 EXPECT_FALSE(plugin->Editor.expired());
 EXPECT_TRUE(IsWindow(plugin->EditorParent));
 EXPECT_EQ(plugin->CloseCalls, 0u);
 // The existing timer completes close once the exclusive gate is released.
 SendMessage(plugin->EditorParent, WM_TIMER, 1, 0);
 EXPECT_EQ(plugin->CloseCalls, 1u);
 EXPECT_FALSE(plugin->ClosedDuringState);
 EXPECT_TRUE(plugin->Editor.expired());
 EXPECT_FALSE(IsWindow(plugin->EditorParent));
}

TEST(VstStaging, DISABLED_RawStateCaptureDefersEditorCloseAndRejectsNestedSdkEntry)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<ReentrantEditorStateClosePlugin>();
 plugin->Manager = &manager;
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_EQ(plugin->GetState(), (std::vector<std::uint8_t>{1, 2, 3}));
 EXPECT_TRUE(plugin->NestedSdkRejected);
 EXPECT_TRUE(plugin->NestedLiveStateRejected);
 EXPECT_TRUE(plugin->KeptAliveDuringState);
 EXPECT_FALSE(plugin->IsStateOperationActive());
 EXPECT_TRUE(IsWindow(plugin->EditorParent));
 EXPECT_EQ(plugin->CloseCalls, 0u);
 SendMessage(plugin->EditorParent, WM_TIMER, 1, 0);
 EXPECT_EQ(plugin->CloseCalls, 1u);
 EXPECT_FALSE(plugin->ClosedDuringState);
 EXPECT_TRUE(plugin->Editor.expired());
 EXPECT_FALSE(IsWindow(plugin->EditorParent));
 EXPECT_TRUE(plugin->Probe());
}

class RackEditorTestPlugin final : public StateGatePlugin
{
public:
 HWND Parent = nullptr;
 std::function<void()> DuringOpen;
 std::function<void()> DuringClose;
 bool FailOpen = false;
 utils::Size2d PreferredSize{400, 250};
 unsigned int Opens = 0u;
 unsigned int Closes = 0u;
 bool OpenEditor(HWND parent) override
 {
  Parent = parent;
  ++Opens;
  if (DuringOpen) DuringOpen();
  return !FailOpen;
 }
 void CloseEditor() override { ++Closes; if (DuringClose) DuringClose(); }
 utils::Size2d GetEditorSize() const noexcept override { return PreferredSize; }
};

TEST(VstStaging, DISABLED_RackEditorToggleUsesExactPluginAndTracksTitleBarClose)
{
 vst::VstEditorWindowManager manager;
 auto first = std::make_shared<RackEditorTestPlugin>();
 auto second = std::make_shared<RackEditorTestPlugin>();
 EXPECT_FALSE(manager.IsVstEditorOpen(first));
 ASSERT_TRUE(manager.ToggleVstEditorForPlugin(first));
 ASSERT_TRUE(manager.ToggleVstEditorForPlugin(second));
 EXPECT_TRUE(manager.IsVstEditorOpen(first));
 ASSERT_TRUE(manager.ToggleVstEditorForPlugin(first));
 EXPECT_FALSE(manager.IsVstEditorOpen(first));
 EXPECT_TRUE(manager.IsVstEditorOpen(second));
 SendMessage(second->Parent, WM_CLOSE, 0, 0);
 EXPECT_FALSE(manager.IsVstEditorOpen(second));
 EXPECT_EQ(first->Closes, 1u);
 EXPECT_EQ(second->Closes, 1u);
 ASSERT_TRUE(manager.ToggleVstEditorForPlugin(second));
 EXPECT_TRUE(manager.IsVstEditorOpen(second));
}

TEST(VstStaging, DISABLED_RackEditorRejectsReopenDuringManagerAndTitleBarClose)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<RackEditorTestPlugin>();
 bool reopen = true;
 plugin->DuringClose = [&] {
  manager.PruneClosedVstEditorWindows();
  reopen = manager.ToggleVstEditorForPlugin(plugin);
 };
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 ASSERT_TRUE(manager.CloseVstEditorForPlugin(plugin));
 EXPECT_FALSE(reopen);
 EXPECT_EQ(plugin->Opens, 1u);
 ASSERT_TRUE(manager.OpenVstEditorForPlugin(plugin));
 SendMessage(plugin->Parent, WM_CLOSE, 0, 0);
 EXPECT_FALSE(reopen);
 EXPECT_EQ(plugin->Opens, 2u);
}

TEST(VstStaging, DISABLED_RackEditorCloseCancelsAnOpenPumpingNativeMessages)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<RackEditorTestPlugin>();
 plugin->DuringOpen = [&] { EXPECT_TRUE(manager.CloseVstEditorForPlugin(plugin)); };
 EXPECT_FALSE(manager.OpenVstEditorForPlugin(plugin));
 EXPECT_FALSE(manager.IsVstEditorOpen(plugin));
 EXPECT_FALSE(IsWindow(plugin->Parent));
 EXPECT_TRUE(plugin->IsLoaded());
 plugin->DuringOpen = {};
 EXPECT_TRUE(manager.OpenVstEditorForPlugin(plugin));
}

TEST(VstStaging, DISABLED_RackEditorFailureLeavesProcessorLoadedAndCanRetry)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<RackEditorTestPlugin>();
 plugin->FailOpen = true;
 EXPECT_FALSE(manager.ToggleVstEditorForPlugin(plugin));
 EXPECT_TRUE(plugin->IsLoaded());
 EXPECT_FALSE(manager.IsVstEditorOpen(plugin));
 plugin->FailOpen = false;
 EXPECT_TRUE(manager.ToggleVstEditorForPlugin(plugin));
}

TEST(VstStaging, DISABLED_RackEditorPreferredSizeFitsMonitorWorkAreaAfterReopen)
{
 vst::VstEditorWindowManager manager;
 auto plugin = std::make_shared<RackEditorTestPlugin>();
 plugin->PreferredSize = {30000, 30000};
 for (unsigned int attempt = 0u; attempt < 2u; ++attempt)
 {
  ASSERT_TRUE(manager.ToggleVstEditorForPlugin(plugin));
  RECT rect{};
  ASSERT_TRUE(GetWindowRect(plugin->Parent, &rect));
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  ASSERT_TRUE(GetMonitorInfo(MonitorFromWindow(plugin->Parent, MONITOR_DEFAULTTONEAREST), &monitor));
  EXPECT_GE(rect.left, monitor.rcWork.left);
  EXPECT_GE(rect.top, monitor.rcWork.top);
  EXPECT_LE(rect.right, monitor.rcWork.right);
  EXPECT_LE(rect.bottom, monitor.rcWork.bottom);
  ASSERT_TRUE(manager.CloseVstEditorForPlugin(plugin));
 }
}

class RackControlsRemoteStation final : public engine::Station
{
public:
 RackControlsRemoteStation(engine::StationParams params, audio::AudioMixerParams mixer)
  : engine::Station(params, mixer) {}
 bool IsRemote() const noexcept override { return true; }
};

class VstRackControlsTest : public testing::Test
{
protected:
 std::mutex Mutex;
 std::vector<std::shared_ptr<engine::Station>> Stations;
 vst::VstEditorWindowManager Editors;
 std::shared_ptr<engine::Station> Station;
 std::shared_ptr<LoopTake> Take;
 std::shared_ptr<Loop> First;
 std::shared_ptr<Loop> Second;
 std::unique_ptr<gui::VstRackControls> Controls;
 std::function<std::wstring()> Pick;
 std::vector<std::wstring> Errors;
 unsigned int Picks = 0u;
 unsigned int Commits = 0u;
 bool Enabled = true;

 void SetUp() override
 {
  engine::StationParams params;
  params.Id = "rack-controls-station";
  params.Size = {220, 220};
  Station = std::make_shared<engine::Station>(params,
   engine::Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
  Take = MakeTake("rack-controls-take");
  First = MakeLoop();
  Second = MakeLoop();
  Take->AddLoop(First);
  Take->AddLoop(Second);
  Station->AddTake(Take);
  Stations.push_back(Station);
  Station->CommitChanges();
  Controls = std::make_unique<gui::VstRackControls>(gui::VstRackControls::Host{
   Mutex, Stations, Editors, [&] { return Enabled; }, [&] { ++Commits; },
   [&] { ++Picks; return Pick ? Pick() : std::wstring{}; },
   [] { return false; }, [&](const std::wstring& error) { Errors.push_back(error); }});
 }
 void Update(base::SelectDepth depth = base::DEPTH_LOOPTAKE)
 {
  std::scoped_lock lock(Mutex);
  Station->SetSelectDepth(depth);
  Take->SetSelectDepth(depth);
  Controls->UpdateLocked(depth);
 }
 void TearDown() override
 {
  Editors.CloseAllVstEditorWindows();
  Controls.reset();
  if (Station) Station->DetachGuiOwnershipForRetirement();
 }
 static void Click(const std::shared_ptr<gui::GuiRack>& rack)
 {
  actions::GuiAction action;
  action.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
  action.Index = gui::GuiRack::VstControlActionIndex;
  rack->OnAction(action);
 }
};

TEST_F(VstRackControlsTest, PickerKeepsCapturedLoopWhenSelectionAndDepthChange)
{
 First->Select();
 Second->DeSelect();
 Update(base::DEPTH_LOOP);
 ASSERT_TRUE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
 Pick = [&] {
  // A modal picker pumps the UI; assert it does not own the Scene lock.
  const bool ownsLock = Mutex.try_lock();
  EXPECT_TRUE(ownsLock);
  if (ownsLock) Mutex.unlock();
  First->DeSelect();
  Second->Select();
  Update(base::DEPTH_LOOPTAKE);
  return std::wstring{L"captured-loop.vst3"};
 };
 Click(Take->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 1u);
 EXPECT_EQ(Commits, 1u);
 EXPECT_TRUE(First->GetVstRackSlot().Pending);
 EXPECT_FALSE(Second->GetVstRackSlot().Pending);
 EXPECT_FALSE(Take->GetVstRackSlot().Pending);
 auto jobs = First->CommitChanges();
 ASSERT_EQ(jobs.size(), 1u);
 EXPECT_EQ(jobs.front().VstPath, L"captured-loop.vst3");
}

TEST_F(VstRackControlsTest, PickerCancellationLeavesOwnerReadyAndUnchanged)
{
 Update();
 Click(Take->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 1u);
 EXPECT_EQ(Commits, 0u);
 EXPECT_FALSE(Take->GetVstRackSlot().Pending);
 EXPECT_TRUE(Take->GetVstRackSlot().Identity.empty());
 Update();
 EXPECT_TRUE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
}

TEST_F(VstRackControlsTest, TargetRemovedDuringPickerDoesNotQueueLoad)
{
 Update();
 Pick = [&] { Stations.clear(); return std::wstring{L"removed-target.vst3"}; };
 Click(Take->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 1u);
 EXPECT_EQ(Commits, 0u);
 EXPECT_FALSE(Take->GetVstRackSlot().Pending);
 EXPECT_TRUE(Take->GetVstRackSlot().Identity.empty());
}

TEST_F(VstRackControlsTest, DisabledAndAmbiguousLoopRacksDoNotOpenPicker)
{
 First->DeSelect();
 Second->DeSelect();
 Update(base::DEPTH_LOOP);
 EXPECT_FALSE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
 Click(Take->GetGuiRack());
 Controls->Pump();
 First->Select();
 Second->Select();
 Update(base::DEPTH_LOOP);
 EXPECT_FALSE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
 Click(Take->GetGuiRack());
 Controls->Pump();
 Enabled = false;
 Update();
 EXPECT_FALSE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
 Click(Take->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 0u);
 EXPECT_EQ(Commits, 0u);
}

TEST_F(VstRackControlsTest, RemoteStationRackIsReadOnly)
{
 engine::StationParams params;
 params.Id = "rack-controls-remote";
 params.Size = {220, 220};
 auto remote = std::make_shared<RackControlsRemoteStation>(params,
  engine::Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
 Stations.push_back(remote);
 Update();
 ASSERT_NE(remote->GetGuiRack()->GetVstControl(), nullptr);
 EXPECT_FALSE(remote->GetGuiRack()->GetVstControl()->IsEnabled());
 Click(remote->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 0u);
 EXPECT_FALSE(remote->GetVstRackSlot().Pending);
}

class RackControlsFailedPlugin final : public StateGatePlugin
{
public:
 bool Load(const std::wstring&, float, unsigned, unsigned, vst::HostedLayoutMode) override { return false; }
};

TEST_F(VstRackControlsTest, FailedSlotsShowRetryAtEachHierarchyLevel)
{
 const auto fail = [](const auto& owner, const wchar_t* path) {
  JobAction job;
  job.JobActionType = JobAction::JOB_LOADVST;
  job.VstPath = path;
  job.PreInitPlugin = std::make_shared<RackControlsFailedPlugin>();
  owner->OnAction(job);
  owner->CommitChanges();
 };
 fail(Station, L"station-missing.vst3");
 fail(Take, L"take-missing.vst3");
 fail(First, L"loop-missing.vst3");

 First->Select();
 Second->DeSelect();
 Update(base::DEPTH_LOOPTAKE);
 ASSERT_NE(Station->GetGuiRack()->VstRack()->SlotButton(0u), nullptr);
 EXPECT_EQ(Station->GetGuiRack()->VstRack()->SlotButton(0u)->Text(), "Retry station-missing");
 ASSERT_NE(Take->GetGuiRack()->GetVstControl(), nullptr);
 EXPECT_EQ(Take->GetGuiRack()->GetVstControl()->Text(), "Retry VST");
 Update(base::DEPTH_LOOP);
 ASSERT_NE(Take->GetGuiRack()->GetVstControl(), nullptr);
 EXPECT_EQ(Take->GetGuiRack()->GetVstControl()->Text(), "Retry VST L1");
}

TEST_F(VstRackControlsTest, FailedLoadReportsErrorAndRemainsReplaceable)
{
 Pick = [] { return std::wstring{L"failed-rack.vst3"}; };
 Update();
 Click(Take->GetGuiRack());
 Controls->Pump();
 auto jobs = Take->CommitChanges();
 const auto job = std::find_if(jobs.begin(), jobs.end(), [](const auto& item) {
  return item.JobActionType == JobAction::JOB_LOADVST;
 });
 ASSERT_NE(job, jobs.end());
 auto failed = *job;
 failed.PreInitPlugin = std::make_shared<RackControlsFailedPlugin>();
 Take->OnAction(failed);
 Take->CommitChanges();
 Controls->Pump();
 ASSERT_EQ(Errors.size(), 1u);
 EXPECT_NE(Errors.front().find(L"failed-rack.vst3"), std::wstring::npos);
 EXPECT_TRUE(Take->GetVstRackSlot().Failed);
 EXPECT_FALSE(Take->GetVstRackSlot().Pending);
 EXPECT_FALSE(Take->GetVstRackSlot().Identity.empty());
 Update();
 EXPECT_TRUE(Take->GetGuiRack()->GetVstControl()->IsEnabled());
 Pick = [] { return std::wstring{L"replacement-rack.vst3"}; };
 Click(Take->GetGuiRack());
 Controls->Pump();
 EXPECT_EQ(Picks, 2u);
 EXPECT_EQ(Commits, 2u);
 EXPECT_TRUE(Take->GetVstRackSlot().Pending);
}

TEST_F(VstRackControlsTest, DISABLED_StationRemovalClosesEditorsAfterFailedSlotsAcrossHierarchy)
{
 const auto install = [](const auto& owner) {
  JobAction failed;
  failed.JobActionType = JobAction::JOB_LOADVST;
  failed.VstPath = L"missing-first.vst3";
  failed.PreInitPlugin = std::make_shared<RackControlsFailedPlugin>();
  owner->OnAction(failed);
  owner->CommitChanges();
  auto plugin = std::make_shared<RackEditorTestPlugin>();
  JobAction loaded;
  loaded.JobActionType = JobAction::JOB_LOADVST;
  loaded.VstPath = L"live-second.vst3";
  loaded.PreInitPlugin = plugin;
  owner->OnAction(loaded);
  owner->CommitChanges();
  return plugin;
 };
 const auto stationPlugin = install(Station);
 const auto takePlugin = install(Take);
 const auto loopPlugin = install(First);
 ASSERT_EQ(Station->GetVstPlugin(0u), nullptr);
 ASSERT_EQ(Take->GetVstPlugin(0u), nullptr);
 ASSERT_EQ(First->GetVstPlugin(0u), nullptr);
 ASSERT_TRUE(Editors.OpenVstEditorForPlugin(stationPlugin));
 ASSERT_TRUE(Editors.OpenVstEditorForPlugin(takePlugin));
 ASSERT_TRUE(Editors.OpenVstEditorForPlugin(loopPlugin));
 Editors.CloseVstEditorsForStation(Station);
 EXPECT_FALSE(Editors.IsVstEditorOpen(stationPlugin));
 EXPECT_FALSE(Editors.IsVstEditorOpen(takePlugin));
 EXPECT_FALSE(Editors.IsVstEditorOpen(loopPlugin));
 ASSERT_TRUE(Editors.OpenVstEditorForPlugin(loopPlugin));
 Editors.CloseVstEditorsForTake(Take);
 EXPECT_FALSE(Editors.IsVstEditorOpen(loopPlugin));
}

TEST_F(VstRackControlsTest, PresentedRackDoesNotRetainItsOwnerHierarchy)
{
 Update();
 std::weak_ptr<engine::Station> weakStation = Station;
 std::weak_ptr<LoopTake> weakTake = Take;
 Controls.reset();
 Station->DetachGuiOwnershipForRetirement();
 Stations.clear();
 Station.reset();
 Take.reset();
 First.reset();
 Second.reset();
 EXPECT_TRUE(weakStation.expired());
 EXPECT_TRUE(weakTake.expired());
}
