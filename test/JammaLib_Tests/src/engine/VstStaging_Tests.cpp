
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
#include <sstream>

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

class RackDropPlugin final : public StateGatePlugin
{
public:
 vst::MidiInputCapabilities MidiInput() const noexcept override { return {true, true, true}; }
};

TEST(VstStaging, MidiCableDropAcceptsEntireVisibleSlotControl)
{
 gui::GuiRackParams params; params.Size = {220u, 300u};
 params.EnableVstControl = true; params.EnableVstRack = true; params.EnableVstRackMidiRouting = true;
 auto rack = std::make_shared<gui::GuiRack>(params); rack->SetPosition({300, 400}); rack->Init();
 const auto view = rack->VstRack(); ASSERT_TRUE(view);
 view->SetViewport({1200u, 1000u}); view->SetChannelsCollapsed(false);
 gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 1u; model.TotalSlots = 1u;
 gui::GuiVstRack::Slot slot; slot.Value.Identity = "target"; slot.Value.Plugin = std::make_shared<RackDropPlugin>();
 model.Slots.push_back(slot);
 std::vector<gui::GuiVstRack::Command> commands;
 view->Update(model, [&](auto command) { commands.push_back(std::move(command)); });
 view->SetVisible(true);
 view->Layout(100u, 0, {0, 0}, {20u, 20u});
 std::vector<gui::CableInteraction::Endpoint> endpoints; std::vector<gui::CableInteraction::Cable> cables;
 view->BuildInteractionGeometry(endpoints, cables); ASSERT_EQ(17u, endpoints.size());
 ASSERT_TRUE(endpoints.back().Available);
 const auto button = view->SlotButton(0u); ASSERT_TRUE(button);
 const auto p = button->GlobalPosition() - view->GlobalPosition(); const auto size = button->GetSize();
 for (const auto offset : {utils::Position2d{static_cast<int>(size.Width) - 6, 6},
                          utils::Position2d{static_cast<int>(size.Width) - 6, static_cast<int>(size.Height) - 6}})
 {
  ASSERT_TRUE(view->SlotScroll()->EffectiveContentRect({0, 0, 1200, 1000}).Contains(button->GlobalPosition() + offset));
  actions::TouchAction touch; touch.Index = 0; touch.Touch = actions::TouchAction::TOUCH_MOUSE;
  touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = endpoints[0].Position;
  view->OnAction(touch); ASSERT_TRUE(view->HasCableDrag());
  touch.State = actions::TouchAction::TOUCH_UP; touch.Position = p + offset;
  EXPECT_FALSE(gui::CableInteraction::HitTest(endpoints.back(), touch.Position, 13.0f));
  view->OnAction(touch); EXPECT_FALSE(view->HasCableDrag());
 }
 ASSERT_EQ(2u, commands.size());
 for (const auto& command : commands)
 {
  EXPECT_EQ((std::vector<std::string>{"target"}), command.ChannelRoutes[0].PluginIds);
 }
 rack->DetachGuiOwnershipForRetirement();
}

