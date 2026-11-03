#include "gtest/gtest.h"
#include "engine/Station.h"
#include "engine/LoopTake.h"
#include "engine/Loop.h"
#include <algorithm>
#include <fstream>
#include "io/WavReadWriter.h"
#include "io/NativeMidiSidecar.h"
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
		std::filesystem::remove(Directory / "routing.wav", error);
		std::filesystem::remove(Directory / "routing.jammidi", error);
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
	const auto slots = station->GetVstRackSlots();
	ASSERT_EQ(slots.size(), 2u);
	EXPECT_EQ(slots[0].Identity, "failed-first");
	EXPECT_EQ(slots[1].Identity, "failed-second");
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
	saved.MidiChannelRoutes[3] = {{"unavailable-slot"}};
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
	EXPECT_EQ((std::vector<std::string>{"unavailable-slot"}), station->MidiChannelRoutes()[3].PluginIds);
	ASSERT_TRUE(station->LoadManagedVstSlot(MissingPath(L"retry.dll"), "unavailable-slot"));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->GetVstRackSlots()[0].Failed);
	EXPECT_EQ((std::vector<std::string>{"unavailable-slot"}), station->MidiChannelRoutes()[3].PluginIds);
	ASSERT_TRUE(station->UnloadManagedVstPlugin("unavailable-slot"));
	for (const auto& job : station->CommitChanges()) station->OnAction(job);
	station->CommitChanges();
	EXPECT_TRUE(station->GetVstRackSlots().empty());
	EXPECT_TRUE(station->MidiChannelRoutes()[3].PluginIds.empty());
}

TEST_F(VstRackLoadTest, InactiveRackPreservesRawOrderRoutesAndExplicitRecovery)
{
	io::JamFile::Station saved;
	saved.Id = "raw-rack"; saved.Name = "raw rack"; saved.StationType = 0;
	saved.MidiChannelRoutes[0] = {{"missing", "slot-16"}};
	saved.MidiChannelRoutes[1] = {{}};
	saved.MidiChannelRoutes[15] = {{"slot-16", "slot-17"}};
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
	EXPECT_FALSE(station->RecoverRack(station->RackRevision()));
	EXPECT_EQ(33u, station->VstEntries().size());
	ASSERT_TRUE(station->RemoveInactiveRackSlot(16, station->RackRevision()));
	EXPECT_FALSE(station->RemoveInactiveRackSlot(16, station->RackRevision() - 1u));
	saved.VstChain = station->VstEntries();
	saved.MidiChannelRoutes = station->MidiChannelRoutes();
	EXPECT_EQ("slot-17", saved.VstChain[16].Id);
	EXPECT_EQ((std::vector<std::string>{"missing"}), saved.MidiChannelRoutes[0].PluginIds);
	EXPECT_TRUE(saved.MidiChannelRoutes[1].PluginIds.empty());
	EXPECT_EQ((std::vector<std::string>{"slot-17"}), saved.MidiChannelRoutes[15].PluginIds);
	io::JamFile jam; jam.Name = "rack-roundtrip"; jam.Stations.push_back(saved);
	std::stringstream encoded;
	ASSERT_TRUE(io::JamFile::ToStream(jam, encoded));
	auto parsed = io::JamFile::FromStream(std::move(encoded));
	ASSERT_TRUE(parsed);
	ASSERT_EQ(1u, parsed->Stations.size());
	const auto& roundTrip = parsed->Stations[0];
	ASSERT_EQ(32u, roundTrip.VstChain.size());
	EXPECT_EQ("slot-17", roundTrip.VstChain[16].Id);
	EXPECT_EQ(saved.MidiChannelRoutes, roundTrip.MidiChannelRoutes);
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
}



