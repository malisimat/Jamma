
#include "gtest/gtest.h"
#include "actions/JobAction.h"
#include "engine/Loop.h"
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
