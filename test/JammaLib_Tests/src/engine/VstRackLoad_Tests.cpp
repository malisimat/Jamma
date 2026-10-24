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
	CheckRetryAndStaleUnload(Take());
	CheckRetryAndStaleUnload(Loop());
}

TEST_F(VstRackLoadTest, StationRetryPreservesIdentityStateBypassAndPendingRemoval)
{
	auto station = Station();
	const std::vector<std::uint8_t> state{1, 4, 9};
	ASSERT_FALSE(station->LoadVstPluginSynchronously(MissingPath(L"old.dll"), state, true, "stable-slot"));
	ASSERT_TRUE(station->LoadManagedVstSlot(MissingPath(L"retry.dll"), "stable-slot"));
	EXPECT_FALSE(station->LoadManagedVstSlot(MissingPath(L"duplicate.dll"), "stable-slot"));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	const auto entries = station->VstEntries();
	ASSERT_EQ(1u, entries.size());
	EXPECT_EQ("stable-slot", entries[0].Id);
	EXPECT_TRUE(entries[0].Bypass);
	EXPECT_EQ(state, entries[0].DecodeState());
	ASSERT_TRUE(station->LoadManagedVstSlot(MissingPath(L"cancelled.dll")));
	const auto pending = station->GetVstRackSlots().back();
	EXPECT_TRUE(pending.Pending);
	ASSERT_TRUE(station->UnloadManagedVstPlugin(pending.Identity));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_EQ(1u, station->GetVstRackSlots().size());
}

TEST_F(VstRackLoadTest, FailedInteractiveAppendsRestoreEmptyRackAndLeaveOnlyDialogOutcomes)
{
	auto station = Station();
	const auto first = MissingPath(L"invalid-first.vst3");
	const auto second = MissingPath(L"invalid-second.vst3");
	ASSERT_TRUE(station->LoadManagedVstSlot(first, {}, true));
	ASSERT_TRUE(station->LoadManagedVstSlot(second, {}, true));
	const auto pending = station->GetVstRackSlots();
	ASSERT_EQ(2u, pending.size());
	const auto firstId = pending[0].Identity, secondId = pending[1].Identity;
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->GetVstRackSlots().empty());
	EXPECT_TRUE(station->VstEntries().empty());
	EXPECT_TRUE(station->RackDiagnostic().empty());
	EXPECT_EQ(station->TakeFailedInteractiveVstLoad(firstId),first);
	EXPECT_EQ(station->TakeFailedInteractiveVstLoad(secondId),second);
	EXPECT_FALSE(station->TakeFailedInteractiveVstLoad(firstId).has_value());
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

TEST_F(VstRackLoadTest, LoadedMissingStationCableStaysRecoverableUntilRemoved)
{
	io::JamFile::Station saved;
	saved.Id = "recoverable-station";
	saved.Name = "recoverable station";
	saved.StationType = 0;
	saved.HasLiveVstRoutes = true;
	saved.LiveVstRoutes[3] = {true, {"unavailable-slot"}};
	io::JamFile::VstEntry entry;
	entry.Id = "unavailable-slot";
	entry.Path = utils::EncodeUtf8(MissingPath(L"unavailable.dll"));
	saved.VstChain.push_back(entry);
	engine::StationParams params;
	params.Name = saved.Name;
	params.Size = {200, 200};
	audio::MergeMixBehaviourParams merge;
	auto restored = engine::Station::FromFile(params,
		engine::Station::GetMixerParams(params.Size, merge), std::move(saved), L"");
	ASSERT_TRUE(restored);
	auto station = *restored;
	ASSERT_EQ(1u, station->GetVstRackSlots().size());
	EXPECT_TRUE(station->GetVstRackSlots()[0].Failed);
	EXPECT_EQ((std::vector<std::string>{"unavailable-slot"}), station->LiveVstChannelRoutes()[3].PluginIds);
	ASSERT_TRUE(station->LoadManagedVstSlot(MissingPath(L"retry.dll"), "unavailable-slot"));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->GetVstRackSlots()[0].Failed);
	EXPECT_EQ((std::vector<std::string>{"unavailable-slot"}), station->LiveVstChannelRoutes()[3].PluginIds);
	ASSERT_TRUE(station->UnloadManagedVstPlugin("unavailable-slot"));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->GetVstRackSlots().empty());
	EXPECT_TRUE(station->LiveVstChannelRoutes()[3].Explicit);
	EXPECT_TRUE(station->LiveVstChannelRoutes()[3].PluginIds.empty());
}

