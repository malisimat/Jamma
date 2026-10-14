#include <future>
#include <thread>
#include <algorithm>
#include <unordered_set>

#include "gtest/gtest.h"
#include "midi/MidiDevice.h"
#include "midi/MidiRouter.h"
#include "io/UserConfig.h"

using midi::MidiDevice;

TEST(MidiDevice, SuccessfulConnectionDeltaIgnoresEnumerationReordering)
{
	midi::MidiConnectionResult previous, refreshed;
	previous.Connected = { { 1u, "Pad", "Pad 1" }, { 2u, "Keys", "Keys 2" } };
	refreshed.Connected = { { 0u, "Keys", "Keys 0" }, { 4u, "Pad", "Pad 4" } };
	EXPECT_TRUE(MidiDevice::LostConnectedInputNames(previous, refreshed).empty());
}

TEST(MidiDevice, SuccessfulConnectionDeltaUsesActualOpenedEndpoints)
{
	midi::MidiConnectionResult previous, refreshed;
	previous.Connected = { { 1u, "Pad", "Pad 1" }, { 2u, "Keys", "Keys 2" } };
	refreshed.Inventory.Devices = previous.Connected;
	refreshed.Connected = { previous.Connected[1] };
	EXPECT_EQ((std::vector<std::string>{ "Pad" }),
		MidiDevice::LostConnectedInputNames(previous, refreshed));
	refreshed.Inventory.Error = "transient inventory failure";
	EXPECT_TRUE(MidiDevice::LostConnectedInputNames(previous, refreshed).empty());
}

TEST(MidiDevice, SuccessfulConnectionDeltaCoalescesDuplicateNamedEndpointLoss)
{
	midi::MidiConnectionResult previous, refreshed;
	previous.Connected = { { 1u, "Pad", "Pad 1" }, { 2u, "Pad", "Pad 2" } };
	refreshed.Connected = { previous.Connected[1] };
	EXPECT_EQ((std::vector<std::string>{ "Pad" }),
		MidiDevice::LostConnectedInputNames(previous, refreshed));
}

TEST(MidiDevice, IsClosedBeforeOpen) {
	MidiDevice device;
	ASSERT_FALSE(device.IsOpen());
}

TEST(MidiDevice, CloseOnUnopenedDeviceIsSafe) {
	MidiDevice device;
	ASSERT_NO_THROW(device.Close());
	ASSERT_FALSE(device.IsOpen());
}

TEST(MidiDevice, OpenWithUnknownNameIsConsistent) {
	MidiDevice device;
	auto result = device.Open("__jamma_bogus_device_xyzzy_12345__",
		[](std::uint8_t, std::uint8_t, std::uint8_t, double, std::int64_t) {});
	ASSERT_FALSE(result);
	ASSERT_FALSE(device.IsOpen());
	device.Close();
}

TEST(MidiDevice, SelectsExactUnclaimedPortAndReportsDuplicateNames) {
	const std::vector<midi::MidiInputDeviceInfo> ports{ { 2u, "Pad" }, { 5u, "Pad" }, { 7u, "Keys" } };
	bool ambiguous = false;
	const auto* first = MidiDevice::SelectUnclaimedExactInput(ports, "Pad", {}, ambiguous);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(2u, first->DeviceId);
	EXPECT_TRUE(ambiguous);
	const auto* second = MidiDevice::SelectUnclaimedExactInput(ports, "Pad", { 2u }, ambiguous);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(5u, second->DeviceId);
	EXPECT_TRUE(ambiguous);
	EXPECT_EQ(nullptr, MidiDevice::SelectUnclaimedExactInput(ports, "Pad", { 2u, 5u }, ambiguous));
	EXPECT_EQ(nullptr, MidiDevice::SelectUnclaimedExactInput(ports, "pad", {}, ambiguous));
	EXPECT_FALSE(ambiguous);
}

TEST(MidiDevice, RemovesOnlyTheCurrentWinMmPortSuffix) {
	EXPECT_EQ("CASIO USB-MIDI", MidiDevice::BaseInputName("CASIO USB-MIDI 3", 3u));
	EXPECT_EQ("Launchkey 49", MidiDevice::BaseInputName("Launchkey 49 3", 3u));
	EXPECT_EQ("Launchkey 49", MidiDevice::BaseInputName("Launchkey 49", 3u));
}

TEST(MidiDevice, MigratesLegacyIndexedNameWhenBasePortAppears) {
	const std::vector<midi::MidiInputDeviceInfo> ports{ { 1u, "CASIO USB-MIDI", "CASIO USB-MIDI 1" } };
	EXPECT_EQ("CASIO USB-MIDI", MidiDevice::ResolveSavedInputName("CASIO USB-MIDI 0", ports));
	EXPECT_EQ("CASIO USB-MIDI", MidiDevice::ResolveSavedInputName("CASIO USB-MIDI", ports));
	EXPECT_EQ("Missing 0", MidiDevice::ResolveSavedInputName("Missing 0", ports));
}