TEST(VstStaging, EveryMidiChannelCellDropsOntoVisibleScrolledSlotAndRejectsStaleDrag)
{
 gui::GuiRackParams params; params.Size = {220u,300u};
 params.EnableVstControl = true; params.EnableVstRack = true; params.EnableVstRackMidiRouting = true;
 auto rack = std::make_shared<gui::GuiRack>(params); rack->SetPosition({300,400}); rack->Init();
 const auto view = rack->VstRack(); ASSERT_TRUE(view);
 view->SetViewport({1200u,1000u}); view->SetChannelsCollapsed(false);
 gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 7u; model.TotalSlots = 12u;
 for (auto i = 0u; i < 12u; ++i)
 {
  gui::GuiVstRack::Slot slot; slot.Value.Identity = "slot-" + std::to_string(i);
  slot.Value.Plugin = std::make_shared<RackDropPlugin>(); model.Slots.push_back(std::move(slot));
 }
 std::vector<gui::GuiVstRack::Command> commands;
 const auto submit = [&](auto command) { commands.push_back(std::move(command)); };
 view->Update(model,submit); view->SetVisible(true);
 view->Layout(100u,0,{0,0},{20u,20u});
 ASSERT_GE(view->SlotScroll()->MaxScrollOffset(),80);
 view->SlotScroll()->SetScrollOffset(80);
 const auto button = view->SlotButton(3u); ASSERT_TRUE(button);
 const auto clip = view->SlotScroll()->EffectiveContentRect({0,0,1200,1000});
 const auto p = button->GlobalPosition(); const auto size = button->GetSize();
 const auto visible = utils::Rect2d{p.X,p.Y,p.X + static_cast<int>(size.Width),p.Y + static_cast<int>(size.Height)}.Intersected(clip);
 ASSERT_FALSE(visible.IsEmpty());
 const auto drop = utils::Position2d{visible.Right - 6,(visible.Bottom + visible.Top) / 2} - view->GlobalPosition();
 std::vector<gui::CableInteraction::Endpoint> endpoints; std::vector<gui::CableInteraction::Cable> cables;
 view->BuildInteractionGeometry(endpoints,cables); ASSERT_EQ(28u,endpoints.size());
 EXPECT_FALSE(gui::CableInteraction::HitTest(endpoints[19u],drop,13.0f));
 for (auto channel = 0u; channel < 16u; ++channel)
 {
  SCOPED_TRACE(channel);
  actions::TouchAction touch; touch.Index = 0; touch.Touch = actions::TouchAction::TOUCH_MOUSE;
  touch.State = actions::TouchAction::TOUCH_DOWN;
  touch.Position = endpoints[channel].Position + utils::Position2d{-20,0};
  const auto pressed = view->OnAction(touch);
  EXPECT_EQ(view,pressed.ActiveElement.lock()); EXPECT_FALSE(view->HasCableDrag());
  actions::TouchMoveAction move; move.Index = 0; move.Touch = actions::TouchAction::TOUCH_MOUSE;
  move.MouseButtonsDown = 1u; move.Position = touch.Position + utils::Position2d{-6,0};
  view->OnAction(move); ASSERT_TRUE(view->HasCableDrag());
  touch.State = actions::TouchAction::TOUCH_UP; touch.Position = drop;
  view->OnAction(touch); EXPECT_FALSE(view->HasCableDrag());
  ASSERT_EQ(channel + 1u,commands.size());
  EXPECT_EQ(7u,commands.back().Revision);
  for (auto routed = 0u; routed < 16u; ++routed)
  {
   EXPECT_EQ(routed == channel ? (std::vector<std::string>{"slot-3"}) : std::vector<std::string>{},
       commands.back().ChannelRoutes[routed].PluginIds);
  }
  EXPECT_EQ(80,view->SlotScroll()->ScrollOffset());
 }
 actions::TouchAction touch; touch.Index = 0; touch.Touch = actions::TouchAction::TOUCH_MOUSE;
 touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = endpoints[0].Position;
 view->OnAction(touch); ASSERT_TRUE(view->HasCableDrag());
 ++model.Revision; view->Update(model,submit); EXPECT_FALSE(view->HasCableDrag());
 touch.State = actions::TouchAction::TOUCH_UP; touch.Position = drop; view->OnAction(touch);
 EXPECT_EQ(16u,commands.size());
 rack->DetachGuiOwnershipForRetirement();
}

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
 gui::RuntimeDialogDecision Decision;

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
   [&](std::string, std::string, std::string) {
    Decision = std::make_shared<std::optional<bool>>(); return Decision;
   }, [&](const std::wstring& error) { Errors.push_back(error); }});
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
 static void Click(const std::shared_ptr<gui::GuiRack>& rack, base::Action::Modifiers modifiers = base::Action::MODIFIER_NONE)
 {
  actions::GuiAction action;
  action.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
  action.Index = gui::GuiRack::VstControlActionIndex;
  action.InputModifiers = modifiers;
  rack->OnAction(action);
 }
};

TEST_F(VstRackControlsTest, RemovalWaitsForDecisionAndCancellationPreservesSlot)
{
 JobAction installed; installed.JobActionType = JobAction::JOB_LOADVST;
 installed.VstPath = L"captured.vst3"; installed.VstIdentity = "captured";
 installed.PreInitPlugin = std::make_shared<StateGatePlugin>();
 Take->OnAction(installed); Take->CommitChanges();
 Update(); Click(Take->GetGuiRack(),base::Action::MODIFIER_ALT); Controls->Pump();
 ASSERT_TRUE(Decision); EXPECT_FALSE(Decision->has_value());
 Controls->Pump(); EXPECT_EQ(0u,Commits); EXPECT_EQ("captured",Take->GetVstRackSlot().Identity);
 *Decision = false; Controls->Pump();
 EXPECT_EQ(0u,Commits); EXPECT_EQ("captured",Take->GetVstRackSlot().Identity);
 Update(); Click(Take->GetGuiRack(),base::Action::MODIFIER_ALT); Controls->Pump();
 ASSERT_TRUE(Decision); *Decision = true; Controls->Pump();
 EXPECT_EQ(1u,Commits);
}

