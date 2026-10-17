#include "gtest/gtest.h"
#include "engine/Station.h"
#include "engine/LoopTake.h"
#include "engine/Loop.h"
#include <algorithm>
#include "../../TestPaths.h"
#include "utils/StringUtils.h"

class VstRackLoadTest : public testing::Test
{
protected:
	std::filesystem::path Directory;
	void SetUp() override
	{
		Directory = TestPaths::FixtureDirectory() / ("vst-rack-" + utils::GetGuid());
		std::filesystem::create_directory(Directory);
	}
	void TearDown() override
	{
		std::error_code error;
		std::filesystem::remove(Directory, error);
	}
	std::wstring MissingPath(const wchar_t* name) const { return (Directory / name).wstring(); }
	static std::shared_ptr<engine::Station> Station()
	{
		engine::StationParams params;
		params.Name = "vst-rack-load";
		params.Size = { 200, 200 };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
	}
	static std::shared_ptr<engine::LoopTake> Take()
	{
		engine::LoopTakeParams params;
		params.Id = "vst-rack-take";
		params.Size = { 100, 100 };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<engine::LoopTake>(params, engine::LoopTake::GetMixerParams(params.Size, merge));
	}
	static std::shared_ptr<engine::Loop> Loop()
	{
		engine::LoopParams params;
		params.Id = "vst-rack-loop";
		params.Size = { 80, 80 };
		audio::WireMixBehaviourParams wire;
		return std::make_shared<engine::Loop>(params, engine::Loop::GetMixerParams(params.Size, wire));
	}
	template<class Owner> void CheckFailedRestore(const std::shared_ptr<Owner>& owner)
	{
		const std::vector<std::uint8_t> state{1u, 4u, 9u};
		EXPECT_FALSE(owner->LoadVstPluginSynchronously(MissingPath(L"first.dll"), state, true, "failed-first"));
		EXPECT_FALSE(owner->LoadVstPluginSynchronously(MissingPath(L"second.dll"), {}, false, "failed-second"));
		const auto slot = owner->GetVstRackSlot();
		EXPECT_FALSE(slot.Pending);
		EXPECT_TRUE(slot.Failed);
		EXPECT_EQ(slot.Identity, "failed-first");
		EXPECT_FALSE(slot.Plugin);
		EXPECT_EQ(slot.Path, MissingPath(L"first.dll"));
		EXPECT_FALSE(owner->UnloadManagedVstPlugin("stale-identity"));
		EXPECT_FALSE(owner->UnloadManagedVstPlugin(""));
	}
	template<class Owner> void CheckRetryAndStaleUnload(const std::shared_ptr<Owner>& owner)
	{
		ASSERT_FALSE(owner->LoadVstPluginSynchronously(MissingPath(L"old.dll"), {}, false, "old-slot"));
		ASSERT_TRUE(owner->LoadManagedVstPlugin(MissingPath(L"retry.dll")));
		EXPECT_TRUE(owner->GetVstRackSlot().Pending);
		EXPECT_FALSE(owner->LoadManagedVstPlugin(L"duplicate.dll"));
		EXPECT_FALSE(owner->UnloadManagedVstPlugin("old-slot"));
		auto jobs = owner->CommitChanges();
		const auto load = std::find_if(jobs.begin(), jobs.end(), [](const auto& job) {
			return job.JobActionType == actions::JobAction::JOB_LOADVST;
		});
		ASSERT_NE(load, jobs.end());
		owner->OnAction(*load);
		EXPECT_TRUE(owner->GetVstRackSlot().Pending);
		// Completion waits for UI publication.
		owner->CommitChanges();
		const auto replacement = owner->GetVstRackSlot();
		EXPECT_FALSE(replacement.Pending);
		EXPECT_TRUE(replacement.Failed);
		EXPECT_NE(replacement.Identity, "old-slot");
		actions::JobAction stale;
		stale.JobActionType = actions::JobAction::JOB_UNLOADVST;
		stale.VstIndex = 0u;
		stale.VstIdentity = "old-slot";
		owner->OnAction(stale);
		owner->CommitChanges();
		EXPECT_EQ(owner->GetVstRackSlot().Identity, replacement.Identity);
		ASSERT_TRUE(owner->UnloadManagedVstPlugin(replacement.Identity));
		jobs = owner->CommitChanges();
		for (auto& job : jobs)
			if (job.JobActionType == actions::JobAction::JOB_UNLOADVST)
				owner->OnAction(job);
		owner->CommitChanges();
		EXPECT_TRUE(owner->GetVstRackSlot().Identity.empty());
		EXPECT_FALSE(owner->GetVstRackSlot().Pending);
	}
};

