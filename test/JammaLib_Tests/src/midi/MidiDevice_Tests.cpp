#include <atomic>
#include <cstdlib>
#include <thread>
#include <unordered_set>

#include "gtest/gtest.h"
#include "midi/MidiDevice.h"

using audio::MidiDevice;

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
	std::vector<audio::MidiInputDeviceInfo> devices;
	ASSERT_NO_THROW(devices = MidiDevice::EnumerateInputDevices());

	std::unordered_set<unsigned int> ids;
	for (const auto& d : devices)
	{
		ASSERT_TRUE(ids.insert(d.DeviceId).second);
	}
}

TEST(MidiDevice, OpensPreferredDeviceWhenAvailable) {
	char* envVal = nullptr;
	size_t envLen = 0;
	_dupenv_s(&envVal, &envLen, "JAMMA_ENABLE_MIDI_HARDWARE_TESTS");
	bool hasEnvVar = (envVal != nullptr);
	free(envVal);
	if (!hasEnvVar)
		GTEST_SKIP() << "Set JAMMA_ENABLE_MIDI_HARDWARE_TESTS=1 to run MIDI hardware tests.";

	auto devices = MidiDevice::EnumerateInputDevices();
	if (devices.empty())
		GTEST_SKIP() << "No MIDI devices available on this machine.";

	MidiDevice device;
	std::atomic<unsigned int> callbackCount{ 0u };

	auto opened = device.Open(devices.front().Name,
		[&callbackCount](std::uint8_t, std::uint8_t, std::uint8_t)
		{
			callbackCount.fetch_add(1u);
		});

	ASSERT_TRUE(opened);
	ASSERT_TRUE(device.IsOpen());
	ASSERT_EQ(devices.front().Name, device.DeviceName());

	// Let the callback thread spin briefly so connection can be validated in logs.
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	device.Close();
	ASSERT_FALSE(device.IsOpen());
}