TEST_F(VstRackLoadTest, InactiveRackPreservesRawOrderRoutesPolicyAndExplicitRecovery)
{
	io::JamFile::Station saved;
	saved.Id = "raw-rack"; saved.Name = "raw rack"; saved.StationType = 0;
	saved.HasLiveVstRoutes = true;
	saved.LiveVstRoutes[0] = {true, {"missing", "slot-16"}};
	saved.LiveVstRoutes[1] = {true, {}};
	saved.LiveVstRoutes[15] = {true, {"slot-16", "slot-17"}};
	for (auto index = 0u; index < 33u; ++index)
	{
		io::JamFile::VstEntry entry;
		entry.Id = "slot-" + std::to_string(index);
		entry.Path = utils::EncodeUtf8(MissingPath(L"preserved.dll"));
		entry.State = io::JamFile::VstEntry::EncodeState({1, 4, 9});
		saved.VstChain.push_back(entry);
	}
	engine::StationParams params; params.Name = saved.Name; params.Size = {200, 200};
	audio::MergeMixBehaviourParams merge;
	const auto reconstruct = [&](io::JamFile::Station value) { return engine::Station::FromFile(params, engine::Station::GetMixerParams(params.Size, merge), std::move(value), L""); };
	auto restored = reconstruct(saved);
	ASSERT_TRUE(restored);
	auto station = *restored;
	EXPECT_TRUE(station->RackInactive());
	EXPECT_EQ(33u, station->RackSlotCount());
	EXPECT_EQ(32u, station->GetVstRackSlots(0, 100000).size());
	EXPECT_EQ(1u, station->GetVstRackSlots(32, 32).size());
	EXPECT_EQ(vst::VstChain::RenderPolicy::LegacySerial, station->RackRenderPolicy());
	EXPECT_FALSE(station->RecoverRack(station->RackRevision()));
	EXPECT_EQ(33u, station->VstEntries().size());
	midi::MidiVstRoutingSnapshot indexedRoutes;
	indexedRoutes.LivePlugin = 16u;
	indexedRoutes.PluginByMidiOutput = {16u, 17u, midi::MidiVstRoutingSnapshot::MissingPlugin};
	ASSERT_TRUE(station->RestoreMidiVstRoutes(indexedRoutes, station->RackSlotCount()));
	ASSERT_TRUE(station->RemoveInactiveRackSlot(16, station->RackRevision()));
	EXPECT_FALSE(station->RemoveInactiveRackSlot(16, station->RackRevision() - 1u));
	saved.VstChain = station->VstEntries();
	saved.LiveVstRoutes = station->LiveVstChannelRoutes();
	EXPECT_EQ("slot-17", saved.VstChain[16].Id);
	EXPECT_EQ((std::vector<std::string>{"missing"}), saved.LiveVstRoutes[0].PluginIds);
	EXPECT_TRUE(saved.LiveVstRoutes[1].Explicit);
	EXPECT_TRUE(saved.LiveVstRoutes[1].PluginIds.empty());
	EXPECT_EQ((std::vector<std::string>{"slot-17"}), saved.LiveVstRoutes[15].PluginIds);
	const auto compactedRoutes = station->SnapshotMidiVstRoutesForExport();
	EXPECT_EQ(midi::MidiVstRoutingSnapshot::MissingPlugin, compactedRoutes.LivePlugin);
	ASSERT_EQ(3u, compactedRoutes.PluginByMidiOutput.size());
	EXPECT_EQ(midi::MidiVstRoutingSnapshot::MissingPlugin, compactedRoutes.PluginByMidiOutput[0]);
	EXPECT_EQ(16u, compactedRoutes.PluginByMidiOutput[1]);
	EXPECT_EQ(midi::MidiVstRoutingSnapshot::MissingPlugin, compactedRoutes.PluginByMidiOutput[2]);
	io::JamFile jam; jam.Name = "rack-roundtrip"; jam.Stations.push_back(saved);
	std::stringstream encoded;
	ASSERT_TRUE(io::JamFile::ToStream(jam, encoded));
	auto parsed = io::JamFile::FromStream(std::move(encoded));
	ASSERT_TRUE(parsed);
	ASSERT_EQ(1u, parsed->Stations.size());
	const auto& roundTrip = parsed->Stations[0];
	ASSERT_EQ(32u, roundTrip.VstChain.size());
	EXPECT_EQ("slot-17", roundTrip.VstChain[16].Id);
	EXPECT_EQ(saved.LiveVstRoutes, roundTrip.LiveVstRoutes);
	EXPECT_TRUE(roundTrip.AllowedMidiChannels.empty());
	ASSERT_TRUE(station->RecoverRack(station->RackRevision()));
	EXPECT_FALSE(station->RackInactive());
	EXPECT_EQ(32u, station->GetVstRackSlots().size());
	auto malformed = saved;
	malformed.VstChain[0].Id = std::string("bad\0id", 6);
	auto bad = reconstruct(malformed);
	ASSERT_TRUE(bad);
	EXPECT_TRUE((*bad)->RackInactive());
	EXPECT_EQ(malformed.VstChain[0].Id, (*bad)->VstEntries()[0].Id);
	io::JamFile::Station empty; empty.Name = "empty"; empty.Id = "empty"; empty.StationType = 0;
	auto emptyStation = reconstruct(empty);
	ASSERT_TRUE(emptyStation);
	EXPECT_EQ(vst::VstChain::RenderPolicy::AdditiveInstruments, (*emptyStation)->RackRenderPolicy());
}