TEST_F(VstRackControlsTest, DelayedRemovalRejectsReplacementSlotAndRemovedOwner)
{
 for (const bool removeOwner : {false,true})
 {
  SCOPED_TRACE(removeOwner);
  JobAction installed; installed.JobActionType = JobAction::JOB_LOADVST;
  installed.VstPath = L"captured.vst3"; installed.VstIdentity = "captured";
  installed.PreInitPlugin = std::make_shared<StateGatePlugin>();
  Take->OnAction(installed); Take->CommitChanges();
  Update(); Click(Take->GetGuiRack(),base::Action::MODIFIER_ALT); Controls->Pump();
  ASSERT_TRUE(Decision); EXPECT_FALSE(Decision->has_value());
  if (removeOwner) Stations.clear();
  else
  {
   JobAction removed; removed.JobActionType = JobAction::JOB_UNLOADVST; removed.VstIdentity = "captured";
   Take->OnAction(removed); Take->CommitChanges();
   installed.VstIdentity = "replacement"; installed.PreInitPlugin = std::make_shared<StateGatePlugin>();
   Take->OnAction(installed); Take->CommitChanges();
  }
  *Decision = true; Controls->Pump(); EXPECT_EQ(0u,Commits);
  if (!removeOwner)
  {
   EXPECT_EQ("replacement",Take->GetVstRackSlot().Identity);
   JobAction removed; removed.JobActionType = JobAction::JOB_UNLOADVST; removed.VstIdentity = "replacement";
   Take->OnAction(removed); Take->CommitChanges();
  }
 }
}

TEST_F(VstRackControlsTest, DelayedStationRemovalRejectsChangedRackRevisionAndResetCancelsToken)
{
 JobAction installed; installed.JobActionType = JobAction::JOB_LOADVST;
 installed.VstPath = L"captured.vst3"; installed.VstIdentity = "captured";
 installed.PreInitPlugin = std::make_shared<StateGatePlugin>();
 Station->OnAction(installed); Station->CommitChanges(); Update();
 const auto view = Station->GetGuiRack()->VstRack(); ASSERT_TRUE(view);
 const auto button = view->SlotButton(0u); ASSERT_TRUE(button);
 actions::TouchAction touch; touch.Index = 0; touch.Touch = actions::TouchAction::TOUCH_MOUSE;
 touch.Modifiers = base::Action::MODIFIER_ALT; touch.Position = {10,10};
 touch.State = actions::TouchAction::TOUCH_DOWN; button->OnAction(touch);
 touch.State = actions::TouchAction::TOUCH_UP; button->OnAction(touch); Controls->Pump();
 ASSERT_TRUE(Decision); EXPECT_FALSE(Decision->has_value());
 const auto revision = Station->RackRevision();
 installed.VstIdentity = "other"; installed.PreInitPlugin = std::make_shared<StateGatePlugin>();
 Station->OnAction(installed); Station->CommitChanges(); ASSERT_NE(revision,Station->RackRevision());
 *Decision = true; Controls->Pump(); EXPECT_EQ(0u,Commits);
 EXPECT_EQ(2u,Station->RackSlotCount());
 Update();
 touch.State = actions::TouchAction::TOUCH_DOWN; view->SlotButton(0u)->OnAction(touch);
 touch.State = actions::TouchAction::TOUCH_UP; view->SlotButton(0u)->OnAction(touch); Controls->Pump();
 ASSERT_TRUE(Decision); EXPECT_FALSE(Decision->has_value());
 Controls->Reset(); ASSERT_TRUE(Decision->has_value()); EXPECT_FALSE(Decision->value());
}

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

