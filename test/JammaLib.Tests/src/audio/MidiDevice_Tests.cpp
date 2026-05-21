#include <atomic>
#include <cstdlib>
#include <thread>
#include <unordered_set>

#include "gtest/gtest.h"
#include "audio/MidiDevice.h"

using audio::MidiDevice;

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
	if (nullptr == std::getenv("JAMMA_ENABLE_MIDI_HARDWARE_TESTS"))
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
