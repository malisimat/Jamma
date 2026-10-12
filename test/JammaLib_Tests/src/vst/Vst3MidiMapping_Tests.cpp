#include "gtest/gtest.h"
#include "vst/Vst3MidiMapping.h"

using vst::Vst3MidiMapping::AfterTouchControllerNumber;
using vst::Vst3MidiMapping::ControllerValue;
using vst::Vst3MidiMapping::MakeEmptyTable;
using vst::Vst3MidiMapping::ParameterTableMailbox;
using vst::Vst3MidiMapping::PitchBendControllerNumber;
using vst::Vst3MidiMapping::TryClassify;
using vst::Vst3MidiMapping::TryLookup;

static midi::MidiEvent MakeEvent(std::uint8_t status, std::uint8_t channel,
	std::uint8_t data1, std::uint8_t data2)
{
	return { 0u, static_cast<std::uint8_t>(status | channel), data1, data2, 0u };
}

TEST(Vst3MidiMapping, ClassifiesSupportedControllers)
{
	const auto assertClassified = [](const midi::MidiEvent& event,
		std::uint8_t controller, float value)
	{
		ControllerValue result;
		ASSERT_TRUE(TryClassify(event, result));
		EXPECT_EQ(result.ControllerNumber, controller);
		EXPECT_NEAR(result.NormalizedValue, value, 1e-5f);
	};

	assertClassified(MakeEvent(0xB0, 3, 74, 127), 74u, 1.0f);
	assertClassified(MakeEvent(0xD0, 5, 127, 0), AfterTouchControllerNumber, 1.0f);
	assertClassified(MakeEvent(0xE0, 0, 0, 0x40), PitchBendControllerNumber,
		8192.0f / 16383.0f);
}

TEST(Vst3MidiMapping, LooksUpOnlyMappedControllers)
{
	auto table = MakeEmptyTable();
	table[2][74] = 12345u;
	std::uint32_t paramId = 0u;

	EXPECT_TRUE(TryLookup(table, 2, 74, paramId));
	EXPECT_EQ(paramId, 12345u);
	EXPECT_FALSE(TryLookup(table, 2, 75, paramId));
}

TEST(Vst3MidiMapping, MailboxPublishesOnlyAfterConsumerAdopts)
{
	ParameterTableMailbox mailbox;
	auto* writeSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(writeSlot, nullptr);
	(*writeSlot)[0][1] = 999u;
	mailbox.Publish();
	EXPECT_EQ(mailbox.AcquireWriteSlot(), nullptr);

	int adoptedSlot = 0;
	mailbox.AdoptPending(adoptedSlot);
	std::uint32_t paramId = 0u;
	ASSERT_TRUE(TryLookup(mailbox.Slot(adoptedSlot), 0, 1, paramId));
	EXPECT_EQ(paramId, 999u);
	EXPECT_NE(mailbox.AcquireWriteSlot(), nullptr);
}

#ifdef JAMMA_VST3_ENABLED
#include "vst/Vst3Plugin.h"

TEST(Vst3PluginMidi, ReservesReleasesAndRejectsOutsideWindowBeforeCapacity)
{
	vst::Vst3Plugin plugin;
	plugin.BeginMidiBlock(0u, 128u);
	const auto attack = midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	for (unsigned int i = 0u; i < 192u; ++i)
		ASSERT_TRUE(plugin.TrySendMidiEvent(attack, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(attack, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(midi::MidiEvent::MakeNoteOff(128u, 0u, 60u), false));
	const auto release = midi::MidiEvent::MakeNoteOff(0u, 0u, 60u);
	for (unsigned int i = 0u; i < 64u; ++i)
		ASSERT_TRUE(plugin.TrySendMidiEvent(release, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(release, false));
	plugin.BeginMidiBlock(128u, 128u);
	EXPECT_TRUE(plugin.TrySendMidiEvent(midi::MidiEvent::MakeNoteOff(128u, 0u, 60u), false));
}
#endif