TEST_F(VstRackControlsTest, FailedInteractiveAppendsRestoreEmptyRackAndLeaveOnlyDialogOutcomes)
{
	const std::wstring first = L"invalid-first.vst3";
	const std::wstring second = L"invalid-second.vst3";
	ASSERT_TRUE(Station->LoadManagedVstSlot(first, {}, true));
	ASSERT_TRUE(Station->LoadManagedVstSlot(second, {}, true));
	const auto pending = Station->GetVstRackSlots();
	ASSERT_EQ(2u, pending.size());
	const auto firstId = pending[0].Identity, secondId = pending[1].Identity;
	const auto jobs = Station->CommitChanges();
	unsigned loadCount = 0;
	for (auto job : jobs) {
		if (job.JobActionType != JobAction::JOB_LOADVST) continue;
		job.PreInitPlugin = std::make_shared<RackControlsFailedPlugin>();
		Station->OnAction(job);
		++loadCount;
	}
	ASSERT_EQ(2u, loadCount);
	Station->CommitChanges();
	EXPECT_TRUE(Station->GetVstRackSlots().empty());
	EXPECT_TRUE(Station->VstEntries().empty());
	EXPECT_TRUE(Station->RackDiagnostic().empty());
	EXPECT_EQ(Station->TakeFailedInteractiveVstLoad(firstId), first);
	EXPECT_EQ(Station->TakeFailedInteractiveVstLoad(secondId), second);
	EXPECT_FALSE(Station->TakeFailedInteractiveVstLoad(firstId).has_value());
}

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
 EXPECT_EQ(Take->GetGuiRack()->VstRack()->SlotButton(0u)->Text(), "Retry take-missing");
 Update(base::DEPTH_LOOP);
 ASSERT_NE(Take->GetGuiRack()->GetVstControl(), nullptr);
 EXPECT_EQ(Take->GetGuiRack()->VstRack()->SlotButton(0u)->Text(), "Retry loop-missing");
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