TEST_F(VstRackLoadTest, MissingPluginsKeepAlignedStateAndRecoverableSlot)
{
	auto station = Station();
	CheckFailedRestore(station);
	auto take = Take();
	CheckFailedRestore(take);
	auto loop = Loop();
	CheckFailedRestore(loop);
	for (const auto& entries : {station->VstEntries(), take->VstEntries(), loop->ToJamFile("test.wav").VstChain})
	{
		ASSERT_EQ(entries.size(), 2u);
		EXPECT_EQ(entries[0].Id, "failed-first");
		EXPECT_TRUE(entries[0].Bypass);
		EXPECT_EQ(entries[0].DecodeState(), (std::vector<std::uint8_t>{1u, 4u, 9u}));
		EXPECT_EQ(entries[1].Id, "failed-second");
	}
}

TEST_F(VstRackLoadTest, AsyncFailureCompletesAndStaleRemovalCannotRemoveRetry)
{
	CheckRetryAndStaleUnload(Station());
	CheckRetryAndStaleUnload(Take());
	CheckRetryAndStaleUnload(Loop());
}

TEST_F(VstRackLoadTest, SeveralJobsBeforePublicationComposeRatherThanOverwrite)
{
	auto station = Station();
	station->LoadVstPlugin(MissingPath(L"one.dll"));
	station->LoadVstPlugin(MissingPath(L"two.dll"));
	const auto jobs = station->CommitChanges();
	for (auto& job : jobs)
		if (job.JobActionType == actions::JobAction::JOB_LOADVST)
			station->OnAction(job);
	station->CommitChanges();
	auto entries = station->VstEntries();
	ASSERT_EQ(entries.size(), 2u);
	station->UnloadVstPlugin(0u);
	station->UnloadVstPlugin(1u);
	const auto removals = station->CommitChanges();
	for (auto& job : removals)
		if (job.JobActionType == actions::JobAction::JOB_UNLOADVST)
			station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->VstEntries().empty());
	EXPECT_FALSE(station->GetVstRackSlot().Pending);
}
TEST_F(VstRackLoadTest, MissingTakePluginPreservesTheTakeDuringSessionRestore)
{
	engine::LoopTakeParams params;
	params.Id = "missing-plugin-take";
	params.Size = {100, 100};
	io::JamFile::LoopTake saved;
	saved.Name = "recover me";
	io::JamFile::VstEntry plugin;
	plugin.Path = utils::EncodeUtf8(MissingPath(L"absent.dll"));
	plugin.Id = "recoverable-take-slot";
	saved.VstChain.push_back(plugin);
	auto restored = engine::LoopTake::FromFile(params, saved, L"");
	ASSERT_TRUE(restored.has_value());
	EXPECT_TRUE(restored.value()->GetVstRackSlot().Failed);
	EXPECT_EQ(restored.value()->GetVstRackSlot().Identity, plugin.Id);
	EXPECT_EQ(restored.value()->VstEntries().size(), 1u);
}

TEST_F(VstRackLoadTest, GroupCaptureKeepsFailedSlotsWithTheirSavedState)
{
	auto station = Station();
	CheckFailedRestore(station);
	std::vector<io::JamFile::VstEntry> entries;
	ASSERT_TRUE(station->TryGroupVstEntries(entries));
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].Id, "failed-first");
	EXPECT_TRUE(entries[0].Bypass);
	EXPECT_EQ(entries[0].DecodeState(), (std::vector<std::uint8_t>{1u, 4u, 9u}));
}

TEST_F(VstRackLoadTest, RepeatedUnloadRequestsCompleteWithoutQueueDeduplication)
{
	auto station = Station();
	ASSERT_FALSE(station->LoadVstPluginSynchronously(MissingPath(L"old.dll"), {}, false, "remove-once"));
	station->UnloadVstPlugin(0u);
	station->UnloadVstPlugin(0u);
	const auto jobs = station->CommitChanges();
	std::vector<actions::JobAction> removals;
	for (const auto& job : jobs)
		if (job.JobActionType == actions::JobAction::JOB_UNLOADVST)
			removals.push_back(job);
	ASSERT_EQ(removals.size(), 2u);
	EXPECT_FALSE(removals[0] == removals[1]);
	for (auto& job : removals)
		station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->VstEntries().empty());
	EXPECT_FALSE(station->GetVstRackSlot().Pending);
}
