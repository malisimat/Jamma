#include <unordered_set>

#include "gtest/gtest.h"
#include "midi/MidiDevice.h"

using midi::MidiDevice;

TEST(MidiDevice, IsClosedBeforeOpen) {
	MidiDevice device;
	ASSERT_FALSE(device.IsOpen());
}

TEST(MidiDevice, CloseOnUnopenedDeviceIsSafe) {
	MidiDevice device;
	ASSERT_NO_THROW(device.Close());
	ASSERT_FALSE(device.IsOpen());
}

// When a name has no match, Open() falls back to the first available device (returns true)
// or returns false if no devices exist at all. Either way, the return value must match IsOpen().
TEST(MidiDevice, OpenWithUnknownNameIsConsistent) {
	MidiDevice device;
	auto result = device.Open("__jamma_bogus_device_xyzzy_12345__",
		[](std::uint8_t, std::uint8_t, std::uint8_t) {});
	ASSERT_EQ(result, device.IsOpen());
	device.Close();
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