TEST_F(VstRackControlsTest, FailedStationAppendShowsErrorWithoutRedSlotOrRackInfo)
{
	auto existing = std::make_shared<StateGatePlugin>();
	JobAction installed; installed.JobActionType = JobAction::JOB_LOADVST;
	installed.VstPath = L"existing.vst3"; installed.PreInitPlugin = existing;
	Station->OnAction(installed); Station->CommitChanges();
	Pick = [] { return std::wstring{L"invalid-station.vst3"}; };
	Update();
	auto view = Station->GetGuiRack()->VstRack(); ASSERT_TRUE(view);
	auto append = view->AppendButton(); ASSERT_TRUE(append);
	actions::TouchAction down; down.Touch = actions::TouchAction::TOUCH_MOUSE; down.Index = 0;
	down.State = actions::TouchAction::TOUCH_DOWN; down.Position = {10,10};
	append->OnAction(down);
	auto up = down; up.State = actions::TouchAction::TOUCH_UP; append->OnAction(up);
	Controls->Pump();
	const auto jobs = Station->CommitChanges();
	const auto load = std::find_if(jobs.begin(),jobs.end(),[](const auto& job) { return job.JobActionType == JobAction::JOB_LOADVST; });
	ASSERT_NE(load,jobs.end());
	auto failed = *load; failed.PreInitPlugin = std::make_shared<RackControlsFailedPlugin>();
	Station->OnAction(failed); Station->CommitChanges();
	Controls->Pump(); Update();
	ASSERT_EQ(1u,Errors.size());
	EXPECT_NE(std::wstring::npos,Errors[0].find(L"invalid-station.vst3"));
	EXPECT_EQ(1u,Station->RackSlotCount());
	EXPECT_EQ(existing,Station->GetVstRackSlots()[0].Plugin);
	EXPECT_TRUE(view->SlotButton(0u));
	EXPECT_FALSE(view->SlotButton(1u));
	EXPECT_FALSE(view->TryGetChild(6u)->IsVisible()); // No stale Rack info footer.
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

class RackOrderPlugin final : public StateGatePlugin
{
public:
 vst::RenderRole Render = vst::RenderRole::Effect;
 float Value = 2.0f;
 std::atomic<bool> Hold{false}, Entered{false}, Release{false};
 std::atomic<unsigned int> Calls{0u}, Concurrent{0u}, Overlaps{0u};
 mutable unsigned int Captures = 0u;
 unsigned int Restores = 0u;
 void ProcessBlock(float* channel, int32_t samples) noexcept override { float* channels[]{channel}; ProcessBlockMulti(channels,1,samples); }
 vst::RenderRole Role() const noexcept override { return Render; }
 vst::MidiInputCapabilities MidiInput() const noexcept override { return {true,true,true}; }
 unsigned int HostedChannelCount() const noexcept override { return 2u; }
 void ProcessBlockMulti(float* const* channels, int32_t count, int32_t samples) noexcept override
 {
  if (Concurrent.fetch_add(1u) != 0u) ++Overlaps;
  ++Calls;
  if (Hold.exchange(false))
  {
   Entered.store(true, std::memory_order_release);
   // Test-owned bounded rendezvous; production callbacks never wait.
   for (unsigned int attempt = 0u; attempt < 10000000u && !Release.load(std::memory_order_acquire); ++attempt) std::this_thread::yield();
  }
  for (int32_t channel = 0; channel < count; ++channel)
   for (int32_t sample = 0; sample < samples; ++sample)
    channels[channel][sample] = Render == vst::RenderRole::Instrument ? Value : channels[channel][sample] * Value;
  Concurrent.fetch_sub(1u);
 }
 std::vector<std::uint8_t> GetState() const override { ++Captures; return {static_cast<std::uint8_t>(Value)}; }
 void SetState(const std::vector<std::uint8_t>&) override { ++Restores; }
};

class RackOrderStation final : public engine::Station
{
public:
 explicit RackOrderStation(engine::StationParams params) : Station(params, Station::GetMixerParams(params.Size, MergeMixBehaviourParams{})) {}
 auto Borrow() const noexcept { return _BorrowAudioState(); }
 float RenderBlock() noexcept
 {
  const auto state = Borrow();
  if (!state || !state->Midi.MidiChain || state->VstRenderChannels == 0u) return -1.0f;
  for (auto* channel : state->VstBlockPtrs) std::fill_n(channel, 8u, 0.0f);
  state->Midi.MidiChain->ProcessRack(state->VstBlockPtrs.data(), state->InstrumentPtrs.data(), state->VstRenderChannels, 8u);
  return state->VstBlockPtrs[0][0];
 }
};

TEST(VstStaging, RackMovePublishesMatchingIdentityDestinationsAndOrderDuringAnActiveBlock)
{
 engine::StationParams params; params.Id = "reorder-station"; params.Size = {200u,200u}; params.NumBusChannels = 2u;
 auto station = std::make_shared<RackOrderStation>(params);
 const auto a = std::make_shared<RackOrderPlugin>(); a->Render = vst::RenderRole::Instrument; a->Value = 1.0f;
 const auto effect = std::make_shared<RackOrderPlugin>();
 const auto b = std::make_shared<RackOrderPlugin>(); b->Render = vst::RenderRole::Instrument; b->Value = 3.0f;
 const std::array plugins{a,effect,b};
 const std::array<std::string,3> ids{"instrument-a","effect","instrument-b"};
 for (auto i = 0u; i < plugins.size(); ++i)
 {
  JobAction load; load.JobActionType = JobAction::JOB_LOADVST; load.VstPath = L"test-owned.vst3";
  load.VstIdentity = ids[i]; load.PreInitPlugin = plugins[i]; station->OnAction(load);
 }
 station->CommitChanges();
 midi::MidiChannelRoute cable; cable.PluginIds = {"instrument-b"};
 ASSERT_TRUE(station->SetMidiChannelRoute(0u,cable,station->RackRevision()));
 const auto old = station->Borrow(); ASSERT_TRUE(old); ASSERT_EQ(3u,old->Midi.MidiDestinations.size());
 const auto destination = old->Midi.MidiDestinations[2];
 ASSERT_EQ(destination,old->Midi.LiveChannelDestinations[0][0]);
 a->Hold = true;
 float oldOutput = 0.0f;
 std::thread callback([&] { oldOutput = station->RenderBlock(); });
 for (auto attempt = 0u; attempt < 1000000u && !a->Entered.load(std::memory_order_acquire); ++attempt) std::this_thread::yield();
 const bool entered = a->Entered.load(std::memory_order_acquire);
 vst::RackMove move{vst::RackMove::Scope::Station,station->Id(),"effect",1u,2u,station->RackRevision()};
 const bool moved = station->MoveVstSlot(move);
 station->CommitChanges();
 a->Release.store(true,std::memory_order_release); callback.join();
 ASSERT_TRUE(entered); ASSERT_TRUE(moved);
 EXPECT_FLOAT_EQ(5.0f,oldOutput); // A -> effect -> B is still the old immutable order.
 EXPECT_FLOAT_EQ(8.0f,station->RenderBlock()); // A + B -> effect, using the same processors.
 const auto current = station->Borrow(); ASSERT_TRUE(current);
 EXPECT_EQ(effect,current->Midi.MidiChain->GetPlugin(2u));
 EXPECT_EQ(effect,old->Midi.MidiChain->GetPlugin(1u));
 EXPECT_EQ(destination,current->Midi.MidiDestinations[1]);
 EXPECT_EQ(destination,current->Midi.LiveChannelDestinations[0][0]);
 EXPECT_TRUE(current->Midi.RetiredMidiDestinations.empty());
 EXPECT_EQ(cable,station->MidiChannelRoutes()[0]);
 for (const auto& plugin : plugins)
 {
  EXPECT_EQ(0u,plugin->Overlaps.load()); EXPECT_EQ(2u,plugin->Calls.load());
  EXPECT_EQ(0u,plugin->Captures); EXPECT_EQ(0u,plugin->Restores);
 }
 station->DetachGuiOwnershipForRetirement();
}

class RackMoveTest : public testing::Test
{
protected:
 static auto Entries(const std::shared_ptr<Loop>& owner) { return owner->ToJamFile("reorder.wav").VstChain; }
 template<class Owner> static auto Entries(const std::shared_ptr<Owner>& owner) { return owner->VstEntries(); }
 template<class Owner> void Check(const std::shared_ptr<Owner>& owner, vst::RackMove::Scope scope)
 {
  std::array<std::shared_ptr<RackOrderPlugin>,3> plugins;
  for (auto i = 0u; i < plugins.size(); ++i)
  {
   plugins[i] = std::make_shared<RackOrderPlugin>(); plugins[i]->Value = static_cast<float>(i + 1u);
   JobAction load; load.JobActionType = JobAction::JOB_LOADVST; load.VstPath = L"order-" + std::to_wstring(i) + L".vst3";
   load.VstIdentity = "order-" + std::to_string(i); load.PreInitPlugin = plugins[i]; owner->OnAction(load);
  }
  owner->CommitChanges();
  vst::RackMove move{scope,owner->Id(),"order-0",0u,2u,owner->RackRevision()};
  auto invalid = move; invalid.OwnerId = "other-owner"; EXPECT_FALSE(owner->MoveVstSlot(invalid));
  invalid = move; invalid.Source = 1u; EXPECT_FALSE(owner->MoveVstSlot(invalid));
  invalid = move; invalid.Target = 3u; EXPECT_FALSE(owner->MoveVstSlot(invalid));
  invalid = move; invalid.Target = 0u; EXPECT_FALSE(owner->MoveVstSlot(invalid));
  invalid = move; invalid.OwnerScope = scope == vst::RackMove::Scope::Station ? vst::RackMove::Scope::Loop : vst::RackMove::Scope::Station; EXPECT_FALSE(owner->MoveVstSlot(invalid));
  ASSERT_TRUE(owner->MoveVstSlot(move));
  EXPECT_EQ(plugins[0],owner->GetVstPlugin(0u)); // No in-place topology mutation before commit.
  EXPECT_FALSE(owner->MoveVstSlot(move)); // Captured revision cannot apply twice.
  owner->CommitChanges();
  EXPECT_EQ(plugins[1],owner->GetVstPlugin(0u)); EXPECT_EQ(plugins[2],owner->GetVstPlugin(1u)); EXPECT_EQ(plugins[0],owner->GetVstPlugin(2u));
  for (const auto& plugin : plugins) { EXPECT_EQ(0u,plugin->Captures); EXPECT_EQ(0u,plugin->Restores); }
  auto entries = Entries(owner); ASSERT_EQ(3u,entries.size());
  EXPECT_EQ("order-1",entries[0].Id); EXPECT_EQ("order-1.vst3",entries[0].Path); EXPECT_EQ((std::vector<std::uint8_t>{2u}),entries[0].DecodeState());
  EXPECT_EQ("order-0",entries[2].Id);
  // Exercise the supported JAM encoding at every owner scope, including blobs.
  io::JamFile jam; jam.Name = "rack-order";
  io::JamFile::Station savedStation{}; savedStation.Id = "saved-station"; savedStation.Name = "saved station";
  io::JamFile::LoopTake savedTake{}; savedTake.Id = "saved-take"; savedTake.Name = "saved take";
  io::JamFile::Loop savedLoop{}; savedLoop.Id = "saved-loop"; savedLoop.Name = "saved.wav"; savedLoop.Length = 8u; savedLoop.Speed = 1.0;
  if (scope == vst::RackMove::Scope::Station) savedStation.VstChain = entries;
  else if (scope == vst::RackMove::Scope::Take) savedTake.VstChain = entries;
  else savedLoop.VstChain = entries;
  savedTake.Loops.push_back(savedLoop); savedStation.LoopTakes.push_back(savedTake); jam.Stations.push_back(savedStation);
  std::stringstream encoded;
  ASSERT_TRUE(io::JamFile::ToStream(jam,encoded));
  const auto decoded = io::JamFile::FromStream(std::move(encoded)); ASSERT_TRUE(decoded);
  ASSERT_EQ(1u,decoded->Stations.size()); ASSERT_EQ(1u,decoded->Stations[0].LoopTakes.size());
  ASSERT_EQ(1u,decoded->Stations[0].LoopTakes[0].Loops.size());
  const auto& restored = scope == vst::RackMove::Scope::Station ? decoded->Stations[0].VstChain
   : scope == vst::RackMove::Scope::Take ? decoded->Stations[0].LoopTakes[0].VstChain
   : decoded->Stations[0].LoopTakes[0].Loops[0].VstChain;
  ASSERT_EQ(entries.size(),restored.size());
  for (auto i = 0u; i < entries.size(); ++i)
  { EXPECT_EQ(entries[i].Id,restored[i].Id); EXPECT_EQ(entries[i].Path,restored[i].Path); EXPECT_EQ(entries[i].State,restored[i].State); }
  move.Source = 2u; move.Target = 0u; move.Revision = owner->RackRevision();
  ASSERT_TRUE(owner->MoveVstSlot(move)); owner->CommitChanges();
  for (auto i = 0u; i < plugins.size(); ++i) EXPECT_EQ(plugins[i],owner->GetVstPlugin(i));
  owner->DetachGuiOwnershipForRetirement();
 }
};

TEST_F(RackMoveTest, StationTakeAndLoopKeepInstancesStateAndSavedOrderInBothDirections)
{
 engine::StationParams params; params.Id = "move-owner"; params.Size = {200u,200u};
 auto station = std::make_shared<engine::Station>(params,engine::Station::GetMixerParams(params.Size,MergeMixBehaviourParams{}));
 Check(station,vst::RackMove::Scope::Station);
 Check(MakeTake(),vst::RackMove::Scope::Take);
 Check(MakeLoop(),vst::RackMove::Scope::Loop);
}

TEST(VstStaging, SlotButtonDragPreservesClicksAndCancelsOutsideEscapeOrStaleOwner)
{
 gui::GuiRackParams params; params.Size = {220u,300u}; params.EnableVstControl = true; params.EnableVstRack = true;
 auto rack = std::make_shared<gui::GuiRack>(params); rack->SetPosition({300,700}); rack->Init();
 auto view = rack->VstRack(); view->SetVisible(true); view->SetViewport({1200u,1000u});
 gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 9u; model.OwnerId = "owner"; model.TotalSlots = 3u;
 for (auto i = 0u; i < 3u; ++i) { gui::GuiVstRack::Slot slot; slot.Value.Identity = "slot-" + std::to_string(i); model.Slots.push_back(slot); }
 std::vector<gui::GuiVstRack::Command> commands;
 const auto submit = [&](auto command) { commands.push_back(std::move(command)); };
 view->Update(model,submit); view->Layout(100u,0,{0,0},{20u,20u});
 const auto point = [&](std::size_t i) { return view->SlotButton(i)->GlobalPosition() - view->GlobalPosition() + utils::Position2d{40,18}; };
 actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0;
 const auto begin = [&] { touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = point(0u); EXPECT_TRUE(view->OnAction(touch).IsEaten); EXPECT_TRUE(view->HasSlotMove()); EXPECT_FALSE(view->HasCableDrag()); };
 EXPECT_EQ(view, view->FindTopmostDescendant(point(0u)));
 EXPECT_EQ(static_cast<unsigned int>(view->SlotScroll()->ContentRect().Right - view->SlotScroll()->ContentRect().Left), view->SlotButton(0u)->GetSize().Width);
 begin(); touch.State = actions::TouchAction::TOUCH_UP; view->OnAction(touch);
 ASSERT_EQ(1u, commands.size()); EXPECT_EQ(gui::GuiVstRack::Command::Kind::Slot, commands.back().Operation);
 commands.clear();
 begin(); touch.Position = point(2u);
 actions::TouchMoveAction motion; motion.Touch = touch.Touch; motion.Index = touch.Index; motion.MouseButtonsDown = 1u; motion.Position = touch.Position;
 view->OnAction(motion); view->Update(model, submit);
 EXPECT_FALSE(view->TryGetChild(6u)->IsVisible()); // Drag hints must not create a Rack info footer.
 touch.State = actions::TouchAction::TOUCH_UP; view->OnAction(touch);
 ASSERT_EQ(1u,commands.size()); EXPECT_EQ(0u,commands[0].Move.Source); EXPECT_EQ(2u,commands[0].Move.Target); EXPECT_EQ("slot-0",commands[0].Move.PluginId); EXPECT_EQ("owner",commands[0].Move.OwnerId);
 begin(); touch.State = actions::TouchAction::TOUCH_UP; touch.Position = {-1000,-1000}; view->OnAction(touch); EXPECT_EQ(1u,commands.size());
 begin(); actions::KeyAction escape; escape.KeyChar = 27u; escape.KeyActionType = actions::KeyAction::KEY_DOWN; view->OnAction(escape);
 touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point(2u); view->OnAction(touch); EXPECT_EQ(1u,commands.size());
 begin(); model.OwnerId = "replacement"; view->Update(model,submit); EXPECT_FALSE(view->HasSlotMove());
 touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point(2u); view->OnAction(touch); EXPECT_EQ(1u,commands.size());
 view->DetachGuiOwnershipForRetirement();
}

class RackRenderSink final : public base::MultiAudioSink
{
public:
 float First = 0.0f;
 unsigned int Writes = 0u;
 void OnBlockWriteChannel(unsigned int, const base::AudioWriteRequest& request, int) override
 { if (request.numSamps) { First += request.samples[0]; ++Writes; } }
};

class RackRenderLoop final : public Loop
{
public:
 static audio::AudioMixerParams Mixer(const LoopParams& params)
 {
  audio::WireMixBehaviourParams wire; wire.Channels = {0u}; return GetMixerParams(params.Size,wire);
 }
 explicit RackRenderLoop(LoopParams params) : Loop(params,Mixer(params))
 {
  _bufferBank.Resize(constants::MaxLoopFadeSamps + 64u);
  for (auto i = 0u; i < _bufferBank.Length(); ++i) _bufferBank[i] = 0.0f;
  _loopLength = 64u; _playIndex = constants::MaxLoopFadeSamps; _playState = STATE_PLAYING;
 }
};

class RackScopeRenderTest : public testing::Test
{
protected:
 template<class Owner, class Render> void Check(const std::shared_ptr<Owner>& owner, vst::RackMove::Scope scope, Render render)
 {
  auto a = std::make_shared<RackOrderPlugin>(); a->Render = vst::RenderRole::Instrument; a->Value = 1.0f;
  auto effect = std::make_shared<RackOrderPlugin>(); effect->Value = 2.0f;
  auto b = std::make_shared<RackOrderPlugin>(); b->Render = vst::RenderRole::Instrument; b->Value = 3.0f;
  const std::array plugins{a,effect,b};
  for (auto i = 0u; i < plugins.size(); ++i)
  {
   JobAction load; load.JobActionType = JobAction::JOB_LOADVST; load.VstPath = L"scope-test.vst3";
   load.VstIdentity = "scope-" + std::to_string(i); load.PreInitPlugin = plugins[i]; owner->OnAction(load);
  }
  owner->CommitChanges();
  EXPECT_FLOAT_EQ(5.0f,render()); // Instrument A -> effect -> additive B.
  vst::RackMove move{scope,owner->Id(),"scope-1",1u,2u,owner->RackRevision()};
  ASSERT_TRUE(owner->MoveVstSlot(move)); owner->CommitChanges();
  EXPECT_FLOAT_EQ(8.0f,render()); // Both instruments -> effect.
  move.Source = 2u; move.Target = 1u; move.Revision = owner->RackRevision();
  ASSERT_TRUE(owner->MoveVstSlot(move)); owner->CommitChanges();
  EXPECT_FLOAT_EQ(5.0f,render());
  for (const auto& plugin : plugins) EXPECT_EQ(3u,plugin->Calls.load());
  owner->DetachGuiOwnershipForRetirement();
 }
};

TEST_F(RackScopeRenderTest, TakeAndLoopPlaybackUseAdditiveInstrumentsAndOrderedEffects)
{
 auto sink = std::make_shared<RackRenderSink>();
 LoopParams params; params.Id = "render-loop"; params.Size = {100u,100u}; params.FadeSamps = 0u;
 auto loop = std::make_shared<RackRenderLoop>(params);
 Check(loop,vst::RackMove::Scope::Loop,[&] { sink->First = 0.0f; loop->WriteBlock(sink,nullptr,0,8u); return sink->First; });
 auto take = MakeTake("render-take"); take->SetNumBusChannels(1u); take->AddLoop(MakeLoop()); take->CommitChanges();
 Check(take,vst::RackMove::Scope::Take,[&] { sink->First = 0.0f; take->Zero(8u,base::Audible::AUDIOSOURCE_LOOPS); take->WriteBlock(sink,nullptr,0,8u); return sink->First; });
 EXPECT_EQ(6u,sink->Writes);
}