TEST_F(VstRackLoadTest, OlderSerialRackKeepsItsModeUntilExplicitConversion)
{
	io::JamFile::Station saved;
	saved.Id = "older-rack"; saved.Name = "older rack"; saved.StationType = 0;
	io::JamFile::VstEntry entry;
	entry.Id = "missing-effect";
	entry.Path = utils::EncodeUtf8(MissingPath(L"older.dll"));
	saved.VstChain.push_back(entry);
	engine::StationParams params; params.Name = saved.Name; params.Size = {200, 200};
	audio::MergeMixBehaviourParams merge;
	const auto restore = [&](io::JamFile::Station value) {
		return engine::Station::FromFile(params, engine::Station::GetMixerParams(params.Size, merge), std::move(value), L"");
	};
	auto restored = restore(saved);
	ASSERT_TRUE(restored);
	const auto station = *restored;
	EXPECT_EQ(vst::VstChain::RenderPolicy::LegacySerial, station->RackRenderPolicy());
	ASSERT_EQ(1u, station->GetVstRackSlots().size());
	EXPECT_TRUE(station->GetVstRackSlots()[0].Failed);
	EXPECT_TRUE(station->CanConvertRackToAdditive());
	ASSERT_TRUE(station->LoadManagedVstSlot(MissingPath(L"retry.dll"), "missing-effect"));
	EXPECT_FALSE(station->CanConvertRackToAdditive());
	EXPECT_FALSE(station->ConvertRackToAdditive(station->RackRevision()));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	// Job completion still needs UI publication before the chain can change mode.
	EXPECT_FALSE(station->CanConvertRackToAdditive());
	EXPECT_FALSE(station->ConvertRackToAdditive(station->RackRevision()));
	station->CommitChanges();
	EXPECT_TRUE(station->CanConvertRackToAdditive());
	const auto revision = station->RackRevision();
	EXPECT_TRUE(station->ConvertRackToAdditive(revision));
	EXPECT_EQ(vst::VstChain::RenderPolicy::AdditiveInstruments, station->RackRenderPolicy());
	EXPECT_FALSE(station->CanConvertRackToAdditive());
	EXPECT_FALSE(station->ConvertRackToAdditive(revision));
	EXPECT_EQ("missing-effect", station->VstEntries()[0].Id);

	saved.VstChain.push_back(entry); // Duplicate identities require explicit recovery first.
	auto malformed = restore(saved);
	ASSERT_TRUE(malformed);
	EXPECT_TRUE((*malformed)->RackInactive());
	EXPECT_FALSE((*malformed)->CanConvertRackToAdditive());
	EXPECT_FALSE((*malformed)->ConvertRackToAdditive((*malformed)->RackRevision()));
	EXPECT_EQ(vst::VstChain::RenderPolicy::LegacySerial, (*malformed)->RackRenderPolicy());
}