TEST_F(VstRackLoadTest, ChildRestoreKeepsIndependentMasksAndExplicitEmpty)
{
	io::WavReadWriter wav;
	ASSERT_TRUE(wav.Write((Directory / "routing.wav").wstring(), std::vector<float>(64u, 0.0f), 64u, 48000u));
	io::JamFile::Loop leaf{};
	leaf.Id = "routing-leaf";
	leaf.Name = "routing.wav";
	leaf.Length = 64ul;
	leaf.Level = 1.0;
	leaf.Speed = 1.0;
	leaf.Mix = {io::JamFile::LoopMix::MIX_WIRE, std::vector<unsigned long>{0ul}};
	leaf.AllowedMidiChannels = {1,6};
	leaf.MidiChannelRoutes[5].PluginIds = {"unavailable-leaf"};
	io::JamFile::LoopTake saved{};
	saved.Name = "routing-take";
	saved.Id = "routing-take";
	saved.AllowedMidiChannels = {2,3};
	saved.MidiChannelRoutes[1].PluginIds = {"unavailable-take"};
	saved.Loops.push_back(leaf);
	for (const bool disabled : {false,true})
	{
		auto descriptor = saved;
		if (disabled)
		{
			descriptor.AllowedMidiChannels.clear();
			descriptor.Loops[0].AllowedMidiChannels.clear();
		}
		engine::LoopTakeParams params;
		params.Id = descriptor.Id;
		params.Size = {100u,100u};
		const auto restored = engine::LoopTake::FromFile(params, descriptor, Directory.wstring());
		ASSERT_TRUE(restored);
		const auto take = *restored;
		take->CommitChanges();
		ASSERT_EQ(1u, take->GetLoops().size());
		const auto loop = take->GetLoops()[0];
		EXPECT_EQ(disabled ? std::vector<int>{} : std::vector<int>({2,3}), take->AllowedMidiChannels());
		EXPECT_EQ(disabled ? std::vector<int>{} : std::vector<int>({1,6}), loop->AllowedMidiChannels());
		EXPECT_EQ(saved.MidiChannelRoutes, take->MidiChannelRoutes());
		EXPECT_EQ(leaf.MidiChannelRoutes, loop->MidiChannelRoutes());
	}
}

TEST_F(VstRackLoadTest, PureMidiTakeRestoresLocalRoutingWithoutAudioLeaves)
{
	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 64u;
	stream.Events = {{0u,0x91u,60u,100u,0u},{32u,0x81u,60u,0u,0u}};
	{
		std::ofstream output(Directory / "routing.jammidi", std::ios::binary);
		ASSERT_TRUE(io::NativeMidiSidecar::ToStream(stream, output));
	}
	io::JamFile::LoopTake descriptor{};
	descriptor.Name = "midi-only";
	descriptor.Id = "midi-only";
	descriptor.MidiPlayLength = 64ul;
	descriptor.MidiStreams.push_back({"routing.jammidi",1u,"test-owned",64ul,0u});
	descriptor.AllowedMidiChannels = {2,3};
	descriptor.MidiChannelRoutes[1].PluginIds = {"unavailable-instrument"};
	engine::LoopTakeParams params;
	params.Id = descriptor.Id;
	params.Size = {100u,100u};
	const auto restored = engine::LoopTake::FromFile(params, descriptor, Directory.wstring());
	ASSERT_TRUE(restored);
	const auto take = *restored;
	take->CommitChanges();
	EXPECT_TRUE(take->GetLoops().empty());
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	EXPECT_EQ(2u, take->GetMidiLoops()[0]->EventCount());
	EXPECT_EQ(descriptor.AllowedMidiChannels, take->AllowedMidiChannels());
	EXPECT_EQ(descriptor.MidiChannelRoutes, take->MidiChannelRoutes());
}

TEST_F(VstRackLoadTest, InvalidChildRoutingRejectsRestoration)
{
	for (const bool atLeaf : {false,true})
	for (auto scenario = 0u; scenario < 5u; ++scenario)
	{
		SCOPED_TRACE(atLeaf);
		SCOPED_TRACE(scenario);
		io::JamFile::Loop leaf{};
		io::JamFile::LoopTake descriptor{};
		descriptor.Id = "invalid-routing-take";
		descriptor.Name = "invalid-routing-take";
		auto& entries = atLeaf ? leaf.VstChain : descriptor.VstChain;
		auto& routes = atLeaf ? leaf.MidiChannelRoutes : descriptor.MidiChannelRoutes;
		entries.push_back({utils::EncodeUtf8(MissingPath(L"absent.dll")),false,{},"slot"});
		if (scenario == 0u) entries[0].Id = std::string("bad\0id",6u);
		if (scenario == 1u) entries.resize(33u,entries[0]);
		if (scenario == 2u) routes[0].PluginIds = {""};
		if (scenario == 3u) routes[0].PluginIds.resize(33u,"missing");
		if (scenario == 4u) routes[0].PluginIds = {"duplicate","duplicate"};
		if (atLeaf) descriptor.Loops.push_back(leaf);
		engine::LoopTakeParams params;
		params.Id = descriptor.Id;
		params.Size = {100u,100u};
		EXPECT_FALSE(engine::LoopTake::FromFile(params, descriptor, Directory.wstring()));
	}
}