TEST(MidiDevice, GeneratedStartupSelectsEveryPhysicalPortWithDuplicateNames) {
	const std::vector<midi::MidiInputDeviceInfo> ports{ { 2u, "Pad" }, { 5u, "Pad" }, { 7u, "Keys" } };
	bool ambiguous = false;
	const auto* first = MidiDevice::SelectStartupInput(ports, "Pad", {}, 0u, true, ambiguous);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(2u, first->DeviceId);
	EXPECT_TRUE(ambiguous);
	const auto* second = MidiDevice::SelectStartupInput(ports, "Pad", { 2u }, 1u, true, ambiguous);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(5u, second->DeviceId);
	EXPECT_TRUE(ambiguous);
	const auto* named = MidiDevice::SelectStartupInput(ports, "Keys", { 2u, 5u }, 2u, true, ambiguous);
	ASSERT_NE(nullptr, named);
	EXPECT_EQ(7u, named->DeviceId);
	EXPECT_FALSE(ambiguous);
	EXPECT_EQ(nullptr, MidiDevice::SelectStartupInput(ports, "Pad", {}, 3u, true, ambiguous));
}

TEST(MidiDevice, EnumeratesInputDevices) {
	std::vector<midi::MidiInputDeviceInfo> devices;
	ASSERT_NO_THROW(devices = MidiDevice::EnumerateInputDevices());

	std::unordered_set<unsigned int> ids;
	for (const auto& d : devices)
	{
		ASSERT_TRUE(ids.insert(d.DeviceId).second);
	}
}

TEST(MidiDevice, PersistedDuplicateNamesNeverGuessAnEnumerationIndex) {
	std::vector<midi::MidiInputDeviceInfo> ports{ { 2u, "Pad" }, { 5u, "Pad" } };
	bool ambiguous = false;
	EXPECT_EQ(nullptr, MidiDevice::SelectStartupInput(ports, "Pad", {}, 0u, false, ambiguous));
	EXPECT_TRUE(ambiguous);
	std::reverse(ports.begin(), ports.end());
	EXPECT_EQ(nullptr, MidiDevice::SelectStartupInput(ports, "Pad", {}, 0u, false, ambiguous));
	EXPECT_TRUE(ambiguous);
	EXPECT_EQ("Pad", MidiDevice::ResolveSavedInputName("Pad 2", ports));
	EXPECT_EQ(nullptr, MidiDevice::SelectStartupInput(ports,
		MidiDevice::ResolveSavedInputName("Pad 2", ports), {}, 0u, false, ambiguous));
}

TEST(MidiDevice, ReconciliationRetainsAcceptedSelectionAcrossInventoryFailure) {
	midi::MidiRouter router;
	io::UserConfig config;
	config.Midi.Devices = { { "Missing keys", true }, { "Disabled pad", false } };
	midi::MidiClockAnchor clock;
	midi::MidiInputInventory inventory;
	const auto accepted = router.InitMidi(config, {}, clock, 48000u, &inventory);
	ASSERT_TRUE(accepted.Accepted);
	ASSERT_EQ(2u, accepted.Attempts.size());
	EXPECT_EQ(midi::MidiConnectionStatus::Missing, accepted.Attempts[0].Status);
	EXPECT_EQ(midi::MidiConnectionStatus::Disabled, accepted.Attempts[1].Status);
	inventory.Error = "inventory unavailable";
	config.Midi.Devices.clear();
	const auto failed = router.InitMidi(config, {}, clock, 48000u, &inventory);
	EXPECT_FALSE(failed.Accepted);
	ASSERT_EQ(2u, failed.Attempts.size());
	EXPECT_EQ("Missing keys", failed.Attempts[0].RequestedName);
	EXPECT_EQ("inventory unavailable", failed.Inventory.Error);
	router.SuspendMidiDispatch();
	router.SuspendMidiDispatch();
	router.ResumeMidiDispatch();
	router.ResumeMidiDispatch();
}

TEST(MidiDevice, ReconciliationReportsAmbiguousWithoutOpeningEitherPort) {
	midi::MidiRouter router;
	io::UserConfig config;
	config.Midi.Devices = { { "Pad", true } };
	midi::MidiClockAnchor clock;
	midi::MidiInputInventory inventory;
	inventory.Devices = { { 2u, "Pad", "Pad 2" }, { 5u, "Pad", "Pad 5" } };
	const auto result = router.InitMidi(config, {}, clock, 48000u, &inventory);
	ASSERT_TRUE(result.Accepted);
	ASSERT_EQ(1u, result.Attempts.size());
	EXPECT_EQ(midi::MidiConnectionStatus::Ambiguous, result.Attempts[0].Status);
	EXPECT_TRUE(result.Connected.empty());
}

namespace midi
{
	class MidiDeviceCallbackTestAccess
	{
	public:
		static void OpenGate(MidiDevice& device) { device._callbackState.store(0u); }
		static bool Enter(MidiDevice& device) { return device.TryEnterCallback(); }
		static void Leave(MidiDevice& device) { device.LeaveCallback(); }
		static bool Closed(MidiDevice& device) { return (device._callbackState.load() & MidiDevice::CallbackClosed) != 0u; }
	};
}
TEST(MidiDeviceTests, CallbackCloseSharesAdmissionWordAndDrainsReader)
{
	midi::MidiDevice device;
	midi::MidiDeviceCallbackTestAccess::OpenGate(device);
	ASSERT_TRUE(midi::MidiDeviceCallbackTestAccess::Enter(device));
	auto suspended = std::async(std::launch::async, [&] {device.SuspendCallbacks();});
	while (!midi::MidiDeviceCallbackTestAccess::Closed(device)) std::this_thread::yield();
	EXPECT_FALSE(midi::MidiDeviceCallbackTestAccess::Enter(device));
	EXPECT_EQ(suspended.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
	midi::MidiDeviceCallbackTestAccess::Leave(device);
	EXPECT_EQ(suspended.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	suspended.get();
	EXPECT_FALSE(midi::MidiDeviceCallbackTestAccess::Enter(device));
}
